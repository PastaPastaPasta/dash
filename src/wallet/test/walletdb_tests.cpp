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

    //! A second wallet holding the same recovery phrase and nothing else, which is exactly what a
    //! wallet restored from that phrase is: the same seed, and no record of the keys the original
    //! wallet handed out
    std::unique_ptr<CWallet> MakeRestoredWallet()
    {
        auto wallet{std::make_unique<CWallet>(m_node.chain.get(), m_coinjoin_loader.get(), "", m_args,
                                              CreateMockWalletDatabase())};
        BOOST_REQUIRE(wallet->LoadWallet() == DBErrors::LOAD_OK);
        SetupHDWallet(*wallet, DASHSYNC_MNEMONIC);
        return wallet;
    }
};

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

//! Nothing is registered on chain yet, so no operator key is taken
static const std::vector<std::vector<unsigned char>> NO_KEYS_IN_USE;

//! Operator public key the way the wallet records it and the masternode list reports it
static std::vector<unsigned char> OperatorPubKeyBytes(const CBLSSecretKey& secret)
{
    return secret.GetPublicKey().ToByteVector(/*specificLegacyScheme=*/false);
}

BOOST_FIXTURE_TEST_CASE(walletdb_masternode_operator_key_derived, DerivingWalletTestingSetup)
{
    SetupHDWallet(m_wallet, DASHSYNC_MNEMONIC);

    CBLSSecretKey first;
    uint32_t first_index{std::numeric_limits<uint32_t>::max()};
    BOOST_REQUIRE(m_wallet.DeriveNextMasternodeOperatorKey(NO_KEYS_IN_USE, first, first_index));
    BOOST_CHECK_EQUAL(first_index, 0U);

    // A derived key is only recorded by its index: the seed regenerates the secret, and
    // storing it again would put a second copy of it on disk for no gain
    BOOST_CHECK_EQUAL(CountRecords(m_wallet, DBKeys::MASTERNODE_OPERATOR_INDEX), 1U);

    const CBLSPublicKey pubkey{first.GetPublicKey()};
    BOOST_CHECK(m_wallet.HaveMasternodeOperatorKey(pubkey));
    CBLSSecretKey read;
    std::string path;
    BOOST_CHECK(m_wallet.GetMasternodeOperatorKey(pubkey, read, &path));
    BOOST_CHECK(read == first);
    BOOST_CHECK_EQUAL(path, "m/9'/1'/3'/3'/0");

    // Running a second masternode off the same wallet must get its own index and its own key:
    // two masternodes sharing an operator key would sign for each other
    CBLSSecretKey second;
    uint32_t second_index{std::numeric_limits<uint32_t>::max()};
    BOOST_REQUIRE(m_wallet.DeriveNextMasternodeOperatorKey(NO_KEYS_IN_USE, second, second_index));
    BOOST_CHECK_EQUAL(second_index, 1U);
    BOOST_CHECK(!(second == first));

    CBLSSecretKey third;
    uint32_t third_index{std::numeric_limits<uint32_t>::max()};
    BOOST_REQUIRE(m_wallet.DeriveNextMasternodeOperatorKey(NO_KEYS_IN_USE, third, third_index));
    BOOST_CHECK_EQUAL(third_index, 2U);
    BOOST_CHECK(!(third == first));
    BOOST_CHECK(!(third == second));
    BOOST_CHECK_EQUAL(CountRecords(m_wallet, DBKeys::MASTERNODE_OPERATOR_INDEX), 3U);

    // Reloading the wallet: a wallet with the same seed that reads the recorded indexes back
    // returns the very same secrets, without any secret having been written to disk
    const auto records{ReadOperatorIndexRecords(m_wallet)};
    BOOST_CHECK_EQUAL(records.size(), 3U);

    const auto reloaded{MakeRestoredWallet()};
    {
        LOCK(reloaded->cs_wallet);
        for (const auto& record : records) {
            BOOST_CHECK(reloaded->LoadMasternodeOperatorIndex(record.first, record.second));
        }
    }
    CBLSSecretKey reloaded_read;
    BOOST_CHECK(reloaded->GetMasternodeOperatorKey(pubkey, reloaded_read));
    BOOST_CHECK(reloaded_read == first);
    BOOST_CHECK(reloaded->GetMasternodeOperatorKey(second.GetPublicKey(), reloaded_read));
    BOOST_CHECK(reloaded_read == second);
}

/**
 * A wallet restored from its recovery phrase has the seed back but no record of the indexes it
 * handed out, so only the keys registered on chain say which ones are taken. Handing out a key
 * one of its own masternodes already uses would make the two sign for each other.
 */
BOOST_FIXTURE_TEST_CASE(walletdb_masternode_operator_key_in_use, DerivingWalletTestingSetup)
{
    SetupHDWallet(m_wallet, DASHSYNC_MNEMONIC);

    CBLSSecretKey registered;
    uint32_t registered_index{std::numeric_limits<uint32_t>::max()};
    BOOST_REQUIRE(m_wallet.DeriveNextMasternodeOperatorKey(NO_KEYS_IN_USE, registered, registered_index));
    BOOST_CHECK_EQUAL(registered_index, 0U);

    const auto restored{MakeRestoredWallet()};
    BOOST_CHECK_EQUAL(CountRecords(*restored, DBKeys::MASTERNODE_OPERATOR_INDEX), 0U);

    const std::vector<std::vector<unsigned char>> in_use{OperatorPubKeyBytes(registered)};
    CBLSSecretKey next;
    uint32_t next_index{std::numeric_limits<uint32_t>::max()};
    BOOST_REQUIRE(restored->DeriveNextMasternodeOperatorKey(in_use, next, next_index));
    BOOST_CHECK_EQUAL(next_index, 1U);
    BOOST_CHECK(!(next == registered));

    // Without the in-use set the restored wallet has nothing to go on and hands out index 0 again,
    // which is exactly the collision the set is there to prevent
    const auto blind{MakeRestoredWallet()};
    CBLSSecretKey collision;
    uint32_t collision_index{std::numeric_limits<uint32_t>::max()};
    BOOST_REQUIRE(blind->DeriveNextMasternodeOperatorKey(NO_KEYS_IN_USE, collision, collision_index));
    BOOST_CHECK_EQUAL(collision_index, 0U);
    BOOST_CHECK(collision == registered);
}

/**
 * Restoring the recovery phrase has to bring back the operator keys of the masternodes the wallet
 * registered, and nothing on disk points at them any more, so they are found by walking the path.
 */
BOOST_FIXTURE_TEST_CASE(walletdb_masternode_operator_key_recovery_walk, DerivingWalletTestingSetup)
{
    SetupHDWallet(m_wallet, DASHSYNC_MNEMONIC);

    CBLSSecretKey first;
    CBLSSecretKey second;
    uint32_t index{std::numeric_limits<uint32_t>::max()};
    BOOST_REQUIRE(m_wallet.DeriveNextMasternodeOperatorKey(NO_KEYS_IN_USE, first, index));
    BOOST_REQUIRE(m_wallet.DeriveNextMasternodeOperatorKey(NO_KEYS_IN_USE, second, index));

    const auto restored{MakeRestoredWallet()};
    BOOST_CHECK_EQUAL(CountRecords(*restored, DBKeys::MASTERNODE_OPERATOR_INDEX), 0U);
    // Nothing says this key is the wallet's, but the wallet can derive, so it says it might be
    BOOST_CHECK(restored->HaveMasternodeOperatorKey(second.GetPublicKey()));

    CBLSSecretKey found;
    std::string path;
    BOOST_CHECK(restored->GetMasternodeOperatorKey(second.GetPublicKey(), found, &path));
    BOOST_CHECK(found == second);
    BOOST_CHECK_EQUAL(path, "m/9'/1'/3'/3'/1");

    // The walk records what it found, so the next lookup goes straight to the index
    BOOST_CHECK_EQUAL(CountRecords(*restored, DBKeys::MASTERNODE_OPERATOR_INDEX), 1U);
    const auto records{ReadOperatorIndexRecords(*restored)};
    BOOST_CHECK_EQUAL(records.size(), 1U);
    const auto record{records.find(OperatorPubKeyBytes(second))};
    BOOST_REQUIRE(record != records.end());
    BOOST_CHECK_EQUAL(record->second, 1U);

    // The walk is bounded, so a key far past the end of the range is reported as not ours instead
    // of walking forever
    CBLSSecretKey far_away;
    BOOST_REQUIRE(restored->DeriveMasternodeOperatorKey(/*index=*/1000, far_away));
    CBLSSecretKey not_found;
    BOOST_CHECK(!restored->GetMasternodeOperatorKey(far_away.GetPublicKey(), not_found));
}

BOOST_FIXTURE_TEST_CASE(walletdb_masternode_operator_key_derive_locked, DerivingWalletTestingSetup)
{
    SetupHDWallet(m_wallet, DASHSYNC_MNEMONIC);
    BOOST_REQUIRE(m_wallet.EncryptWallet("passphrase"));
    BOOST_REQUIRE(m_wallet.IsLocked());

    // The wallet still knows it could derive, it just cannot reach the seed while locked, so the
    // caller is told to unlock instead of being told the key is not theirs
    BOOST_CHECK(m_wallet.CanDeriveMasternodeOperatorKey());
    SecureVector seed;
    BOOST_CHECK(!m_wallet.GetHDSeed(seed));
    CBLSSecretKey locked;
    BOOST_CHECK(!m_wallet.DeriveMasternodeOperatorKey(/*index=*/0, locked));
    uint32_t index{std::numeric_limits<uint32_t>::max()};
    BOOST_CHECK(!m_wallet.DeriveNextMasternodeOperatorKey(NO_KEYS_IN_USE, locked, index));
    BOOST_CHECK_EQUAL(CountRecords(m_wallet, DBKeys::MASTERNODE_OPERATOR_INDEX), 0U);

    BOOST_REQUIRE(m_wallet.Unlock("passphrase"));
    BOOST_CHECK(m_wallet.DeriveNextMasternodeOperatorKey(NO_KEYS_IN_USE, locked, index));
    BOOST_CHECK_EQUAL(index, 0U);
    // Encryption must not have disturbed the seed, so the vector still holds
    BOOST_CHECK_EQUAL(HexStr(locked.ToByteVector(/*specificLegacyScheme=*/false)), DASHSYNC_OPERATOR_SECRET);
}

//! A wallet without an HD seed has no operator keys at all, and says so without deriving anything
BOOST_AUTO_TEST_CASE(walletdb_masternode_operator_key_no_seed)
{
    BOOST_CHECK(!m_wallet.CanDeriveMasternodeOperatorKey());
    SecureVector seed;
    BOOST_CHECK(!m_wallet.GetHDSeed(seed));

    CBLSSecretKey secret;
    secret.MakeNewKey();
    BOOST_CHECK(!m_wallet.HaveMasternodeOperatorKey(secret.GetPublicKey()));
    CBLSSecretKey read;
    BOOST_CHECK(!m_wallet.GetMasternodeOperatorKey(secret.GetPublicKey(), read));

    uint32_t index{std::numeric_limits<uint32_t>::max()};
    BOOST_CHECK(!m_wallet.DeriveNextMasternodeOperatorKey(NO_KEYS_IN_USE, secret, index));
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace wallet
