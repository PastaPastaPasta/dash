#!/usr/bin/env python3
# Copyright (c) 2017-2019 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test RPC calls related to net.

Tests correspond to code in rpc/net.cpp.
"""

from test_framework.p2p import P2PInterface
import test_framework.messages
from test_framework.messages import (
    NODE_NETWORK,
)

from itertools import product

from test_framework.test_framework import DashTestFramework
from test_framework.util import (
    assert_approx,
    assert_equal,
    assert_greater_than,
    assert_raises_rpc_error,
    p2p_port,
)


def assert_net_servicesnames(servicesflag, servicenames):
    """Utility that checks if all flags are correctly decoded in
    `getpeerinfo` and `getnetworkinfo`.

    :param servicesflag: The services as an integer.
    :param servicenames: The list of decoded services names, as strings.
    """
    servicesflag_generated = 0
    for servicename in servicenames:
        servicesflag_generated |= getattr(test_framework.messages, 'NODE_' + servicename)
    assert servicesflag_generated == servicesflag


class NetTest(DashTestFramework):
    def set_test_params(self):
        self.set_dash_test_params(3, 1, fast_dip3_enforcement=True)
        self.supports_cli = False

    def run_test(self):
        # Get out of IBD for the getpeerinfo tests.
        self.nodes[0].generate(101)
        # Wait for one ping/pong to finish so that we can be sure that there is no chatter between nodes for some time
        # Especially the exchange of messages like getheaders and friends causes test failures here
        self.nodes[0].ping()
        self.wait_until(lambda: all(['pingtime' in n for n in self.nodes[0].getpeerinfo()]))
        self.log.info('Connect nodes both way')
        self.connect_nodes(0, 1)
        self.connect_nodes(1, 0)
        self.sync_all()

        self.test_connection_count()
        self.test_getpeerinfo()
        self.test_getnettotals()
        self.test_getnetworkinfo()
        self.test_getaddednodeinfo()
        self.test_service_flags()
        self.test_getnodeaddresses()

    def test_connection_count(self):
        self.log.info("Test getconnectioncount")
        # After using `connect_nodes` to connect nodes 0 and 1 to each other.
        # and node0 was also connected to node2 (a masternode)
        # during network setup
        assert_equal(self.nodes[0].getconnectioncount(), 3)

        self.log.info("Check getpeerinfo output before a version message was sent")
        no_version_peer_id = 2
        no_version_peer_conntime = int(time.time())
        self.nodes[0].setmocktime(no_version_peer_conntime)
        with self.nodes[0].assert_debug_log([f"Added connection peer={no_version_peer_id}"]):
            self.nodes[0].add_p2p_connection(P2PInterface(), send_version=False, wait_for_verack=False)
        self.nodes[0].setmocktime(0)
        peer_info = self.nodes[0].getpeerinfo()[no_version_peer_id]
        peer_info.pop("addr")
        peer_info.pop("addrbind")
        assert_equal(
            peer_info,
            {
                "addr_processed": 0,
                "addr_rate_limited": 0,
                "addr_relay_enabled": False,
                "bip152_hb_from": False,
                "bip152_hb_to": False,
                "bytesrecv": 0,
                "bytesrecv_per_msg": {},
                "bytessent": 0,
                "bytessent_per_msg": {},
                "connection_type": "inbound",
                "conntime": no_version_peer_conntime,
                "id": no_version_peer_id,
                "inbound": True,
                "inflight": [],
                "last_block": 0,
                "last_transaction": 0,
                "lastrecv": 0,
                "lastsend": 0,
                "minfeefilter": Decimal("0E-8"),
                "network": "not_publicly_routable",
                "permissions": [],
                "presynced_headers": -1,
                "relaytxes": False,
                "services": "0000000000000000",
                "servicesnames": [],
                "startingheight": -1,
                "subver": "",
                "synced_blocks": -1,
                "synced_headers": -1,
                "timeoffset": 0,
                "version": 0,
            },
        )
        self.nodes[0].disconnect_p2ps()

    def test_getnettotals(self):
        self.log.info("Test getnettotals")
        # Test getnettotals and getpeerinfo by doing a ping. The bytes
        # sent/received should increase by at least the size of one ping (32
        # bytes) and one pong (32 bytes).
        net_totals_before = self.nodes[0].getnettotals()
        peer_info_before = self.nodes[0].getpeerinfo()

        self.nodes[0].ping()
        self.wait_until(lambda: (self.nodes[0].getnettotals()['totalbytessent'] >= net_totals_before['totalbytessent'] + 32 * 2), timeout=1)
        self.wait_until(lambda: (self.nodes[0].getnettotals()['totalbytesrecv'] >= net_totals_before['totalbytesrecv'] + 32 * 2), timeout=1)

        for peer_before in peer_info_before:
            peer_after = lambda: next(p for p in self.nodes[0].getpeerinfo() if p['id'] == peer_before['id'])
            self.wait_until(lambda: peer_after()['bytesrecv_per_msg'].get('pong', 0) >= peer_before['bytesrecv_per_msg'].get('pong', 0) + 32, timeout=1)
            self.wait_until(lambda: peer_after()['bytessent_per_msg'].get('ping', 0) >= peer_before['bytessent_per_msg'].get('ping', 0) + 32, timeout=1)

    def test_getnetworkinfo(self):
        self.log.info("Test getnetworkinfo")
        info = self.nodes[0].getnetworkinfo()
        assert_equal(self.nodes[0].getnetworkinfo()['networkactive'], True)
        assert_equal(self.nodes[0].getnetworkinfo()['connections'], 3)

        with self.nodes[0].assert_debug_log(expected_msgs=['SetNetworkActive: false\n']):
            self.nodes[0].setnetworkactive(state=False)
        assert_equal(self.nodes[0].getnetworkinfo()['networkactive'], False)
        # Wait a bit for all sockets to close
        self.wait_until(lambda: self.nodes[0].getnetworkinfo()['connections'] == 0, timeout=3)
        self.wait_until(lambda: self.nodes[1].getnetworkinfo()['connections'] == 0, timeout=3)

        with self.nodes[0].assert_debug_log(expected_msgs=['SetNetworkActive: true\n']):
            self.nodes[0].setnetworkactive(state=True)
        # Connect nodes both ways.
        self.connect_nodes(0, 1)
        self.connect_nodes(1, 0)

        assert_equal(self.nodes[0].getnetworkinfo()['networkactive'], True)
        assert_equal(self.nodes[0].getnetworkinfo()['connections'], 2)

        # check the `servicesnames` field
        network_info = [node.getnetworkinfo() for node in self.nodes]
        for info in network_info:
            assert_net_servicesnames(int(info["localservices"], 0x10), info["localservicesnames"])

        # Check dynamically generated networks list in getnetworkinfo help output.
        assert "(ipv4, ipv6, onion, i2p)" in self.nodes[0].help("getnetworkinfo")

        self.log.info('Test extended connections info')
        self.connect_nodes(1, 2)
        self.nodes[1].ping()
        self.wait_until(lambda: all(['pingtime' in n for n in self.nodes[1].getpeerinfo()]))
        assert_equal(self.nodes[1].getnetworkinfo()['connections'], 3)
        assert_equal(self.nodes[1].getnetworkinfo()['inboundconnections'], 1)
        assert_equal(self.nodes[1].getnetworkinfo()['outboundconnections'], 2)
        assert_equal(self.nodes[1].getnetworkinfo()['mnconnections'], 1)
        assert_equal(self.nodes[1].getnetworkinfo()['inboundmnconnections'], 0)
        assert_equal(self.nodes[1].getnetworkinfo()['outboundmnconnections'], 1)

    def test_getaddednodeinfo(self):
        self.log.info("Test getaddednodeinfo")
        assert_equal(self.nodes[0].getaddednodeinfo(), [])
        # add a node (node2) to node0
        ip_port = "127.0.0.1:{}".format(p2p_port(2))
        self.nodes[0].addnode(node=ip_port, command='add')
        # check that the node has indeed been added
        added_nodes = self.nodes[0].getaddednodeinfo(ip_port)
        assert_equal(len(added_nodes), 1)
        assert_equal(added_nodes[0]['addednode'], ip_port)
        # check that node cannot be added again
        assert_raises_rpc_error(-23, "Node already added", self.nodes[0].addnode, node=ip_port, command='add')
        # check that node can be removed
        self.nodes[0].addnode(node=ip_port, command='remove')
        assert_equal(self.nodes[0].getaddednodeinfo(), [])
        # check that trying to remove the node again returns an error
        assert_raises_rpc_error(-24, "Node could not be removed", self.nodes[0].addnode, node=ip_port, command='remove')
        # check that a non-existent node returns an error
        assert_raises_rpc_error(-24, "Node has not been added", self.nodes[0].getaddednodeinfo, '1.1.1.1')

    def test_getpeerinfo(self):
        self.log.info("Test getpeerinfo")
        # Create a few getpeerinfo last_block/last_transaction values.
        if self.is_wallet_compiled():
            self.nodes[0].sendtoaddress(self.nodes[1].getnewaddress(), 1)
        self.nodes[1].generate(1)
        self.sync_all()
        time_now = self.mocktime
        peer_info = [x.getpeerinfo() for x in self.nodes]
        # Verify last_block and last_transaction keys/values.
        for node, peer, field in product(range(self.num_nodes - self.mn_count), range(2), ['last_block', 'last_transaction']):
            assert field in peer_info[node][peer].keys()
            if peer_info[node][peer][field] != 0:
                assert_approx(peer_info[node][peer][field], time_now, vspan=60)
        # check both sides of bidirectional connection between nodes
        # the address bound to on one side will be the source address for the other node
        assert_equal(peer_info[0][0]['addrbind'], peer_info[1][0]['addr'])
        assert_equal(peer_info[1][0]['addrbind'], peer_info[0][0]['addr'])
        # check the `servicesnames` field
        for info in peer_info:
            assert_net_servicesnames(int(info[0]["services"], 0x10), info[0]["servicesnames"])

        # Check dynamically generated networks list in getpeerinfo help output.
        assert "(ipv4, ipv6, onion, i2p, not_publicly_routable)" in self.nodes[0].help("getpeerinfo")
        # This part is slightly different comparing to the Bitcoin implementation. This is expected because we create connections on network setup a bit differently too.
        # We also create more connection during the test itself to test mn specific stats
        assert_equal(peer_info[0][0]['connection_type'], 'inbound')
        assert_equal(peer_info[0][1]['connection_type'], 'inbound')
        assert_equal(peer_info[0][2]['connection_type'], 'manual')

        assert_equal(peer_info[1][0]['connection_type'], 'manual')
        assert_equal(peer_info[1][1]['connection_type'], 'inbound')

        assert_equal(peer_info[2][0]['connection_type'], 'manual')

    def test_service_flags(self):
        self.log.info("Test service flags")
        self.nodes[0].add_p2p_connection(P2PInterface(), services=(1 << 4) | (1 << 63))
        assert_equal(['UNKNOWN[2^4]', 'UNKNOWN[2^63]'], self.nodes[0].getpeerinfo()[-1]['servicesnames'])
        self.nodes[0].disconnect_p2ps()

    def test_getnodeaddresses(self):
        self.log.info("Test getnodeaddresses")
        self.nodes[0].add_p2p_connection(P2PInterface())

        # Add some addresses to the Address Manager over RPC. Due to the way
        # bucket and bucket position are calculated, some of these addresses
        # will collide.
        imported_addrs = []
        for i in range(10000):
            first_octet = i >> 8
            second_octet = i % 256
            a = "{}.{}.1.1".format(first_octet, second_octet)
            imported_addrs.append(a)
            self.nodes[0].addpeeraddress(a, 8333)

        # Obtain addresses via rpc call and check they were ones sent in before.
        #
        # Maximum possible addresses in addrman is 10000, although actual
        # number will usually be less due to bucket and bucket position
        # collisions.
        node_addresses = self.nodes[0].getnodeaddresses(0)
        assert_greater_than(len(node_addresses), 5000)
        assert_greater_than(10000, len(node_addresses))
        for a in node_addresses:
            assert_equal(a["time"], self.mocktime)
            assert_equal(a["services"], NODE_NETWORK)
            assert a["address"] in imported_addrs
            assert_equal(a["port"], 8333)

        node_addresses = self.nodes[0].getnodeaddresses(1)
        assert_equal(len(node_addresses), 1)

        assert_raises_rpc_error(-8, "Address count out of range", self.nodes[0].getnodeaddresses, -1)

        # addrman's size cannot be known reliably after insertion, as hash collisions may occur
        # so only test that requesting a large number of addresses returns less than that
        LARGE_REQUEST_COUNT = 10000
        node_addresses = self.nodes[0].getnodeaddresses(LARGE_REQUEST_COUNT)
        assert_greater_than(LARGE_REQUEST_COUNT, len(node_addresses))


if __name__ == '__main__':
    NetTest().main()
