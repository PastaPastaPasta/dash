// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NODE_UTXO_SNAPSHOT_H
#define BITCOIN_NODE_UTXO_SNAPSHOT_H

#include <protocol.h>
#include <serialize.h>
#include <tinyformat.h>
#include <uint256.h>
#include <util/fs.h>
#include <util/strencodings.h>
#include <validation.h>

#include <algorithm>
#include <array>
#include <optional>
#include <set>

// UTXO set snapshot magic bytes.
static constexpr std::array<uint8_t, 5> SNAPSHOT_MAGIC_BYTES = {'u', 't', 'x', 'o', 0xff};

namespace node {
//! Metadata describing a serialized version of a UTXO set from which an
//! assumeutxo Chainstate can be constructed. All fields come from an untrusted
//! file and must be validated before use.
class SnapshotMetadata
{
    inline static const uint16_t VERSION{2};
    const std::set<uint16_t> m_supported_versions{VERSION};
    std::array<uint8_t, CMessageHeader::MESSAGE_START_SIZE> m_network_magic;

public:
    //! The hash of the block that reflects the tip of the chain for the
    //! UTXO set contained in this snapshot.
    uint256 m_base_blockhash;

    //! The number of coins in the UTXO set contained in this snapshot. Used
    //! during snapshot load to estimate progress of UTXO set reconstruction.
    uint64_t m_coins_count = 0;

    SnapshotMetadata(
        const CMessageHeader::MessageStartChars& network_magic) {
            std::copy(std::begin(network_magic), std::end(network_magic), m_network_magic.begin());
        }
    SnapshotMetadata(
        const CMessageHeader::MessageStartChars& network_magic,
        const uint256& base_blockhash,
        uint64_t coins_count) :
            m_base_blockhash(base_blockhash),
            m_coins_count(coins_count) {
                std::copy(std::begin(network_magic), std::end(network_magic), m_network_magic.begin());
            }

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        s << SNAPSHOT_MAGIC_BYTES;
        s << VERSION;
        s << m_network_magic;
        s << m_base_blockhash;
        s << m_coins_count;
    }

    template <typename Stream>
    void Unserialize(Stream& s)
    {
        std::array<uint8_t, SNAPSHOT_MAGIC_BYTES.size()> snapshot_magic;
        s >> snapshot_magic;
        if (snapshot_magic != SNAPSHOT_MAGIC_BYTES) {
            throw std::ios_base::failure("Invalid UTXO set snapshot magic bytes. Please check if this is indeed a snapshot file or if you are using an outdated snapshot format.");
        }

        uint16_t version;
        s >> version;
        if (!m_supported_versions.count(version)) {
            throw std::ios_base::failure(strprintf("Version of snapshot %s does not match any of the supported versions.", version));
        }

        std::array<uint8_t, CMessageHeader::MESSAGE_START_SIZE> message;
        s >> message;
        if (message != m_network_magic) {
            throw std::ios_base::failure(strprintf(
                "The network magic of the snapshot (%s) does not match the network magic of this node (%s).",
                HexStr(message), HexStr(m_network_magic)));
        }

        s >> m_base_blockhash;
        s >> m_coins_count;
    }
};

//! The file in the snapshot chainstate dir which stores the base blockhash. This is
//! needed to reconstruct snapshot chainstates on init.
//!
//! Because we only allow loading a single snapshot at a time, there will only be one
//! chainstate directory with this filename present within it.
const fs::path SNAPSHOT_BLOCKHASH_FILENAME{"base_blockhash"};

//! Write out the blockhash of the snapshot base block that was used to construct
//! this chainstate. This value is read in during subsequent initializations and
//! used to reconstruct snapshot-based chainstates.
bool WriteSnapshotBaseBlockhash(Chainstate& snapshot_chainstate)
    EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

//! Read the blockhash of the snapshot base block that was used to construct the
//! chainstate.
std::optional<uint256> ReadSnapshotBaseBlockhash(fs::path chaindir)
    EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

//! Suffix appended to the chainstate (leveldb) dir when created based upon
//! a snapshot.
constexpr std::string_view SNAPSHOT_CHAINSTATE_SUFFIX = "_snapshot";


//! Return a path to the snapshot-based chainstate dir, if one exists.
std::optional<fs::path> FindSnapshotChainstateDir();

} // namespace node

#endif // BITCOIN_NODE_UTXO_SNAPSHOT_H
