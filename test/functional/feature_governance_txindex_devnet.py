#!/usr/bin/env python3
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test which indexes a node needs to run governance validation outside regtest.

Regtest exempts nodes from the index check, so it is exercised on devnet here.
"""

from test_framework.governance import EXPECTED_STDERR_NO_GOV
from test_framework.test_framework import BitcoinTestFramework


class GovernanceTxindexDevnetTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.chain = "devnet"
        self.num_nodes = 1

    def run_test(self):
        self.stop_node(0)

        self.log.info("Test that governance validation is refused without either index")
        self.nodes[0].assert_start_raises_init_error(
            extra_args=["-txindex=0", "-govcollateralindex=0"],
            expected_msg="Error: Governance validation needs -txindex or -govcollateralindex. Either start with "
                         "-disablegovernance command line switch or enable one of these indexes.")

        self.log.info("Test that -txindex=0 switches governance over to -govcollateralindex")
        self.start_node(0, extra_args=["-txindex=0"])
        assert "txindex" not in self.nodes[0].getindexinfo()
        assert "govcollateralindex" in self.nodes[0].getindexinfo()
        self.stop_node(0)

        self.log.info("Test that -txindex=0 is allowed together with -disablegovernance, without either index")
        self.start_node(0, extra_args=["-txindex=0", "-disablegovernance"])
        assert "govcollateralindex" not in self.nodes[0].getindexinfo()
        self.stop_node(0, expected_stderr=EXPECTED_STDERR_NO_GOV)


if __name__ == '__main__':
    GovernanceTxindexDevnetTest().main()
