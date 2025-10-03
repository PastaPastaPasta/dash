// Copyright (c) 2014-2025 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_COINJOIN_PARAMS_H
#define BITCOIN_COINJOIN_PARAMS_H

// Timeouts used by CoinJoin queue and signing, kept minimal to avoid heavy deps
static constexpr int COINJOIN_QUEUE_TIMEOUT = 30;
static constexpr int COINJOIN_SIGNING_TIMEOUT = 15;

#endif // BITCOIN_COINJOIN_PARAMS_H


