#!/usr/bin/env python3
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

"""End-to-end AssumeUTXO coverage for Dash deterministic state.

Build a DIP3/v20 chain with real masternodes and both rotated and non-rotated
quorums, load its dynamically authorized snapshot, and exercise validation,
restart recovery, LLMQ use, and malformed evo sections.
"""

import subprocess
from io import BytesIO
from pathlib import Path

from test_framework.messages import msg_isdlock
from test_framework.p2p import P2PInterface
from test_framework.test_framework import DashTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
    force_finish_mnsync,
    initialize_datadir,
    sha256sum_file,
)


LLMQ_TEST = 100
LLMQ_TEST_DIP0024 = 103
EVO_MARKER = b"DASHEVO\x00"
REGTEST_SPORK_KEY = "cP4EKFyJsHT39LDqgdcB43Y3YXjNyjb5Fuas1GQSeAtjnZWmZEQK"
COMPILED_HEIGHT = 110
COMPILED_BLOCK_HASH = "729bcb1479ff9f4968439f0276bd76bcb2de0f0720b7a16f383321f6a41cb238"

# These are local views, not deterministic masternode-list state:
# - confirmations depends on the node's locally active UTXO chainstate;
# - wallet describes locally held keys/scripts (and is absent with -disablewallet);
# - metaInfo is locally learned connection/mixing metadata.
LOCAL_PROTX_FIELDS = {"confirmations", "wallet", "metaInfo"}
QUORUM_COMMITMENT_FIELDS = (
    "height",
    "type",
    "quorumHash",
    "quorumIndex",
    "minedBlock",
    "previousConsecutiveDKGFailures",
    "quorumPublicKey",
)
QUORUM_MEMBER_FIELDS = ("proTxHash", "pubKeyOperator", "valid", "pubKeyShare")


class AssumeutxoDashTest(DashTestFramework):
    def set_test_params(self):
        # Rotation produces two four-member indexed quorums per cycle, so the
        # fixture needs eight masternodes. Delay v20 to follow the established
        # rotation fixture: first make ordinary quorums, then activate rotation.
        args = [["-vbparams=testdummy:999999999999:999999999999"] for _ in range(6)]
        self.set_dash_test_params(6, 5, extra_args=args, evo_count=3)
        self.set_dash_llmq_test_params(3, 2)
        self.delay_v20_and_mn_rr(height=300)
        self.rpc_timeout = 180

    def add_options(self, parser):
        self.add_wallet_options(parser)

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def add_snapshot_node(self, assumeutxo_arg):
        node_index = len(self.nodes)
        extra_args = [
            *self.extra_args[0],
            assumeutxo_arg,
            "-disablewallet",
            f"-sporkkey={REGTEST_SPORK_KEY}",
        ]
        initialize_datadir(self.options.tmpdir, node_index, self.chain, self.disable_autoconnect)
        self.add_nodes(1, extra_args=[extra_args])
        self.start_node(node_index, extra_args)
        return node_index, extra_args

    def submit_headers(self, node, source, height, start_height=1):
        for block_height in range(start_height, height + 1):
            node.submitheader(source.getblock(source.getblockhash(block_height), 0))
        assert_equal(node.getblockchaininfo()["headers"], height)

    def assert_unvalidated_snapshot(self, node, base_height, base_hash, background_height):
        normal, snapshot = node.getchainstates()["chainstates"]
        assert_equal(normal["blocks"], background_height)
        assert_equal(normal["validated"], True)
        assert "snapshot_blockhash" not in normal
        assert_equal(snapshot["blocks"], base_height)
        assert_equal(snapshot["snapshot_blockhash"], base_hash)
        assert_equal(snapshot["validated"], False)

    def normalized_protx_state(self, node, height):
        entries = node.protx("list", "registered", True, height)
        return sorted(
            ({key: value for key, value in entry.items() if key not in LOCAL_PROTX_FIELDS}
             for entry in entries),
            key=lambda entry: entry["proTxHash"],
        )

    def normalized_quorum_info(self, node, llmq_type, quorum_hash):
        info = node.quorum("info", llmq_type, quorum_hash)
        return {
            "commitment": {field: info[field] for field in QUORUM_COMMITMENT_FIELDS if field in info},
            # Preserve RPC order: member position is commitment-significant.
            "members": [
                {field: member[field] for field in QUORUM_MEMBER_FIELDS if field in member}
                for member in info["members"]
            ],
        }

    def test_assumeutxodata_startup(self, node, valid_arg):
        self.log.info("Reject malformed and colliding -assumeutxodata entries on regtest")
        fields = valid_arg.removeprefix("-assumeutxodata=").split(":")
        valid_hash = fields[1]
        invalid_values = [
            ":".join(fields[:-1]),                              # too few fields
            ":".join([*fields, "extra"]),                     # too many fields
            ":".join(["0", *fields[1:]]),                    # height out of range
            ":".join(["2147483648", *fields[1:]]),           # height over int32
            ":".join([fields[0], *fields[1:3], "4294967296", fields[4]]),  # nchaintx over uint32
            ":".join([fields[0], "0" * 64, *fields[2:]]),    # null serialized hash
            ":".join([fields[0], "not-a-hash", *fields[2:]]), # malformed serialized hash
            ":".join([*fields[:2], "0" * 64, *fields[3:]]),  # null evo hash
            ":".join([*fields[:2], "not-a-hash", *fields[3:]]), # malformed evo hash
            ":".join([*fields[:4], "0" * 64]),                # null block hash
            ":".join([*fields[:4], "not-a-hash"]),           # malformed block hash
        ]
        for value in invalid_values:
            node.assert_start_raises_init_error(
                extra_args=[f"-assumeutxodata={value}"],
                expected_msg=(
                    f"Error: Invalid value ({value}) for -assumeutxodata=<height>:<hash_serialized>:"
                    "<evo_hash>:<nchaintx>:<blockhash>."
                ),
            )

        unique_a = f"901:{valid_hash}:{fields[2]}:902:{'01'.zfill(64)}"
        duplicate_height = f"901:{valid_hash}:{fields[2]}:903:{'02'.zfill(64)}"
        duplicate_hash = f"902:{valid_hash}:{fields[2]}:903:{'01'.zfill(64)}"
        compiled_height = f"{COMPILED_HEIGHT}:{valid_hash}:{fields[2]}:903:{'03'.zfill(64)}"
        compiled_hash = f"903:{valid_hash}:{fields[2]}:904:{COMPILED_BLOCK_HASH}"
        for args, duplicate in (
            ([unique_a, duplicate_height], duplicate_height),
            ([unique_a, duplicate_hash], duplicate_hash),
            ([compiled_height], compiled_height),
            ([compiled_hash], compiled_hash),
        ):
            node.assert_start_raises_init_error(
                extra_args=[f"-assumeutxodata={value}" for value in args],
                expected_msg=f"Error: Duplicate height or block hash in -assumeutxodata ({duplicate}).",
            )

        self.log.info("Ignore -assumeutxodata silently on a non-regtest startup")
        node.start(
            extra_args=["-regtest=0", "-testnet", "-server=0", "-listen=0", "-dnsseed=0", "-assumeutxodata=malformed"],
        )
        try:
            try:
                node.process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                pass
            else:
                raise AssertionError("non-regtest node rejected an ignored -assumeutxodata value")
        finally:
            node.process.terminate()
            node.process.wait(timeout=10)
            node.running = False
            node.process = None

    def relay_islock(self, node, raw_tx, islock_hex, txid):
        # Sporks are network state, not part of the snapshot. Enable IS locally
        # without connecting this isolated node and starting block download.
        node.sporkupdate("SPORK_2_INSTANTSEND_ENABLED", 0)
        force_finish_mnsync(node)
        assert_equal(node.sendrawtransaction(raw_tx), txid)
        islock = msg_isdlock()
        islock.deserialize(BytesIO(bytes.fromhex(islock_hex)))
        peer = node.add_p2p_connection(P2PInterface())
        peer.send_message(islock)
        self.wait_until(lambda: node.getislocks([txid])[0] != "None")
        assert_equal(node.getislocks([txid])[0]["hex"], islock_hex)

    def run_test(self):
        node0 = self.nodes[0]
        node0.sporkupdate("SPORK_17_QUORUM_DKG_ENABLED", 0)
        self.wait_for_sporks_same()

        self.log.info("Add three EvoNodes for llmq_test_platform")
        for _ in range(self.evo_count):
            self.dynamically_add_masternode(evo=True)

        self.log.info("Create non-rotated quorums before activating v20")
        self.mine_quorum()
        self.mine_quorum()
        self.activate_v20(expected_activation_height=300)
        self.generate(node0, 1)

        self.log.info("Create rotated quorums and fill all active/safety horizons")
        for _ in range(4):
            self.mine_cycle_quorum()

        # The helper finishes after the DKG mining window plus the signing
        # maturity offset. This is deliberately outside active DKG phases.
        base_height = node0.getblockcount()
        assert 10 < base_height % 24 < 24
        base_hash = node0.getbestblockhash()
        self.wait_for_chainlocked_block_all_nodes(base_hash, timeout=30)

        deployment_info = node0.getdeploymentinfo()
        for deployment in ("dip0003", "dip0008", "dip0024", "v19", "v20"):
            assert deployment_info["deployments"][deployment]["active"]
        ordinary_quorums = node0.quorum("list", LLMQ_TEST)["llmq_test"]
        rotated_quorums = node0.quorum("list", LLMQ_TEST_DIP0024)["llmq_test_dip0024"]
        assert ordinary_quorums
        assert rotated_quorums

        self.log.info("Create a tip oracle for the full Dash UTXO+evo snapshot")
        oracle = node0.dumptxoutset("assumeutxo-dash-oracle.dat", "latest")
        assert_equal(oracle["base_height"], base_height)
        assert_equal(oracle["base_hash"], base_hash)

        self.log.info("Dump the same base historically and prove rollback+rollforward preserves Evo state")
        node0.setnetworkactive(False)
        extra_hash = self.generate(node0, 1, sync_fun=self.no_op)[0]
        assert_equal(node0.getblockcount(), base_height + 1)
        dump = node0.dumptxoutset("assumeutxo-dash.dat", rollback=base_height)
        assert_equal(node0.getblockcount(), base_height + 1)
        for field in (
            "coins_written",
            "base_hash",
            "base_height",
            "txoutset_hash",
            "evo_hash",
            "evo_mn_count",
            "nchaintx",
        ):
            assert_equal(dump[field], oracle[field])
        assert_equal(sha256sum_file(dump["path"]), sha256sum_file(oracle["path"]))

        # Keep the remainder of the lifecycle fixture at the historical base.
        # The isolated extra block was never relayed or ChainLocked.
        node0.invalidateblock(extra_hash)
        assert_equal(node0.getblockcount(), base_height)
        node0.setnetworkactive(True)
        force_finish_mnsync(node0)
        self.sync_all()
        self.wait_for_sporks_same()

        self.log.info("Keep the M4 dump-side checks and capture both commitments")
        assert_equal(dump["base_height"], base_height)
        assert_equal(dump["base_hash"], base_hash)
        assert len(dump["txoutset_hash"]) == 64
        assert len(dump["evo_hash"]) == 64
        assert dump["evo_mn_count"] >= 8
        snapshot_path = Path(dump["path"])
        snapshot_bytes = snapshot_path.read_bytes()
        marker_offset = snapshot_bytes.index(EVO_MARKER)
        assert marker_offset > 40

        assumeutxo_arg = (
            f"-assumeutxodata={base_height}:{dump['txoutset_hash']}:"
            f"{dump['evo_hash']}:{dump['nchaintx']}:{base_hash}"
        )

        self.stop_node(5)
        self.test_assumeutxodata_startup(self.nodes[5], assumeutxo_arg)
        self.start_node(5)

        self.log.info("An active masternode must refuse snapshot loading")
        assert_raises_rpc_error(
            -1,
            "loadtxoutset is unavailable in masternode mode",
            self.mninfo[0].get_node(self).loadtxoutset,
            str(snapshot_path),
        )

        self.log.info("Negative evo-section checks")
        negative_index, _ = self.add_snapshot_node(assumeutxo_arg)
        negative = self.nodes[negative_index]
        self.submit_headers(negative, node0, base_height)

        utxo_only_path = snapshot_path.with_suffix(".utxo-only.dat")
        utxo_only_path.write_bytes(snapshot_bytes[:marker_offset])
        with negative.assert_debug_log(["missing evo section at DIP3-active base"]):
            assert_raises_rpc_error(
                -32603,
                "missing evo section at DIP3-active base",
                negative.loadtxoutset,
                str(utxo_only_path),
            )

        # This fixture has no asset-unlock ranges or MNHF signals, so the last
        # 26 bytes are three int64 credit-pool fields followed by two zero
        # CompactSize counts. Alter currentLimit, which remains structurally
        # valid and is intentionally not a CbTx root, to reach the evo hash check.
        tampered = bytearray(snapshot_bytes)
        assert_equal(tampered[-2:], b"\x00\x00")
        tampered[-18] ^= 1
        tampered_path = snapshot_path.with_suffix(".tampered-evo.dat")
        tampered_path.write_bytes(tampered)
        with negative.assert_debug_log(["bad evo snapshot hash"]):
            assert_raises_rpc_error(
                -32603,
                "evo snapshot hash mismatch",
                negative.loadtxoutset,
                str(tampered_path),
            )
        self.log.info("Load the full snapshot on a fresh non-masternode")
        snapshot_index, snapshot_args = self.add_snapshot_node(assumeutxo_arg)
        snapshot_node = self.nodes[snapshot_index]
        # Leave a bounded tail for the distinct post-restart background phase.
        mid_height = base_height - 48
        for height in range(1, mid_height + 1):
            assert snapshot_node.submitblock(
                node0.getblock(node0.getblockhash(height), 0)) in (None, "duplicate")
        assert_equal(snapshot_node.getblockcount(), mid_height)
        self.submit_headers(snapshot_node, node0, base_height, mid_height + 1)
        loaded = snapshot_node.loadtxoutset(str(snapshot_path))
        assert_equal(loaded["base_height"], base_height)
        assert_equal(loaded["tip_hash"], base_hash)
        assert_equal(loaded["coins_loaded"], dump["coins_written"])
        self.assert_unvalidated_snapshot(snapshot_node, base_height, base_hash, mid_height)

        def assert_unfaked_snapshot_counts(background_height):
            # The dynamic -assumeutxodata count belongs only to the snapshot
            # base. Header-only blocks between background validation and the
            # base must keep unknown nTx and nChainTx values at zero.
            assert_equal(snapshot_node.getblockheader(base_hash)["nTx"], 0)
            assert_equal(
                snapshot_node.getchaintxstats(nblocks=1, blockhash=base_hash)["txcount"],
                dump["nchaintx"],
            )
            first_unknown_height = background_height + 1
            if first_unknown_height < base_height:
                first_unknown_hash = snapshot_node.getblockhash(first_unknown_height)
                assert_equal(snapshot_node.getblockheader(first_unknown_hash)["nTx"], 0)
                assert_equal(
                    snapshot_node.getchaintxstats(nblocks=1, blockhash=first_unknown_hash)["txcount"],
                    0,
                )

        assert_unfaked_snapshot_counts(mid_height)

        self.log.info("Restart immediately after loading the snapshot")
        self.restart_node(snapshot_index, extra_args=snapshot_args)
        self.assert_unvalidated_snapshot(snapshot_node, base_height, base_hash, mid_height)
        assert_unfaked_snapshot_counts(mid_height)

        self.log.info("Advance only the disconnected background chainstate")
        advanced_height = base_height - 1
        assert advanced_height > mid_height
        for height in range(mid_height + 1, advanced_height + 1):
            assert snapshot_node.submitblock(
                node0.getblock(node0.getblockhash(height), 0)) in (None, "duplicate")
        self.assert_unvalidated_snapshot(snapshot_node, base_height, base_hash, advanced_height)
        assert_unfaked_snapshot_counts(advanced_height)

        self.log.info("Compare deterministic masternode and quorum state at the base")
        assert_equal(
            self.normalized_protx_state(snapshot_node, base_height),
            self.normalized_protx_state(node0, base_height),
        )
        for llmq_type, quorum_hashes in (
            (LLMQ_TEST, ordinary_quorums),
            (LLMQ_TEST_DIP0024, rotated_quorums),
        ):
            for quorum_hash in quorum_hashes:
                assert_equal(
                    self.normalized_quorum_info(snapshot_node, llmq_type, quorum_hash),
                    self.normalized_quorum_info(node0, llmq_type, quorum_hash),
                )

        self.log.info("Verify and relay ChainLock and InstantSend locks pre-completion")
        best_cl = node0.getbestchainlock()
        assert snapshot_node.verifychainlock(
            best_cl["blockhash"], best_cl["signature"], best_cl["height"])
        assert_equal(
            snapshot_node.submitchainlock(
                best_cl["blockhash"], best_cl["signature"], best_cl["height"]),
            best_cl["height"],
        )
        assert_equal(snapshot_node.getbestchainlock()["blockhash"], best_cl["blockhash"])

        raw_tx = self.create_raw_tx(node0, node0, 1, 1, 100)
        txid = node0.sendrawtransaction(raw_tx["hex"])
        self.wait_for_instantlock(txid, nodes=[node0])
        islock = node0.getislocks([txid])[0]
        assert snapshot_node.verifyislock(islock["id"], txid, islock["signature"], base_height)
        self.relay_islock(snapshot_node, raw_tx["hex"], islock["hex"], txid)

        assert_raises_rpc_error(
            -32603,
            "Only available in masternode mode",
            snapshot_node.quorum,
            "sign",
            LLMQ_TEST,
            "01".zfill(64),
            "02".zfill(64),
        )

        self.log.info("Restart while background validation is paused halfway")
        self.restart_node(snapshot_index, extra_args=snapshot_args)
        self.assert_unvalidated_snapshot(snapshot_node, base_height, base_hash, advanced_height)

        self.log.info("Complete background validation and its deferred evo comparison")
        with snapshot_node.assert_debug_log(
            ["has been fully validated"],
            unexpected_msgs=["evo state mismatch", "EVO_STATE_MISMATCH"],
            timeout=180,
        ):
            self.connect_nodes(snapshot_index, 0)
            self.wait_until(lambda: len(snapshot_node.getchainstates()["chainstates"]) == 1, timeout=180)
        self.disconnect_nodes(snapshot_index, 0)

        completed, = snapshot_node.getchainstates()["chainstates"]
        assert_equal(completed["blocks"], base_height)
        assert_equal(completed["validated"], True)
        assert_equal(completed["snapshot_blockhash"], base_hash)
        assert_equal(
            snapshot_node.getblockheader(base_hash)["nTx"],
            node0.getblockheader(base_hash)["nTx"],
        )
        assert_equal(
            snapshot_node.getchaintxstats(nblocks=1, blockhash=base_hash)["txcount"],
            dump["nchaintx"],
        )

        self.log.info("Restart after completion to run validated cleanup and b_b4 promotion")
        with snapshot_node.assert_debug_log(
            ["cleaning up unneeded background chainstate", "moving snapshot chainstate"],
            unexpected_msgs=["evo state mismatch", "EVO_STATE_MISMATCH"],
            timeout=180,
        ):
            self.restart_node(snapshot_index, extra_args=snapshot_args)
        cleaned, = snapshot_node.getchainstates()["chainstates"]
        assert_equal(cleaned["blocks"], base_height)
        assert_equal(cleaned["validated"], True)
        assert "snapshot_blockhash" not in cleaned
        assert not (snapshot_node.chain_path / "chainstate_snapshot").exists()
        assert not (snapshot_node.chain_path / "chainstate_todelete").exists()

        self.log.info("Restart once more after cleanup")
        self.restart_node(snapshot_index, extra_args=snapshot_args)
        final_state, = snapshot_node.getchainstates()["chainstates"]
        assert_equal(final_state["blocks"], base_height)
        assert_equal(final_state["validated"], True)
        assert "snapshot_blockhash" not in final_state


if __name__ == "__main__":
    AssumeutxoDashTest().main()
