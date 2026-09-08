#!/usr/bin/env python3
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Measure chain-height RPC latency during inventory download and NOTFOUND fallback.

Run from the repository root with PYTHONPATH=test/functional and DASHD pointing
to the binary under test. See doc/benchmarking.md for methodology and examples.
"""

import json
import multiprocessing
import subprocess
import time
from pathlib import Path

from test_framework.authproxy import AuthServiceProxy
from test_framework.messages import CInv, msg_inv, msg_notfound
from test_framework.p2p import P2PInterface, p2p_lock
from test_framework.test_framework import BitcoinTestFramework


def query_rpc(url, stop, output):
    rpc = AuthServiceProxy(url, timeout=60)
    latencies = []
    error = None
    try:
        while not stop.is_set():
            start = time.perf_counter()
            rpc.getblockcount()
            latencies.append((time.perf_counter() - start) * 1000)
            stop.wait(0.001)
    except Exception as exc:
        error = str(exc)
    output.send((latencies, error))
    output.close()


class Peer(P2PInterface):
    def __init__(self):
        super().__init__()
        self.requested = 0

    def on_getdata(self, message):
        self.requested += len(message.inv)
        self.send_message(msg_notfound(message.inv))


class InventoryBenchmark(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [["-debug=0", "-logtimemicros=1"]]

    def add_options(self, parser):
        parser.add_argument("--batch", type=int, default=5000)
        parser.add_argument("--rounds", type=int, default=20)
        parser.add_argument("--peers", type=int, default=4)
        parser.add_argument("--sample", action="store_true", help="Capture a macOS sample trace (separate from timing comparisons)")
        parser.add_argument("--inv-type", type=int, choices=(6, 17, 18, 29, 31), default=6)

    def run_test(self):
        assert 1 <= self.options.batch <= 50000
        assert self.options.rounds > 0
        assert 1 <= self.options.peers <= 16
        node = self.nodes[0]
        self.generate(node, 1)
        if self.options.inv_type in (17, 18):
            while not node.mnsync("status")["IsBlockchainSynced"]:
                node.mnsync("next")
        node.logging([], ["all"])
        node.logging(["lock"], [])
        peers = [node.add_p2p_connection(Peer()) for _ in range(self.options.peers)]
        # Construct inventories before timing; transport serialization remains included.
        messages = [msg_inv([CInv(self.options.inv_type, 1 + r * self.options.batch + i)
                            for i in range(self.options.batch)])
                    for r in range(self.options.rounds)]
        # A separate process avoids measuring the P2P driver's Python GIL contention as RPC latency.
        context = multiprocessing.get_context("spawn")
        stop = context.Event()
        receive, output = context.Pipe(duplex=False)
        observer = context.Process(target=query_rpc, args=(node.url, stop, output))
        sample = None
        if self.options.sample:
            sample = subprocess.Popen(["sample", str(node.process.pid), "10", "1", "-file",
                                       str(Path(self.options.tmpdir) / "sample.txt")],
                                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        observer.start()
        start = time.perf_counter()
        try:
            for round_index, message in enumerate(messages, start=1):
                for peer in peers:
                    peer.send_message(message)
                # Drain this round before announcing more: otherwise outstanding requests can
                # activate the overload delay, which cannot expire while mocktime is fixed.
                self.wait_until(lambda: all(peer.requested >= round_index * self.options.batch for peer in peers))
                for peer in peers:
                    peer.sync_with_ping()
            for peer in peers:
                peer.sync_with_ping()
            expected = len(peers) * len(messages) * self.options.batch
            self.wait_until(lambda: sum(peer.requested for peer in peers) >= expected)
        finally:
            elapsed = time.perf_counter() - start
            stop.set()
            if not receive.poll(65):
                observer.terminate()
                observer.join(timeout=5)
                raise RuntimeError("RPC observer did not finish")
            latencies, error = receive.recv()
            observer.join(timeout=5)
        assert not observer.is_alive()
        assert not error, error
        receive.close()
        output.close()
        if sample:
            sample.wait(timeout=30)
        latencies.sort()
        assert latencies, "No RPC samples were collected"
        with p2p_lock:
            requested = sum(peer.requested for peer in peers)
            assert all(peer.requested == len(messages) * self.options.batch for peer in peers)
        assert requested == expected, (requested, expected)
        result = {"seconds": elapsed, "announcements": len(peers) * len(messages) * self.options.batch,
                  "requested": requested, "rpc_count": len(latencies),
                  "rpc_ms_p50": latencies[len(latencies)//2],
                  "rpc_ms_p95": latencies[int(len(latencies)*.95)],
                  "rpc_ms_p99": latencies[int(len(latencies)*.99)], "rpc_ms_max": max(latencies)}
        Path(self.options.tmpdir, "rpc-latencies-ms.json").write_text(json.dumps(latencies) + "\n")
        Path(self.options.tmpdir, "metrics.json").write_text(json.dumps(result, indent=2) + "\n")
        self.log.info("PROFILE %s", json.dumps(result))


if __name__ == "__main__":
    InventoryBenchmark().main()
