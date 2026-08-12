// Copyright (c) 2012-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/setup_common.h>
#include <bls/bls.h>
#include <clientversion.h>
#include <streams.h>
#include <uint256.h>
#include <wallet/hdchain.h>
#include <wallet/test/wallet_test_fixture.h>
#include <wallet/walletdb.h>

#include <boost/test/unit_test.hpp>

namespace wallet {
BOOST_FIXTURE_TEST_SUITE(walletdb_tests, WalletTestingSetup)

BOOST_AUTO_TEST_CASE(walletdb_readkeyvalue)
{
    /**
     * When ReadKeyValue() reads from either a "key" or "wkey" it first reads the CDataStream steam into a
     * CPrivKey or CWalletKey respectively and then reads a hash of the pubkey and privkey into a uint256.
     * Wallets from 0.8 or before do not store the pubkey/privkey hash, trying to read the hash from old
     * wallets throws an exception, for backwards compatibility this read is wrapped in a try block to
     * silently fail. The test here makes sure the type of exception thrown from CDataStream::read()
     * matches the type we expect, otherwise we need to update the "key"/"wkey" exception type caught.
     */
    CDataStream ssValue(SER_DISK, CLIENT_VERSION);
    uint256 dummy;
    BOOST_CHECK_THROW(ssValue >> dummy, std::ios_base::failure);
}

// Helper: build a key/value stream pair for an HD chain DB record and run ReadKeyValue.
static bool TryReadHDChainRecord(CWallet& wallet, const std::string& dbKey, bool fCrypted, std::string& strErrOut)
{
    CHDChain chain;
    chain.SetCrypted(fCrypted);

    CDataStream ssKey(SER_DISK, CLIENT_VERSION);
    CDataStream ssValue(SER_DISK, CLIENT_VERSION);
    ssKey << dbKey;
    ssValue << chain;

    std::string strType;
    LOCK(wallet.cs_wallet);
    return ReadKeyValue(&wallet, ssKey, ssValue, strType, strErrOut);
}

BOOST_AUTO_TEST_CASE(walletdb_hdchain_type_mismatch)
{
    // Regression: a wallet record claiming HDCHAIN but carrying a crypted CHDChain
    // (or vice versa) used to trigger an assert and abort the process. It must now
    // surface as a graceful load error.
    std::string strErr;

    BOOST_CHECK(!TryReadHDChainRecord(m_wallet, DBKeys::HDCHAIN, /*fCrypted=*/true, strErr));
    BOOST_CHECK_EQUAL(strErr, "Error reading wallet database: HD chain type mismatch");

    strErr.clear();
    BOOST_CHECK(!TryReadHDChainRecord(m_wallet, DBKeys::CRYPTED_HDCHAIN, /*fCrypted=*/false, strErr));
    BOOST_CHECK_EQUAL(strErr, "Error reading wallet database: HD chain type mismatch");
}

//! Number of records of the given type currently in the wallet database
static size_t CountRecords(CWallet& wallet, const std::string& type)
{
    std::unique_ptr<DatabaseBatch> batch{wallet.GetDatabase().MakeBatch()};
    BOOST_REQUIRE(batch);
    BOOST_REQUIRE(batch->StartCursor());
    size_t count{0};
    while (true) {
        CDataStream key(SER_DISK, CLIENT_VERSION);
        CDataStream value(SER_DISK, CLIENT_VERSION);
        bool complete{false};
        BOOST_REQUIRE(batch->ReadAtCursor(key, value, complete));
        if (complete) break;
        std::string record_type;
        key >> record_type;
        if (record_type == type) ++count;
    }
    batch->CloseCursor();
    return count;
}

BOOST_AUTO_TEST_CASE(walletdb_masternode_operator_key)
{
    CBLSSecretKey secret;
    secret.MakeNewKey();
    const CBLSPublicKey pubkey{secret.GetPublicKey()};

    BOOST_CHECK(m_wallet.CanStoreMasternodeOperatorKey());
    BOOST_CHECK(!m_wallet.HaveMasternodeOperatorKey(pubkey));
    BOOST_CHECK(m_wallet.AddMasternodeOperatorKey(secret));
    BOOST_CHECK(m_wallet.HaveMasternodeOperatorKey(pubkey));
    // Storing the same key again is a no-op success, so a retry cannot fail
    BOOST_CHECK(m_wallet.AddMasternodeOperatorKey(secret));

    CBLSSecretKey read;
    BOOST_CHECK(m_wallet.GetMasternodeOperatorKey(pubkey, read));
    BOOST_CHECK(read == secret);

    // While the wallet is unencrypted the secret is stored in the clear, as
    // private keys are
    BOOST_CHECK_EQUAL(CountRecords(m_wallet, DBKeys::MASTERNODE_OPERATOR_KEY), 1U);
    BOOST_CHECK_EQUAL(CountRecords(m_wallet, DBKeys::CRYPTED_MASTERNODE_OPERATOR_KEY), 0U);

    // Encrypting the wallet must convert it: leaving a plaintext record behind
    // would leak the operator key of every masternode registered before the
    // user encrypted their wallet
    BOOST_CHECK(m_wallet.EncryptWallet("passphrase"));
    BOOST_CHECK_EQUAL(CountRecords(m_wallet, DBKeys::MASTERNODE_OPERATOR_KEY), 0U);
    BOOST_CHECK_EQUAL(CountRecords(m_wallet, DBKeys::CRYPTED_MASTERNODE_OPERATOR_KEY), 1U);
    BOOST_CHECK(m_wallet.HaveMasternodeOperatorKey(pubkey));

    // A locked wallet neither hands out nor accepts secrets
    BOOST_CHECK(m_wallet.IsLocked());
    CBLSSecretKey locked_read;
    BOOST_CHECK(!m_wallet.GetMasternodeOperatorKey(pubkey, locked_read));
    CBLSSecretKey other;
    other.MakeNewKey();
    BOOST_CHECK(!m_wallet.AddMasternodeOperatorKey(other));

    BOOST_CHECK(m_wallet.Unlock("passphrase"));
    CBLSSecretKey unlocked_read;
    BOOST_CHECK(m_wallet.GetMasternodeOperatorKey(pubkey, unlocked_read));
    BOOST_CHECK(unlocked_read == secret);
    BOOST_CHECK(m_wallet.AddMasternodeOperatorKey(other));
    BOOST_CHECK_EQUAL(CountRecords(m_wallet, DBKeys::CRYPTED_MASTERNODE_OPERATOR_KEY), 2U);
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace wallet
