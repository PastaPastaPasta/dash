#!/usr/bin/env python3
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test governance on a pruned node.

A pruned node can't run -txindex, so it looks proposal collateral up in -govcollateralindex instead.
Node 0 is a full node, node 1 is pruned and nodes 2-4 are masternodes.
"""

import os
import shutil

from test_framework.governance import (
    EXPECTED_STDERR_GOV_PRUNED_TOO_FAR,
    EXPECTED_STDERR_NO_GOV,
    have_trigger_for_height,
    prepare_object,
)
from test_framework.messages import CBlock, from_hex, uint256_to_string
from test_framework.test_framework import DashTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error, force_finish_mnsync

BUDGET_ARGS = ["-budgetparams=10:10:10"]
PRUNED_ARGS = BUDGET_ARGS + ["-prune=550", "-fastprune", "-debug=gobject", "-debug=mnpayments"]
SB_CYCLE = 20
SB_MATURITY_WINDOW = 10
MIN_BLOCKS_TO_KEEP = 288


class GovernancePrunedTest(DashTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser)

    def set_test_params(self):
        self.set_dash_test_params(5, 3, [BUDGET_ARGS, PRUNED_ARGS] + [BUDGET_ARGS] * 3)

    def prepare_proposal(self, node, name):
        return prepare_object(node, 1, uint256_to_string(0), self.mocktime, 1, name, 1, node.getnewaddress())

    def submit(self, node, proposal):
        return node.gobject("submit", "0", 1, proposal["createdAt"], proposal["hex"], proposal["collateralHash"])

    def mine(self, num_blocks):
        # The masternodes follow on their own until they are cut off the network once the superblock is checked
        self.bump_mocktime(num_blocks)
        self.generate(self.nodes[0], num_blocks, sync_fun=lambda: self.sync_blocks(self.nodes[0:2]))

    def run_test(self):
        full_node = self.nodes[0]
        pruned_node = self.nodes[1]
        # Otherwise blocks leave out transactions until they are InstantSend locked or old enough
        full_node.sporkupdate("SPORK_2_INSTANTSEND_ENABLED", 4070908800)
        self.wait_for_sporks_same()

        self.log.info("A pruned node runs governance, looking collateral up in -govcollateralindex")
        assert pruned_node.getblockchaininfo()["pruned"]
        indexes = pruned_node.getindexinfo()
        assert "txindex" not in indexes
        assert indexes["govcollateralindex"]["synced"]
        assert pruned_node.mnsync("status")["IsSynced"]

        self.log.info("Proposals created on either node reach the other one")
        from_full_node = self.prepare_proposal(full_node, "from_full_node")
        # Its collateral is mined now but the proposal is only submitted once that block is pruned
        submitted_late = self.prepare_proposal(full_node, "submitted_late")
        full_node.sendtoaddress(pruned_node.getnewaddress(), 10)
        self.mine(1)
        # getbalance waits for the wallet to see the block, gobject prepare doesn't
        assert_equal(pruned_node.getbalance(), 10)
        from_pruned_node = self.prepare_proposal(pruned_node, "from_pruned_node")
        collateral_height = full_node.getblockcount()
        self.sync_mempools([full_node, pruned_node])
        self.mine(6)

        self.log.info("Collateral errors say what is wrong with the collateral")
        assert_raises_rpc_error(-32603, "Invalid proposal collateral: Couldn't find opReturn", pruned_node.gobject,
                                "submit", "0", 1, submitted_late["createdAt"], submitted_late["hex"],
                                from_full_node["collateralHash"])

        from_full_node_hash = self.submit(full_node, from_full_node)
        from_pruned_node_hash = self.submit(pruned_node, from_pruned_node)
        for node in [full_node, pruned_node]:
            self.wait_until(lambda: len(node.gobject("list", "valid", "proposals")) == 2)

        self.log.info("Votes reach the pruned node")
        for proposal_hash in [from_full_node_hash, from_pruned_node_hash]:
            full_node.gobject("vote-many", proposal_hash, "funding", "yes")
            self.wait_until(lambda: pruned_node.gobject("get", proposal_hash)["FundingResult"]["YesCount"] == self.mn_count)

        self.log.info("The pruned node validates superblock payments in full")
        sb_height = ((full_node.getblockcount() + SB_MATURITY_WINDOW) // SB_CYCLE + 1) * SB_CYCLE
        # The masternode paid by the first block of the maturity window creates the trigger, the others vote for it
        # once they see the next block
        self.mine(sb_height - SB_MATURITY_WINDOW - full_node.getblockcount())
        self.wait_until(lambda: have_trigger_for_height(self.nodes, sb_height))
        self.mine(1)

        def trigger_votes(node):
            return [trigger["AbsoluteYesCount"] for trigger in node.gobject("list", "valid", "triggers").values()]
        self.wait_until(lambda: all(trigger_votes(node) == [self.mn_count] for node in self.nodes))
        self.mine(sb_height - 1 - full_node.getblockcount())
        self.wait_until(lambda: all(len(node.getblocktemplate()["superblock"]) > 0 for node in self.nodes))

        # A superblock whose miner keeps the budget stays within the superblock limits, so only a node that knows
        # the trigger can tell it is invalid
        self.bump_mocktime(1)
        payment_scripts = {bytes.fromhex(payment["script"]) for payment in full_node.getblocktemplate()["superblock"]}
        miner_address = full_node.getnewaddress()
        miner_script = bytes.fromhex(full_node.getaddressinfo(miner_address)["scriptPubKey"])
        stolen_budget = from_hex(CBlock(), self.generateblock(full_node, output=miner_address, transactions=[], submit=False)["hex"])
        coinbase = stolen_budget.vtx[0]
        budget = sum(txout.nValue for txout in coinbase.vout if txout.scriptPubKey in payment_scripts)
        assert budget > 0
        coinbase.vout = [txout for txout in coinbase.vout if txout.scriptPubKey not in payment_scripts]
        next(txout for txout in coinbase.vout if txout.scriptPubKey == miner_script).nValue += budget
        coinbase.rehash()
        stolen_budget.hashMerkleRoot = stolen_budget.calc_merkle_root()
        stolen_budget.solve()
        assert_equal(pruned_node.submitblock(stolen_budget.serialize().hex()), "bad-cb-amount")

        with pruned_node.assert_debug_log(["-- nOutputs = "], ["max bounds only"]):
            self.generate(full_node, 1)
        assert_equal(pruned_node.getblockcount(), sb_height)
        coinbase_outputs = pruned_node.getblock(pruned_node.getbestblockhash(), 2)["tx"][0]["vout"]
        paid_addresses = {txout["scriptPubKey"].get("address") for txout in coinbase_outputs}
        for proposal in [from_full_node, from_pruned_node]:
            assert proposal["data"]["payment_address"] in paid_addresses

        # Otherwise the masternodes keep creating triggers, and the bursts of blocks below may leave out a superblock
        # that one node already knows the trigger for but another doesn't
        for mn in self.mninfo:
            self.isolate_node(mn.nodeIdx)

        self.log.info("Collateral can still be found after its block is pruned")
        # Each -fastprune block file holds dozens of blocks, so mine enough to prune the collateral's whole file
        self.mine(MIN_BLOCKS_TO_KEEP + 150)
        pruned_node.pruneblockchain(pruned_node.getblockcount() - MIN_BLOCKS_TO_KEEP)
        assert pruned_node.getblockchaininfo()["pruneheight"] > collateral_height
        assert_raises_rpc_error(-1, "Block not available (pruned data)", pruned_node.getblock,
                                pruned_node.getblockhash(collateral_height))
        submitted_late_hash = self.submit(pruned_node, submitted_late)
        self.wait_until(lambda: submitted_late_hash in full_node.gobject("list", "valid", "proposals"))

        self.log.info("Governance objects survive a restart of the pruned node")
        self.restart_node(1)
        force_finish_mnsync(pruned_node)
        self.connect_nodes(0, 1)
        assert_equal(len(pruned_node.gobject("list", "valid", "proposals")), 3)

        self.log.info("An index that misses blocks pruned while governance was off leaves governance off")
        self.restart_node(1, extra_args=PRUNED_ARGS + ["-disablegovernance"])
        index_height = pruned_node.getblockcount()
        self.connect_nodes(0, 1)
        self.mine(MIN_BLOCKS_TO_KEEP + 150)
        pruned_node.pruneblockchain(pruned_node.getblockcount() - MIN_BLOCKS_TO_KEEP)
        assert pruned_node.getblockchaininfo()["pruneheight"] > index_height + 1
        self.restart_node(1, extra_args=PRUNED_ARGS, expected_stderr=EXPECTED_STDERR_NO_GOV)
        assert "govcollateralindex" not in pruned_node.getindexinfo()
        self.stop_node(1, expected_stderr=EXPECTED_STDERR_GOV_PRUNED_TOO_FAR)

        self.log.info("A node that pruned before it had the index leaves governance off instead of failing to start")
        shutil.rmtree(os.path.join(pruned_node.chain_path, "indexes", "govcollateralindex"))
        self.start_node(1)
        assert "govcollateralindex" not in pruned_node.getindexinfo()
        assert_raises_rpc_error(-1, "Preparing a proposal now would burn its fee", pruned_node.gobject, "prepare",
                                "0", 1, self.mocktime, from_pruned_node["hex"])
        assert_raises_rpc_error(-1, "Governance is disabled on this node", pruned_node.gobject, "submit",
                                "0", 1, submitted_late["createdAt"], submitted_late["hex"], submitted_late["collateralHash"])
        self.stop_node(1, expected_stderr=EXPECTED_STDERR_GOV_PRUNED_TOO_FAR)

        self.log.info("A -reindex downloads the chain again, builds the index and turns governance back on")
        self.start_node(1, extra_args=PRUNED_ARGS + ["-reindex"])
        self.connect_nodes(0, 1)
        self.sync_blocks(self.nodes[0:2])
        assert pruned_node.getblockchaininfo()["pruned"]
        self.wait_until(lambda: pruned_node.getindexinfo()["govcollateralindex"]["synced"])
        self.bump_mocktime(5 * 60)

        def governance_synced():
            self.bump_mocktime(1)
            return pruned_node.mnsync("status")["IsSynced"]
        self.wait_until(governance_synced)
        self.wait_until(lambda: len(pruned_node.gobject("list", "valid", "proposals")) == 3)


if __name__ == '__main__':
    GovernancePrunedTest().main()
