#!/usr/bin/env python3
# Copyright (c) 2021 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

from test_framework.mininode import *
from test_framework.test_framework import DashTestFramework
from test_framework.util import force_finish_mnsync, assert_equal, connect_nodes

'''
p2p_quorum_data.py

Tests QGETDATA/QDATA functionality
'''


def assert_qdata(qdata, qgetdata, error, len_vvec=0, len_contributions=0):
    assert qdata is not None and qgetdata is not None
    assert_equal(qdata.quorum_type, qgetdata.quorum_type)
    assert_equal(qdata.quorum_hash, qgetdata.quorum_hash)
    assert_equal(qdata.data_mask, qgetdata.data_mask)
    assert_equal(qdata.protx_hash, qgetdata.protx_hash)
    assert_equal(qdata.error, error)
    assert_equal(len(qdata.quorum_vvec), len_vvec)
    assert_equal(len(qdata.enc_contributions), len_contributions)


def wait_for_banscore(node, peer_id, expexted_score):
    def get_score():
        for peer in node.getpeerinfo():
            if peer["id"] == peer_id:
                return peer["banscore"]
        return None
    wait_until(lambda: get_score() == expexted_score, timeout=6, sleep=2)


def p2p_connection(node, subversion=MY_SUBVERSION):
    connection = node.add_p2p_connection(QuorumDataInterface(), send_version=False)
    version_message = msg_version()
    version_message.strSubVer = subversion
    connection.sendbuf = connection.build_message(version_message)
    return connection


def get_mininode_id(node, subversion=MY_SUBVERSION):
    tries = 0
    node_id = None
    while tries < 10 and node_id is None:
        for p in node.getpeerinfo():
            if p["subver"] == subversion.decode():
                node_id = p["id"]
                break
        tries += 1
        time.sleep(1)
    assert(node_id is not None)
    return node_id


def mnauth(node, node_id, protx_hash, operator_pubkey):
    assert(node.mnauth(node_id, protx_hash, operator_pubkey))
    mnauth_peer_id = None
    for peer in node.getpeerinfo():
        if "verified_proregtx_hash" in peer and peer["verified_proregtx_hash"] == protx_hash:
            assert_equal(mnauth_peer_id, None)
            mnauth_peer_id = peer["id"]
    assert_equal(mnauth_peer_id, node_id)

class QuorumDataInterface(P2PInterface):
    def __init__(self):
        super().__init__()

    def test_qgetdata(self, qgetdata, expected_error=0, len_vvec=0, len_contributions=0, response_expected=True):
        self.send_message(qgetdata)
        self.wait_for_qdata(message_expected=response_expected)
        if response_expected:
            assert_qdata(self.get_qdata(), qgetdata, expected_error, len_vvec, len_contributions)

    def get_qgetdata(self):
        return self.last_message["qdata"]

    def wait_for_qgetdata(self, timeout=3, message_expected=True):
        def test_function():
            return self.message_count["qgetdata"]
        wait_until(test_function, timeout=timeout, lock=mininode_lock, do_assert=message_expected)
        self.message_count["qgetdata"] = 0
        if not message_expected:
            assert (not self.message_count["qgetdata"])

    def get_qdata(self):
        return self.last_message["qdata"]

    def wait_for_qdata(self, timeout=10, message_expected=True):
        def test_function():
            return self.message_count["qdata"]
        wait_until(test_function, timeout=timeout, lock=mininode_lock, do_assert=message_expected)
        self.message_count["qdata"] = 0
        if not message_expected:
            assert (not self.message_count["qdata"])


class QuorumDataMessagesTest(DashTestFramework):
    def set_test_params(self):
        self.set_dash_test_params(4, 3, fast_dip3_enforcement=True)

    def restart_mn(self, mn, reindex=False):
        args = self.extra_args[mn.nodeIdx] + ['-masternodeblsprivkey=%s' % mn.keyOperator]
        if reindex:
            args.append('-reindex')
        self.restart_node(mn.nodeIdx, args)
        force_finish_mnsync(mn.node)
        connect_nodes(mn.node, 0)
        self.sync_blocks()

    def run_test(self):

        def test_send_from_two_to_one(send_1, expected_banscore_1, send_2, expected_banscore_2, clear_requests=False):
            if clear_requests:
                force_request_expire()
            if send_1:
                p2p_mn3_1.test_qgetdata(qgetdata_vvec, 0, self.llmq_threshold, 0)
            if send_2:
                p2p_mn3_2.test_qgetdata(qgetdata_vvec, 0, self.llmq_threshold, 0)
            wait_for_banscore(mn3.node, id_p2p_mn3_1, expected_banscore_1)
            wait_for_banscore(mn3.node, id_p2p_mn3_2, expected_banscore_2)

        def force_request_expire(bump_seconds=self.quorum_data_request_expiry_seconds + 1):
            self.bump_mocktime(bump_seconds)
            if node0.getblockcount() % 2:  # Test with/without expired request cleanup
                node0.generate(1)
                self.sync_blocks()

        self.nodes[0].spork("SPORK_17_QUORUM_DKG_ENABLED", 0)
        self.nodes[0].spork("SPORK_19_CHAINLOCKS_ENABLED", 4070908800)

        self.wait_for_sporks_same()
        quorum_hash = self.mine_quorum()

        node0 = self.nodes[0]
        mn1 = self.mninfo[0]
        mn2 = self.mninfo[1]
        mn3 = self.mninfo[2]

        fake_mnauth_1 = ["cecf37bf0ec05d2d22cb8227f88074bb882b94cd2081ba318a5a444b1b15b9fd",
                    "087ba00bf61135f3860c4944a0debabe186ef82628fbe4ceaed1ad51d672c58dde14ea4b321efe0b89257a40322bc972"]
        fake_mnauth_2 = ["6ad7ed7a2d6c2c1db30fc364114602b36b2730a9aa96d8f11f1871a9cee37378",
                    "122463411a86362966a5161805f24cf6a0eef08a586b8e00c4f0ad0b084c5bb3f5c9a60ee5ffc78db2313897e3ab2223"]

        p2p_node0 = p2p_connection(node0)
        p2p_mn1 = p2p_connection(mn1.node)
        network_thread_start()
        p2p_node0.wait_for_verack()
        p2p_mn1.wait_for_verack()
        id_p2p_node0 = get_mininode_id(node0)
        id_p2p_mn1 = get_mininode_id(mn1.node)

        wait_for_banscore(node0, id_p2p_node0, 0)
        wait_for_banscore(mn1.node, id_p2p_mn1, 0)

        quorum_hash_int = int(quorum_hash, 16)
        protx_hash_int = int(mn1.proTxHash, 16)

        # Valid requests
        qgetdata_vvec = msg_qgetdata(quorum_hash_int, 100, 0x01, protx_hash_int)
        qgetdata_contributions = msg_qgetdata(quorum_hash_int, 100, 0x02, protx_hash_int)
        qgetdata_all = msg_qgetdata(quorum_hash_int, 100, 0x03, protx_hash_int)
        # The normal node should not respond to qgetdata
        p2p_node0.test_qgetdata(qgetdata_all, response_expected=False)
        wait_for_banscore(node0, id_p2p_node0, 10)
        # The masternode should not respond to qgetdata for non-masternode connections
        p2p_mn1.test_qgetdata(qgetdata_all, response_expected=False)
        wait_for_banscore(mn1.node, id_p2p_mn1, 10)
        # Open a fake MNAUTH authenticated P2P connection to the masternode to allow qgetdata
        node0.disconnect_p2ps()
        mn1.node.disconnect_p2ps()
        network_thread_join()
        p2p_mn1 = p2p_connection(mn1.node)
        network_thread_start()
        p2p_mn1.wait_for_verack()
        id_p2p_mn1 = get_mininode_id(mn1.node)
        mnauth(mn1.node, id_p2p_mn1, fake_mnauth_1[0], fake_mnauth_1[1])
        # The masternode should now respond to qgetdata requests
        # Request verification vector
        p2p_mn1.test_qgetdata(qgetdata_vvec, 0, self.llmq_threshold, 0)
        wait_for_banscore(mn1.node, id_p2p_mn1, 0)
        # Request encrypted contributions
        p2p_mn1.test_qgetdata(qgetdata_contributions, 0, 0, self.llmq_size)
        wait_for_banscore(mn1.node, id_p2p_mn1, 25)
        # Request both
        p2p_mn1.test_qgetdata(qgetdata_all, 0, self.llmq_threshold, self.llmq_size)
        wait_for_banscore(mn1.node, id_p2p_mn1, 50)
        # Create invalid messages for all error cases
        qgetdata_invalid_type = msg_qgetdata(quorum_hash_int, 103, 0x01, protx_hash_int)
        qgetdata_invalid_block = msg_qgetdata(protx_hash_int, 100, 0x01, protx_hash_int)
        qgetdata_invalid_quorum = msg_qgetdata(int(mn1.node.getblockhash(0), 16), 100, 0x01, protx_hash_int)
        qgetdata_invalid_no_member = msg_qgetdata(quorum_hash_int, 100, 0x02, quorum_hash_int)
        # Possible error values of QDATA
        QUORUM_TYPE_INVALID = 1
        QUORUM_BLOCK_NOT_FOUND = 2
        QUORUM_NOT_FOUND = 3
        MASTERNODE_IS_NO_MEMBER = 4
        QUORUM_VERIFICATION_VECTOR_MISSING = 5
        ENCRYPTED_CONTRIBUTIONS_MISSING = 6
        # Test all invalid messages
        mn1.node.disconnect_p2ps()
        network_thread_join()
        p2p_mn1 = p2p_connection(mn1.node)
        network_thread_start()
        p2p_mn1.wait_for_verack()
        id_p2p_mn1 = get_mininode_id(mn1.node)
        mnauth(mn1.node, id_p2p_mn1, fake_mnauth_1[0], fake_mnauth_1[1])
        p2p_mn1.test_qgetdata(qgetdata_invalid_type, QUORUM_TYPE_INVALID)
        p2p_mn1.test_qgetdata(qgetdata_invalid_block, QUORUM_BLOCK_NOT_FOUND)
        p2p_mn1.test_qgetdata(qgetdata_invalid_quorum, QUORUM_NOT_FOUND)
        p2p_mn1.test_qgetdata(qgetdata_invalid_no_member, MASTERNODE_IS_NO_MEMBER)
        # The last two error case require the node to miss its DKG data so we just reindex the node.
        mn1.node.disconnect_p2ps()
        network_thread_join()
        self.restart_mn(mn1, reindex=True)
        # Re-connect to the masternode
        p2p_mn1 = p2p_connection(mn1.node)
        p2p_mn2 = p2p_connection(mn2.node)
        network_thread_start()
        p2p_mn1.wait_for_verack()
        p2p_mn2.wait_for_verack()
        id_p2p_mn1 = get_mininode_id(mn1.node)
        id_p2p_mn2 = get_mininode_id(mn2.node)
        assert(id_p2p_mn1 is not None)
        assert(id_p2p_mn2 is not None)
        mnauth(mn1.node, id_p2p_mn1, fake_mnauth_1[0], fake_mnauth_1[1])
        mnauth(mn2.node, id_p2p_mn2, fake_mnauth_2[0], fake_mnauth_2[1])
        # Validate the DKG data is missing
        p2p_mn1.test_qgetdata(qgetdata_vvec, QUORUM_VERIFICATION_VECTOR_MISSING)
        p2p_mn1.test_qgetdata(qgetdata_contributions, ENCRYPTED_CONTRIBUTIONS_MISSING)
        # Now that mn1 is missing its DKG data try to recover it by querying the data from mn2 and then sending it to
        # mn1 with a direct QDATA message.
        #
        # mininode - QGETDATA -> mn2 - QDATA -> mininode - QDATA -> mn1
        #
        # However, mn1 only accepts self requested QDATA messages, that's why we trigger mn1 - QGETDATA -> mininode
        # via the RPC command "quorum getdata".
        #
        # Get the required DKG data for mn1
        p2p_mn2.test_qgetdata(qgetdata_all, 0, self.llmq_threshold, self.llmq_size)
        # Trigger mn1 - QGETDATA -> p2p_mn1
        assert(mn1.node.quorum("getdata", id_p2p_mn1, 100, quorum_hash, 0x03, mn1.proTxHash))
        # Wait until mn1 sent the QGETDATA to p2p_mn1
        p2p_mn1.wait_for_qgetdata()
        # Send the QDATA received from mn2 to mn1
        p2p_mn1.send_message(p2p_mn2.get_qdata())
        # Now mn1 should have its data back!
        self.wait_for_quorum_data([mn1], 100, quorum_hash, recover=False)
        # Restart one more time and make sure data gets saved to db
        mn1.node.disconnect_p2ps()
        mn2.node.disconnect_p2ps()
        network_thread_join()
        self.restart_mn(mn1)
        self.wait_for_quorum_data([mn1], 100, quorum_hash, recover=False)
        # Test request limiting / banscore increase
        p2p_mn1 = p2p_connection(mn1.node)
        network_thread_start()
        p2p_mn1.wait_for_verack()
        id_p2p_mn1 = get_mininode_id(mn1.node)
        mnauth(mn1.node, id_p2p_mn1, fake_mnauth_1[0], fake_mnauth_1[1])
        p2p_mn1.test_qgetdata(qgetdata_vvec, 0, self.llmq_threshold, 0)
        wait_for_banscore(mn1.node, id_p2p_mn1, 0)
        force_request_expire(299)  # This shouldn't clear requests, next request should bump score
        p2p_mn1.test_qgetdata(qgetdata_vvec, 0, self.llmq_threshold, 0)
        wait_for_banscore(mn1.node, id_p2p_mn1, 25)
        force_request_expire(1)  # This should clear the requests now, next request should not bump score
        p2p_mn1.test_qgetdata(qgetdata_vvec, 0, self.llmq_threshold, 0)
        wait_for_banscore(mn1.node, id_p2p_mn1, 25)
        mn1.node.disconnect_p2ps()
        network_thread_join()
        # Requesting one QDATA with mn1 and mn2 from mn3 should not result in banscore increase
        # for either of both.
        version_m3_1 = MY_SUBVERSION + b"MN3_1"
        version_m3_2 = MY_SUBVERSION + b"MN3_2"
        p2p_mn3_1 = p2p_connection(mn3.node, subversion=version_m3_1)
        p2p_mn3_2 = p2p_connection(mn3.node, subversion=version_m3_2)
        network_thread_start()
        p2p_mn3_1.wait_for_verack()
        p2p_mn3_2.wait_for_verack()
        id_p2p_mn3_1 = get_mininode_id(mn3.node, version_m3_1)
        id_p2p_mn3_2 = get_mininode_id(mn3.node, version_m3_2)
        assert(id_p2p_mn3_1 != id_p2p_mn3_2)
        mnauth(mn3.node, id_p2p_mn3_1, fake_mnauth_1[0], fake_mnauth_1[1])
        mnauth(mn3.node, id_p2p_mn3_2, fake_mnauth_2[0], fake_mnauth_2[1])
        # Now try some {mn1, mn2} - QGETDATA -> mn3 combinations to make sure request limit works connection based
        test_send_from_two_to_one(False, 0, True, 0, True)
        test_send_from_two_to_one(True, 0, True, 25)
        test_send_from_two_to_one(True, 25, False, 25)
        test_send_from_two_to_one(False, 25, True, 25, True)
        test_send_from_two_to_one(True, 25, True, 50)
        test_send_from_two_to_one(True, 50, True, 75)
        test_send_from_two_to_one(True, 50, True, 75, True)
        test_send_from_two_to_one(True, 75, False, 75)
        test_send_from_two_to_one(False, 75, True, None)
        # mn1 should still have a score of 75
        wait_for_banscore(mn3.node, id_p2p_mn3_1, 75)
        # mn2 should be "banned" now
        wait_until(lambda: not p2p_mn3_2.is_connected, timeout=10)
        mn3.node.disconnect_p2ps()
        network_thread_join()
        # Test ban score increase for invalid/unexpected QDATA
        p2p_mn1 = p2p_connection(mn1.node)
        network_thread_start()
        p2p_mn1.wait_for_verack()
        id_p2p_mn1 = get_mininode_id(mn1.node)
        mnauth(mn1.node, id_p2p_mn1, fake_mnauth_1[0], fake_mnauth_1[1])
        wait_for_banscore(mn1.node, id_p2p_mn1, 0)
        qdata_valid = p2p_mn2.get_qdata()
        # - Not requested
        p2p_mn1.send_message(qdata_valid)
        time.sleep(1)
        wait_for_banscore(mn1.node, id_p2p_mn1, 10)
        # - Already received
        force_request_expire()
        assert(mn1.node.quorum("getdata", id_p2p_mn1, 100, quorum_hash, 0x03, mn1.proTxHash))
        p2p_mn1.wait_for_qgetdata()
        p2p_mn1.send_message(qdata_valid)
        time.sleep(1)
        p2p_mn1.send_message(qdata_valid)
        wait_for_banscore(mn1.node, id_p2p_mn1, 20)
        # - Not like requested
        force_request_expire()
        assert(mn1.node.quorum("getdata", id_p2p_mn1, 100, quorum_hash, 0x03, mn1.proTxHash))
        p2p_mn1.wait_for_qgetdata()
        qdata_invalid_request = qdata_valid
        qdata_invalid_request.data_mask = 2
        p2p_mn1.send_message(qdata_invalid_request)
        wait_for_banscore(mn1.node, id_p2p_mn1, 30)
        # - Invalid verification vector
        force_request_expire()
        assert(mn1.node.quorum("getdata", id_p2p_mn1, 100, quorum_hash, 0x03, mn1.proTxHash))
        p2p_mn1.wait_for_qgetdata()
        qdata_invalid_vvec = qdata_valid
        qdata_invalid_vvec.quorum_vvec.pop()
        p2p_mn1.send_message(qdata_invalid_vvec)
        wait_for_banscore(mn1.node, id_p2p_mn1, 40)
        # - Invalid contributions
        force_request_expire()
        assert(mn1.node.quorum("getdata", id_p2p_mn1, 100, quorum_hash, 0x03, mn1.proTxHash))
        p2p_mn1.wait_for_qgetdata()
        qdata_invalid_contribution = qdata_valid
        qdata_invalid_contribution.enc_contributions.pop()
        p2p_mn1.send_message(qdata_invalid_contribution)
        wait_for_banscore(mn1.node, id_p2p_mn1, 50)


if __name__ == '__main__':
    QuorumDataMessagesTest().main()
