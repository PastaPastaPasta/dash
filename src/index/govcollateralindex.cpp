// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <index/govcollateralindex.h>

#include <node/blockstorage.h>
#include <primitives/block.h>
#include <util/system.h>
#include <validation.h>

#include <algorithm>
#include <cassert>

constexpr uint8_t DB_GOVCOLLATERAL{'g'};

std::unique_ptr<GovCollateralIndex> g_gov_collateral_index;

class GovCollateralIndex::DB : public BaseIndex::DB
{
public:
    explicit DB(size_t n_cache_size, bool f_memory = false, bool f_wipe = false);

    bool ReadCollateral(const uint256& txid, governance::CollateralInfo& info) const;

    [[nodiscard]] bool WriteCollaterals(const std::vector<std::pair<uint256, governance::CollateralInfo>>& collaterals);
};

GovCollateralIndex::DB::DB(size_t n_cache_size, bool f_memory, bool f_wipe) :
    BaseIndex::DB(gArgs.GetDataDirNet() / "indexes" / "govcollateralindex", n_cache_size, f_memory, f_wipe)
{
}

bool GovCollateralIndex::DB::ReadCollateral(const uint256& txid, governance::CollateralInfo& info) const
{
    return Read(std::make_pair(DB_GOVCOLLATERAL, txid), info);
}

bool GovCollateralIndex::DB::WriteCollaterals(const std::vector<std::pair<uint256, governance::CollateralInfo>>& collaterals)
{
    CDBBatch batch(*this);
    for (const auto& [txid, info] : collaterals) {
        batch.Write(std::make_pair(DB_GOVCOLLATERAL, txid), info);
    }
    return WriteBatch(batch);
}

GovCollateralIndex::GovCollateralIndex(std::unique_ptr<interfaces::Chain> chain, size_t n_cache_size, bool f_memory, bool f_wipe) :
    BaseIndex(std::move(chain), "govcollateralindex"),
    m_db(std::make_unique<GovCollateralIndex::DB>(n_cache_size, f_memory, f_wipe))
{
}

GovCollateralIndex::~GovCollateralIndex() = default;

bool GovCollateralIndex::CustomInit(const std::optional<interfaces::BlockKey>& block)
{
    // BaseIndex only locks pruning once a new index has read its first block, so pruning at startup could
    // still delete the blocks it is about to read.
    if (!block) {
        WITH_LOCK(::cs_main, m_chainstate->m_blockman.UpdatePruneLock(GetName(), node::PruneLockInfo{/*height_first=*/0}));
    }
    return true;
}

bool GovCollateralIndex::CustomAppend(const interfaces::BlockInfo& block)
{
    // Like the transaction index, leave out the genesis block, whose outputs are not spendable.
    if (block.height == 0) return true;

    assert(block.data);
    std::vector<std::pair<uint256, governance::CollateralInfo>> collaterals;
    for (const auto& tx : block.data->vtx) {
        auto info{governance::GetCollateralInfo(*tx, block.hash)};
        const bool burns_fee{std::any_of(info.burns.begin(), info.burns.end(),
                                         [](const auto& burn) { return burn.second >= GOVERNANCE_PROPOSAL_FEE_TX; })};
        if (burns_fee) {
            collaterals.emplace_back(tx->GetHash(), std::move(info));
        }
    }
    return collaterals.empty() || m_db->WriteCollaterals(collaterals);
}

BaseIndex::DB& GovCollateralIndex::GetDB() const { return *m_db; }

bool GovCollateralIndex::CanSync(Chainstate& chainstate) const
{
    AssertLockHeld(::cs_main);
    CBlockLocator locator;
    const CBlockIndex* best_block{nullptr};
    if (m_db->ReadBestBlock(locator) && !locator.IsNull()) {
        best_block = chainstate.FindForkInGlobalIndex(locator);
    }
    for (const CBlockIndex* block{chainstate.m_chain.Tip()}; block != best_block; block = block->pprev) {
        if (!(block->nStatus & BLOCK_HAVE_DATA)) return false;
    }
    return true;
}

std::optional<governance::CollateralInfo> GovCollateralIndex::FindCollateral(const uint256& txid) const
{
    governance::CollateralInfo info;
    if (!m_db->ReadCollateral(txid, info)) return std::nullopt;
    return info;
}
