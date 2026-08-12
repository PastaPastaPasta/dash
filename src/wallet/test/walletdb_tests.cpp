// Copyright (c) 2012-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/setup_common.h>
#include <bls/bls.h>
#include <clientversion.h>
#include <streams.h>
#include <uint256.h>
#include <util/strencodings.h>
#include <wallet/hdchain.h>
#include <wallet/scriptpubkeyman.h>
#include <wallet/test/wallet_test_fixture.h>
#include <wallet/wallet.h>
#include <wallet/walletdb.h>

#include <boost/test/unit_test.hpp>

#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

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

/**
 * The masternode operator key derivation path uses coin type 5 on mainnet and 1 on every
 * other network, so a wallet on regtest derives exactly what DashSync derives on testnet.
 * That is what makes the known-answer test below applicable.
 */
struct DerivingWalletTestingSetup : public WalletTestingSetup {
    DerivingWalletTestingSetup() : WalletTestingSetup(CBaseChainParams::REGTEST)
    {
        // None of these tests spend, and a default sized keypool costs a couple of thousand
        // key derivations whenever the wallet tops up
        gArgs.ForceSetArg("-keypool", "1");
    }
};

/**
 * Recovery phrase from DashSync's testCollateralProviderRegistrationTransaction
 * (Example/Tests/DSProviderTransactionsTests.m). That test builds a ProRegTx whose payload
 * carries the operator public key this phrase derives at index 0 on testnet:
 *
 *   0100 0000 0000 <collateral> ... <keyIDOwner>
 *   157b10706659e25eb362b5d902d809f9160b1688e201ee6e94b40f9b5062d7074683ef05a2d5efb7793c47059c878dfa
 *
 * The payload serializes the operator key in the legacy scheme (its operatorKeyVersion is 1),
 * which is why the legacy serialization is what gets compared here. Reproducing these bytes is
 * the only thing that proves Dash Core and DashSync agree on the derivation, so do not
 * "fix" this vector: if it fails, the derivation changed and mobile compatibility is broken.
 */
const SecureString DASHSYNC_MNEMONIC{
    "enemy check owner stumble unaware debris suffer peanut good fabric bleak outside"};
const std::string DASHSYNC_OPERATOR_SECRET{
    "344e7bf67fddf1c2d2f629f2392ce2b4af99393cd8a4c70d4bbaef7aaf4c364b"};
const std::string DASHSYNC_OPERATOR_PUBKEY_LEGACY{
    "157b10706659e25eb362b5d902d809f9160b1688e201ee6e94b40f9b5062d7074683ef05a2d5efb7793c47059c878dfa"};

//! Give a wallet the legacy HD chain a known recovery phrase produces, the way loading a
//! wallet file does. Loading it rather than generating it keeps the keypool out of the way.
static void SetupHDWallet(CWallet& wallet, const SecureString& mnemonic)
{
    CHDChain chain;
    BOOST_REQUIRE(chain.SetMnemonic(mnemonic, /*ssMnemonicPassphrase=*/"", /*fUpdateID=*/true));
    chain.AddAccount(); // as GenerateNewHDChain() does, so key derivation off this chain works

    LOCK(wallet.cs_wallet);
    LegacyScriptPubKeyMan* spk_man{wallet.GetOrCreateLegacyScriptPubKeyMan()};
    BOOST_REQUIRE(spk_man != nullptr);
    BOOST_REQUIRE(spk_man->LoadHDChain(chain));
}

//! Derived operator key records as they sit in the database, keyed by public key
static std::map<std::vector<unsigned char>, uint32_t> ReadOperatorIndexRecords(CWallet& wallet)
{
    std::unique_ptr<DatabaseBatch> batch{wallet.GetDatabase().MakeBatch()};
    BOOST_REQUIRE(batch);
    BOOST_REQUIRE(batch->StartCursor());
    std::map<std::vector<unsigned char>, uint32_t> records;
    while (true) {
        CDataStream key(SER_DISK, CLIENT_VERSION);
        CDataStream value(SER_DISK, CLIENT_VERSION);
        bool complete{false};
        BOOST_REQUIRE(batch->ReadAtCursor(key, value, complete));
        if (complete) break;
        std::string record_type;
        key >> record_type;
        if (record_type != DBKeys::MASTERNODE_OPERATOR_INDEX) continue;
        std::vector<unsigned char> pubkey;
        uint32_t index{0};
        key >> pubkey;
        value >> index;
        records.emplace(pubkey, index);
    }
    batch->CloseCursor();
    return records;
}

BOOST_FIXTURE_TEST_CASE(walletdb_masternode_operator_key_dashsync_vector, DerivingWalletTestingSetup)
{
    SetupHDWallet(m_wallet, DASHSYNC_MNEMONIC);
    BOOST_CHECK(m_wallet.CanDeriveMasternodeOperatorKey());
    BOOST_CHECK_EQUAL(m_wallet.GetMasternodeOperatorKeyPath(0), "m/9'/1'/3'/3'/0");

    CBLSSecretKey secret;
    BOOST_REQUIRE(m_wallet.DeriveMasternodeOperatorKey(/*index=*/0, secret));
    BOOST_CHECK_EQUAL(HexStr(secret.ToByteVector(/*specificLegacyScheme=*/false)), DASHSYNC_OPERATOR_SECRET);
    BOOST_CHECK_EQUAL(HexStr(secret.GetPublicKey().ToByteVector(/*specificLegacyScheme=*/true)),
                      DASHSYNC_OPERATOR_PUBKEY_LEGACY);

    // Deriving the same index twice gives the same key, which is what lets a wallet
    // restored from the phrase alone recover its operator keys
    CBLSSecretKey again;
    BOOST_REQUIRE(m_wallet.DeriveMasternodeOperatorKey(/*index=*/0, again));
    BOOST_CHECK(again == secret);

    CBLSSecretKey next;
    BOOST_REQUIRE(m_wallet.DeriveMasternodeOperatorKey(/*index=*/1, next));
    BOOST_CHECK(!(next == secret));
}

BOOST_FIXTURE_TEST_CASE(walletdb_masternode_operator_key_derived, DerivingWalletTestingSetup)
{
    SetupHDWallet(m_wallet, DASHSYNC_MNEMONIC);

    CBLSSecretKey first;
    uint32_t first_index{std::numeric_limits<uint32_t>::max()};
    BOOST_REQUIRE(m_wallet.DeriveNextMasternodeOperatorKey(first, first_index));
    BOOST_CHECK_EQUAL(first_index, 0U);

    // A derived key is only recorded by its index: the seed regenerates the secret, and
    // storing it again would put a second copy of it on disk for no gain
    BOOST_CHECK_EQUAL(CountRecords(m_wallet, DBKeys::MASTERNODE_OPERATOR_INDEX), 1U);
    BOOST_CHECK_EQUAL(CountRecords(m_wallet, DBKeys::MASTERNODE_OPERATOR_KEY), 0U);
    BOOST_CHECK_EQUAL(CountRecords(m_wallet, DBKeys::CRYPTED_MASTERNODE_OPERATOR_KEY), 0U);

    const CBLSPublicKey pubkey{first.GetPublicKey()};
    BOOST_CHECK(m_wallet.HaveMasternodeOperatorKey(pubkey));
    CBLSSecretKey read;
    BOOST_CHECK(m_wallet.GetMasternodeOperatorKey(pubkey, read));
    BOOST_CHECK(read == first);

    // The next derive must not hand out the same index again
    CBLSSecretKey second;
    uint32_t second_index{std::numeric_limits<uint32_t>::max()};
    BOOST_REQUIRE(m_wallet.DeriveNextMasternodeOperatorKey(second, second_index));
    BOOST_CHECK_EQUAL(second_index, 1U);
    BOOST_CHECK(!(second == first));
    BOOST_CHECK_EQUAL(m_wallet.ListMasternodeOperatorKeys().size(), 2U);

    // Reloading the wallet: a wallet with the same seed that reads the recorded indexes back
    // returns the very same secrets, without any secret having been written to disk
    const auto records{ReadOperatorIndexRecords(m_wallet)};
    BOOST_CHECK_EQUAL(records.size(), 2U);

    CWallet reloaded{m_node.chain.get(), m_coinjoin_loader.get(), "", m_args, CreateMockWalletDatabase()};
    reloaded.LoadWallet();
    SetupHDWallet(reloaded, DASHSYNC_MNEMONIC);
    {
        LOCK(reloaded.cs_wallet);
        for (const auto& record : records) {
            BOOST_CHECK(reloaded.LoadMasternodeOperatorIndex(record.first, record.second));
        }
    }
    CBLSSecretKey reloaded_read;
    BOOST_CHECK(reloaded.GetMasternodeOperatorKey(pubkey, reloaded_read));
    BOOST_CHECK(reloaded_read == first);
    BOOST_CHECK(reloaded.GetMasternodeOperatorKey(second.GetPublicKey(), reloaded_read));
    BOOST_CHECK(reloaded_read == second);
}

BOOST_FIXTURE_TEST_CASE(walletdb_masternode_operator_key_derive_locked, DerivingWalletTestingSetup)
{
    SetupHDWallet(m_wallet, DASHSYNC_MNEMONIC);
    BOOST_REQUIRE(m_wallet.EncryptWallet("passphrase"));
    BOOST_REQUIRE(m_wallet.IsLocked());

    // The wallet still knows it could derive, it just cannot reach the seed while locked, so
    // the caller is told to unlock instead of being pushed onto the random-key path
    BOOST_CHECK(m_wallet.CanDeriveMasternodeOperatorKey());
    SecureVector seed;
    BOOST_CHECK(!m_wallet.GetHDSeed(seed));
    CBLSSecretKey locked;
    BOOST_CHECK(!m_wallet.DeriveMasternodeOperatorKey(/*index=*/0, locked));
    uint32_t index{std::numeric_limits<uint32_t>::max()};
    BOOST_CHECK(!m_wallet.DeriveNextMasternodeOperatorKey(locked, index));
    BOOST_CHECK_EQUAL(CountRecords(m_wallet, DBKeys::MASTERNODE_OPERATOR_INDEX), 0U);

    BOOST_REQUIRE(m_wallet.Unlock("passphrase"));
    BOOST_CHECK(m_wallet.DeriveNextMasternodeOperatorKey(locked, index));
    BOOST_CHECK_EQUAL(index, 0U);
    // Encryption must not have disturbed the seed, so the vector still holds
    BOOST_CHECK_EQUAL(HexStr(locked.ToByteVector(/*specificLegacyScheme=*/false)), DASHSYNC_OPERATOR_SECRET);
}

//! A wallet without an HD seed cannot derive, and must keep the stored-random-key path
BOOST_AUTO_TEST_CASE(walletdb_masternode_operator_key_no_seed)
{
    BOOST_CHECK(!m_wallet.CanDeriveMasternodeOperatorKey());
    BOOST_CHECK(m_wallet.CanStoreMasternodeOperatorKey());
    SecureVector seed;
    BOOST_CHECK(!m_wallet.GetHDSeed(seed));
    CBLSSecretKey secret;
    uint32_t index{std::numeric_limits<uint32_t>::max()};
    BOOST_CHECK(!m_wallet.DeriveNextMasternodeOperatorKey(secret, index));
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace wallet
