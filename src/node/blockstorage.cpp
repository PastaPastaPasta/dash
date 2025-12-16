// Copyright (c) 2011-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/blockstorage.h>

#include <chain.h>
#include <chainparams.h>
#include <clientversion.h>
#include <consensus/validation.h>
#include <flatfile.h>
#include <hash.h>
#include <kernel/chain.h>
#include <kernel/types.h>
#include <logging.h>
#include <pow.h>
#include <shutdown.h>
#include <streams.h>
#include <undo.h>
#include <util/fs.h>
#include <util/system.h>
#include <validation.h>
#include <walletinitinterface.h>

#include <map>
#include <ranges>
#include <unordered_map>

namespace node {
std::atomic_bool fImporting(false);
std::atomic_bool fReindex(false);
bool fPruneMode = false;
uint64_t nPruneTarget = 0;

bool CBlockIndexWorkComparator::operator()(const CBlockIndex* pa, const CBlockIndex* pb) const
{
    // First sort by most total work, ...
    if (pa->nChainWork > pb->nChainWork) return false;
    if (pa->nChainWork < pb->nChainWork) return true;

    // ... then by earliest time received, ...
    if (pa->nSequenceId < pb->nSequenceId) return false;
    if (pa->nSequenceId > pb->nSequenceId) return true;

    // Use pointer address as tie breaker (should only happen with blocks
    // loaded from disk, as those all have id 0).
    if (pa < pb) return false;
    if (pa > pb) return true;

    // Identical blocks.
    return false;
}

bool CBlockIndexHeightOnlyComparator::operator()(const CBlockIndex* pa, const CBlockIndex* pb) const
{
    return pa->nHeight < pb->nHeight;
}

static FILE* OpenUndoFile(const FlatFilePos& pos, bool fReadOnly = false);
static FlatFileSeq BlockFileSeq();
static FlatFileSeq UndoFileSeq();

std::vector<CBlockIndex*> BlockManager::GetAllBlockIndices()
{
    AssertLockHeld(cs_main);
    std::vector<CBlockIndex*> rv;
    rv.reserve(m_block_index.size());
    for (auto& [_, block_index] : m_block_index) {
        rv.push_back(&block_index);
    }
    return rv;
}

CBlockIndex* BlockManager::LookupBlockIndex(const uint256& hash)
{
    AssertLockHeld(cs_main);
    BlockMap::iterator it = m_block_index.find(hash);
    return it == m_block_index.end() ? nullptr : &it->second;
}

const CBlockIndex* BlockManager::LookupBlockIndex(const uint256& hash) const
{
    AssertLockHeld(cs_main);
    BlockMap::const_iterator it = m_block_index.find(hash);
    return it == m_block_index.end() ? nullptr : &it->second;
}

CBlockIndex* BlockManager::AddToBlockIndex(const CBlockHeader& block, const uint256& hash, CBlockIndex*& best_header,
                                           enum BlockStatus nStatus)
{
    assert(!(nStatus & BLOCK_FAILED_MASK)); // no failed blocks allowed
    AssertLockHeld(cs_main);

    auto [mi, inserted] = m_block_index.try_emplace(hash, block);
    if (!inserted) {
        return &mi->second;
    }
    CBlockIndex* pindexNew = &(*mi).second;

    // We assign the sequence id to blocks only when the full data is available,
    // to avoid miners withholding blocks but broadcasting headers, to get a
    // competitive advantage.
    pindexNew->nSequenceId = 0;

    pindexNew->phashBlock = &((*mi).first);
    BlockMap::iterator miPrev = m_block_index.find(block.hashPrevBlock);
    if (miPrev != m_block_index.end()) {
        pindexNew->pprev = &(*miPrev).second;
        pindexNew->nHeight = pindexNew->pprev->nHeight + 1;
        pindexNew->BuildSkip();
    }
    pindexNew->nTimeMax = (pindexNew->pprev ? std::max(pindexNew->pprev->nTimeMax, pindexNew->nTime) : pindexNew->nTime);
    pindexNew->nChainWork = (pindexNew->pprev ? pindexNew->pprev->nChainWork : 0) + GetBlockProof(*pindexNew);
    if (nStatus & BLOCK_VALID_MASK) {
        pindexNew->RaiseValidity(nStatus);
        if (best_header == nullptr || best_header->nChainWork < pindexNew->nChainWork) {
            best_header = pindexNew;
        }
    } else {
        pindexNew->RaiseValidity(BLOCK_VALID_TREE); // required validity level
        pindexNew->nStatus |= nStatus;
    }

    m_dirty_blockindex.insert(pindexNew);

    // track prevBlockHash -> pindex (multimap)
    if (pindexNew->pprev) {
        m_prev_block_index.emplace(pindexNew->pprev->GetBlockHash(), pindexNew);
    }

    return pindexNew;
}

void BlockManager::PruneOneBlockFile(const int fileNumber)
{
    AssertLockHeld(cs_main);
    LOCK(cs_LastBlockFile);

    for (auto& entry : m_block_index) {
        CBlockIndex* pindex = &entry.second;
        if (pindex->nFile == fileNumber) {
            pindex->nStatus &= ~BLOCK_HAVE_DATA;
            pindex->nStatus &= ~BLOCK_HAVE_UNDO;
            pindex->nFile = 0;
            pindex->nDataPos = 0;
            pindex->nUndoPos = 0;
            m_dirty_blockindex.insert(pindex);

            // Prune from m_blocks_unlinked -- any block we prune would have
            // to be downloaded again in order to consider its chain, at which
            // point it would be considered as a candidate for
            // m_blocks_unlinked or setBlockIndexCandidates.
            auto range = m_blocks_unlinked.equal_range(pindex->pprev);
            while (range.first != range.second) {
                std::multimap<CBlockIndex*, CBlockIndex*>::iterator _it = range.first;
                range.first++;
                if (_it->second == pindex) {
                    m_blocks_unlinked.erase(_it);
                }
            }
        }
    }

    m_blockfile_info.at(fileNumber) = CBlockFileInfo{};
    m_dirty_fileinfo.insert(fileNumber);
}

void BlockManager::FindFilesToPruneManual(
    std::set<int>& setFilesToPrune,
    int nManualPruneHeight,
    const Chainstate& chain)
{
    assert(fPruneMode && nManualPruneHeight > 0);

    LOCK2(cs_main, cs_LastBlockFile);
    if (chain.m_chain.Height() < 0) {
        return;
    }

    const auto [min_block_to_prune, last_block_can_prune] = chain.GetPruneRange(nManualPruneHeight);

    int count = 0;
    for (int fileNumber = 0; fileNumber < this->MaxBlockfileNum(); fileNumber++) {
        const auto& fileinfo = m_blockfile_info[fileNumber];
        if (fileinfo.nSize == 0 || fileinfo.nHeightLast > (unsigned)last_block_can_prune || fileinfo.nHeightFirst < (unsigned)min_block_to_prune) {
            continue;
        }

        PruneOneBlockFile(fileNumber);
        setFilesToPrune.insert(fileNumber);
        count++;
    }
    LogPrintf("[%s] Prune (Manual): prune_height=%d removed %d blk/rev pairs\n",
        chain.GetRole(), last_block_can_prune, count);
}

void BlockManager::FindFilesToPrune(
    std::set<int>& setFilesToPrune,
    int last_prune,
    const Chainstate& chain,
    ChainstateManager& chainman)
{
    LOCK2(cs_main, cs_LastBlockFile);
    if (chain.m_chain.Height() < 0 || nPruneTarget == 0) {
        return;
    }
    // Compute `target` value with maximum size (in bytes) of blocks below the
    // `last_prune` height which should be preserved and not pruned. The
    // `target` value will be derived from the -prune preference provided by the
    // user. If there is a historical chainstate being used to populate indexes
    // and validate the snapshot, the target is divided by two so half of the
    // block storage will be reserved for the historical chainstate, and the
    // other half will be reserved for the most-work chainstate.
    const int num_chainstates{chainman.HistoricalChainstate() ? 2 : 1};

    // Distribute our -prune budget over all chainstates. On regtest, preserve
    // the lower configured target used by pruning tests.
    const auto target = std::max(
        std::min(MIN_DISK_SPACE_FOR_BLOCK_FILES, nPruneTarget),
        nPruneTarget / num_chainstates);

    if (target == 0) {
        return;
    }
    if (static_cast<uint64_t>(chain.m_chain.Height()) <= chainman.GetParams().PruneAfterHeight()) {
        return;
    }

    const auto [min_block_to_prune, last_block_can_prune] = chain.GetPruneRange(last_prune);

    uint64_t nCurrentUsage = CalculateCurrentUsage();
    // We don't check to prune until after we've allocated new space for files
    // So we should leave a buffer under our target to account for another allocation
    // before the next pruning.
    uint64_t nBuffer = BLOCKFILE_CHUNK_SIZE + UNDOFILE_CHUNK_SIZE;
    uint64_t nBytesToPrune;
    int count = 0;

    if (nCurrentUsage + nBuffer >= target) {
        // On a prune event, the chainstate DB is flushed.
        // To avoid excessive prune events negating the benefit of high dbcache
        // values, we should not prune too rapidly.
        // So when pruning in IBD, increase the buffer a bit to avoid a re-prune too soon.
        if (chain.IsInitialBlockDownload()) {
            // Since this is only relevant during IBD, we use a fixed 10%
            nBuffer += target / 10;
        }

        for (int fileNumber = 0; fileNumber < this->MaxBlockfileNum(); fileNumber++) {
            const auto& fileinfo = m_blockfile_info[fileNumber];
            nBytesToPrune = fileinfo.nSize + fileinfo.nUndoSize;

            if (fileinfo.nSize == 0) {
                continue;
            }

            if (nCurrentUsage + nBuffer < target) { // are we below our target?
                break;
            }

            // don't prune files that could have a block that's not within the allowable
            // prune range for the chain being pruned.
            if (fileinfo.nHeightLast > (unsigned)last_block_can_prune || fileinfo.nHeightFirst < (unsigned)min_block_to_prune) {
                continue;
            }

            PruneOneBlockFile(fileNumber);
            // Queue up the files for removal
            setFilesToPrune.insert(fileNumber);
            nCurrentUsage -= nBytesToPrune;
            count++;
        }
    }

    LogPrint(BCLog::PRUNE, "[%s] target=%dMiB actual=%dMiB diff=%dMiB min_height=%d max_prune_height=%d removed %d blk/rev pairs\n",
             chain.GetRole(), target / 1024 / 1024, nCurrentUsage / 1024 / 1024,
             (int64_t(target) - int64_t(nCurrentUsage)) / 1024 / 1024,
             min_block_to_prune, last_block_can_prune, count);
}

void BlockManager::UpdatePruneLock(const std::string& name, const PruneLockInfo& lock_info) {
    AssertLockHeld(::cs_main);
    m_prune_locks[name] = lock_info;
}

bool BlockManager::DeletePruneLock(const std::string& name)
{
    AssertLockHeld(::cs_main);
    return m_prune_locks.erase(name) > 0;
}

CBlockIndex* BlockManager::InsertBlockIndex(const uint256& hash)
{
    AssertLockHeld(cs_main);

    if (hash.IsNull()) {
        return nullptr;
    }

    const auto [mi, inserted]{m_block_index.try_emplace(hash)};
    CBlockIndex* pindex = &(*mi).second;
    if (inserted) {
        pindex->phashBlock = &((*mi).first);
    }
    return pindex;
}

bool BlockManager::LoadBlockIndex(const std::optional<uint256>& snapshot_blockhash)
{
    if (!m_block_tree_db->LoadBlockIndexGuts(GetConsensus(), [this](const uint256& hash) EXCLUSIVE_LOCKS_REQUIRED(cs_main) { return this->InsertBlockIndex(hash); })) {
        return false;
    }

    for (auto& [_, block_index] : m_block_index) {
        // build m_blockman.m_prev_block_index
        if (block_index.pprev) {
            m_prev_block_index.emplace(block_index.pprev->GetBlockHash(), &block_index);
        }

        // M4/M5 assumeutxo entries used a reserved status bit and fabricated
        // transaction counts for header-only blocks. Remove that legacy state
        // before rebuilding nChainTx below, and persist the upgraded entry.
        if ((block_index.nStatus & BLOCK_STATUS_RESERVED) &&
            (block_index.nStatus & BLOCK_VALID_MASK) < BLOCK_VALID_TRANSACTIONS) {
            block_index.nTx = 0;
            block_index.nChainTx = 0;
            block_index.nStatus &= ~BLOCK_STATUS_RESERVED;
            m_dirty_blockindex.insert(&block_index);
        }
    }

    if (snapshot_blockhash) {
        const std::optional<AssumeutxoData> maybe_au_data = GetParams().AssumeutxoForBlockhash(*snapshot_blockhash);
        if (!maybe_au_data) {
            AbortNode(strprintf("Assumeutxo data not found for the given blockhash '%s'.", snapshot_blockhash->ToString()));
            return false;
        }
        const AssumeutxoData& au_data = *Assert(maybe_au_data);
        m_snapshot_height = au_data.height;
        CBlockIndex* base{LookupBlockIndex(*snapshot_blockhash)};

        // Since nChainTx (responsible for estimated progress) isn't persisted
        // to disk, bootstrap the snapshot base value from the hardcoded
        // assumeutxo chainparams. Earlier unknown counts remain zero.
        base->nChainTx = au_data.nChainTx;
        LogPrintf("[snapshot] set nChainTx=%d for %s\n", au_data.nChainTx, snapshot_blockhash->ToString());
    } else {
        // If this isn't called with a snapshot blockhash, make sure the cached snapshot height
        // is null. This is relevant during snapshot completion, when the blockman may be loaded
        // with a height that then needs to be cleared after the snapshot is fully validated.
        m_snapshot_height.reset();
    }

    Assert(m_snapshot_height.has_value() == snapshot_blockhash.has_value());

    // Calculate nChainWork
    std::vector<CBlockIndex*> vSortedByHeight{GetAllBlockIndices()};
    std::sort(vSortedByHeight.begin(), vSortedByHeight.end(),
              CBlockIndexHeightOnlyComparator());

    CBlockIndex* previous_index{nullptr};
    for (CBlockIndex* pindex : vSortedByHeight) {
        if (ShutdownRequested()) return false;
        if (previous_index && pindex->nHeight > previous_index->nHeight + 1) {
            return error("%s: block index is non-contiguous, index of height %d missing", __func__, previous_index->nHeight + 1);
        }
        previous_index = pindex;
        pindex->nChainWork = (pindex->pprev ? pindex->pprev->nChainWork : 0) + GetBlockProof(*pindex);
        pindex->nTimeMax = (pindex->pprev ? std::max(pindex->pprev->nTimeMax, pindex->nTime) : pindex->nTime);

        // Link the chain of blocks for which we've received transactions at
        // some point. The snapshot base nChainTx was bootstrapped above even
        // when its nTx is still unknown. Pruned nodes may have deleted a block.
        if (pindex->nTx > 0) {
            if (pindex->pprev) {
                if (m_snapshot_height && pindex->nHeight == *m_snapshot_height &&
                        pindex->GetBlockHash() == *snapshot_blockhash) {
                    // Should have been set above; don't disturb it with code below.
                    Assert(pindex->nChainTx > 0);
                } else if (pindex->pprev->nChainTx > 0) {
                    pindex->nChainTx = pindex->pprev->nChainTx + pindex->nTx;
                } else {
                    pindex->nChainTx = 0;
                    m_blocks_unlinked.insert(std::make_pair(pindex->pprev, pindex));
                }
            } else {
                pindex->nChainTx = pindex->nTx;
            }
        }
        if (!(pindex->nStatus & BLOCK_FAILED_MASK) && pindex->pprev && (pindex->pprev->nStatus & BLOCK_FAILED_MASK)) {
            pindex->nStatus |= BLOCK_FAILED_CHILD;
            m_dirty_blockindex.insert(pindex);
        }
        if (pindex->pprev) {
            pindex->BuildSkip();
        }
    }

    return true;
}

bool BlockManager::WriteBlockIndexDB()
{
    AssertLockHeld(::cs_main);
    std::vector<std::pair<int, const CBlockFileInfo*>> vFiles;
    vFiles.reserve(m_dirty_fileinfo.size());
    for (std::set<int>::iterator it = m_dirty_fileinfo.begin(); it != m_dirty_fileinfo.end();) {
        vFiles.emplace_back(*it, &m_blockfile_info[*it]);
        m_dirty_fileinfo.erase(it++);
    }
    std::vector<const CBlockIndex*> vBlocks;
    vBlocks.reserve(m_dirty_blockindex.size());
    for (std::set<CBlockIndex*>::iterator it = m_dirty_blockindex.begin(); it != m_dirty_blockindex.end();) {
        vBlocks.push_back(*it);
        m_dirty_blockindex.erase(it++);
    }
    int max_blockfile = WITH_LOCK(cs_LastBlockFile, return this->MaxBlockfileNum());
    if (!m_block_tree_db->WriteBatchSync(vFiles, max_blockfile, vBlocks)) {
        return false;
    }
    return true;
}

bool BlockManager::LoadBlockIndexDB(const std::optional<uint256>& snapshot_blockhash)
{
    if (!LoadBlockIndex(snapshot_blockhash)) {
        return false;
    }
    int max_blockfile_num{0};

    // Load block file info
    m_block_tree_db->ReadLastBlockFile(max_blockfile_num);
    m_blockfile_info.resize(max_blockfile_num + 1);
    LogPrintf("%s: last block file = %i\n", __func__, max_blockfile_num);
    for (int nFile = 0; nFile <= max_blockfile_num; nFile++) {
        m_block_tree_db->ReadBlockFileInfo(nFile, m_blockfile_info[nFile]);
    }
    LogPrintf("%s: last block file info: %s\n", __func__, m_blockfile_info[max_blockfile_num].ToString());
    for (int nFile = max_blockfile_num + 1; true; nFile++) {
        CBlockFileInfo info;
        if (m_block_tree_db->ReadBlockFileInfo(nFile, info)) {
            m_blockfile_info.push_back(info);
        } else {
            break;
        }
    }

    // Check presence of blk files
    LogPrintf("Checking all blk files are present...\n");
    std::set<int> setBlkDataFiles;
    for (const auto& [_, block_index] : m_block_index) {
        if (block_index.nStatus & BLOCK_HAVE_DATA) {
            setBlkDataFiles.insert(block_index.nFile);
        }
    }
    for (std::set<int>::iterator it = setBlkDataFiles.begin(); it != setBlkDataFiles.end(); it++) {
        FlatFilePos pos(*it, 0);
        if (CAutoFile(OpenBlockFile(pos, true), SER_DISK, CLIENT_VERSION).IsNull()) {
            return false;
        }
    }

    {
        // Initialize the blockfile cursors.
        LOCK(cs_LastBlockFile);
        for (size_t i = 0; i < m_blockfile_info.size(); ++i) {
            const auto last_height_in_file = m_blockfile_info[i].nHeightLast;
            m_blockfile_cursors[BlockfileTypeForHeight(last_height_in_file)] = {static_cast<int>(i), 0};
        }
    }

    // Check whether we have ever pruned block & undo files
    m_block_tree_db->ReadFlag("prunedblockfiles", m_have_pruned);
    if (m_have_pruned) {
        LogPrintf("LoadBlockIndexDB(): Block files have previously been pruned\n");
    }

    // Check whether we need to continue reindexing
    bool fReindexing = false;
    m_block_tree_db->ReadReindexing(fReindexing);
    if (fReindexing) fReindex = true;

    // Migrate old synchronous index data from block index database
    // to async indexes in separate databases
    if (!m_block_tree_db->MigrateOldIndexData()) {
        return false;
    }

    return true;
}

void BlockManager::ScanAndUnlinkAlreadyPrunedFiles()
{
    AssertLockHeld(::cs_main);
    int max_blockfile = WITH_LOCK(cs_LastBlockFile, return this->MaxBlockfileNum());
    if (!m_have_pruned) {
        return;
    }

    std::set<int> block_files_to_prune;
    for (int file_number = 0; file_number < max_blockfile; file_number++) {
        if (m_blockfile_info[file_number].nSize == 0) {
            block_files_to_prune.insert(file_number);
        }
    }

    UnlinkPrunedFiles(block_files_to_prune);
}

const CBlockIndex* BlockManager::GetLastCheckpoint(const CCheckpointData& data)
{
    const MapCheckpoints& checkpoints = data.mapCheckpoints;

    for (const MapCheckpoints::value_type& i : checkpoints | std::views::reverse) {
        const uint256& hash = i.second;
        const CBlockIndex* pindex = LookupBlockIndex(hash);
        if (pindex) {
            return pindex;
        }
    }
    return nullptr;
}

bool BlockManager::IsBlockPruned(const CBlockIndex* pblockindex)
{
    AssertLockHeld(::cs_main);
    return (m_have_pruned && !(pblockindex->nStatus & BLOCK_HAVE_DATA) && pblockindex->nTx > 0);
}

const CBlockIndex* BlockManager::GetFirstStoredBlock(const CBlockIndex& start_block) const
{
    AssertLockHeld(::cs_main);
    const CBlockIndex* last_block = &start_block;
    while (last_block->pprev && (last_block->pprev->nStatus & BLOCK_HAVE_DATA)) {
        last_block = last_block->pprev;
    }
    return last_block;
}

// If we're using -prune with -reindex, then delete block files that will be ignored by the
// reindex.  Since reindexing works by starting at block file 0 and looping until a blockfile
// is missing, do the same here to delete any later block files after a gap.  Also delete all
// rev files since they'll be rewritten by the reindex anyway.  This ensures that m_blockfile_info
// is in sync with what's actually on disk by the time we start downloading, so that pruning
// works correctly.
void CleanupBlockRevFiles()
{
    std::map<std::string, fs::path> mapBlockFiles;

    // Glob all blk?????.dat and rev?????.dat files from the blocks directory.
    // Remove the rev files immediately and insert the blk file paths into an
    // ordered map keyed by block file index.
    LogPrintf("Removing unusable blk?????.dat and rev?????.dat files for -reindex with -prune\n");
    const fs::path& blocksdir = gArgs.GetBlocksDirPath();
    for (fs::directory_iterator it(blocksdir); it != fs::directory_iterator(); it++) {
        const std::string path = fs::PathToString(it->path().filename());
        if (fs::is_regular_file(*it) &&
            path.length() == 12 &&
            path.substr(8,4) == ".dat")
        {
            if (path.substr(0, 3) == "blk") {
                mapBlockFiles[path.substr(3, 5)] = it->path();
            } else if (path.substr(0, 3) == "rev") {
                remove(it->path());
            }
        }
    }

    // Remove all block files that aren't part of a contiguous set starting at
    // zero by walking the ordered map (keys are block file indices) by
    // keeping a separate counter.  Once we hit a gap (or if 0 doesn't exist)
    // start removing block files.
    int nContigCounter = 0;
    for (const std::pair<const std::string, fs::path>& item : mapBlockFiles) {
        if (LocaleIndependentAtoi<int>(item.first) == nContigCounter) {
            nContigCounter++;
            continue;
        }
        remove(item.second);
    }
}

CBlockFileInfo* BlockManager::GetBlockFileInfo(size_t n)
{
    LOCK(cs_LastBlockFile);

    return &m_blockfile_info.at(n);
}

static bool UndoWriteToDisk(const CBlockUndo& blockundo, FlatFilePos& pos, const uint256& hashBlock, const CMessageHeader::MessageStartChars& messageStart)
{
    // Open history file to append
    CAutoFile fileout(OpenUndoFile(pos), SER_DISK, CLIENT_VERSION);
    if (fileout.IsNull()) {
        return error("%s: OpenUndoFile failed", __func__);
    }

    // Write index header
    unsigned int nSize = GetSerializeSize(blockundo, fileout.GetVersion());
    fileout << messageStart << nSize;

    // Write undo data
    long fileOutPos = ftell(fileout.Get());
    if (fileOutPos < 0) {
        return error("%s: ftell failed", __func__);
    }
    pos.nPos = (unsigned int)fileOutPos;
    fileout << blockundo;

    // calculate & write checksum
    HashWriter hasher{};
    hasher << hashBlock;
    hasher << blockundo;
    fileout << hasher.GetHash();

    return true;
}

bool UndoReadFromDisk(CBlockUndo& blockundo, const CBlockIndex* pindex)
{
    const FlatFilePos pos{WITH_LOCK(::cs_main, return pindex->GetUndoPos())};

    if (pos.IsNull()) {
        return error("%s: no undo data available", __func__);
    }

    // Open history file to read
    CAutoFile filein(OpenUndoFile(pos, true), SER_DISK, CLIENT_VERSION);
    if (filein.IsNull()) {
        return error("%s: OpenUndoFile failed", __func__);
    }

    // Read block
    uint256 hashChecksum;
    CHashVerifier<CAutoFile> verifier(&filein); // We need a CHashVerifier as reserializing may lose data
    try {
        verifier << pindex->pprev->GetBlockHash();
        verifier >> blockundo;
        filein >> hashChecksum;
    } catch (const std::exception& e) {
        return error("%s: Deserialize or I/O error - %s", __func__, e.what());
    }

    // Verify checksum
    if (hashChecksum != verifier.GetHash()) {
        return error("%s: Checksum mismatch", __func__);
    }

    return true;
}

bool BlockManager::FlushUndoFile(int block_file, bool finalize)
{
    FlatFilePos undo_pos_old(block_file, m_blockfile_info[block_file].nUndoSize);
    if (!UndoFileSeq().Flush(undo_pos_old, finalize)) {
        AbortNode("Flushing undo file to disk failed. This is likely the result of an I/O error.");
        return false;
    }
    return true;
}

bool BlockManager::FlushBlockFile(int blockfile_num, bool fFinalize, bool finalize_undo)
{
    bool success{true};
    LOCK(cs_LastBlockFile);

    if (m_blockfile_info.size() < 1) {
        // Return if we haven't loaded any blockfiles yet. This happens during
        // chainstate init, when we call ChainstateManager::MaybeRebalanceCaches() (which
        // then calls FlushStateToDisk()), resulting in a call to this function before we
        // have populated `m_blockfile_info` via LoadBlockIndexDB().
        return true;
    }
    assert(static_cast<int>(m_blockfile_info.size()) > blockfile_num);

    FlatFilePos block_pos_old(blockfile_num, m_blockfile_info[blockfile_num].nSize);
    if (!BlockFileSeq().Flush(block_pos_old, fFinalize)) {
        AbortNode("Flushing block file to disk failed. This is likely the result of an I/O error.");
        success = false;
    }
    // we do not always flush the undo file, as the chain tip may be lagging behind the incoming blocks,
    // e.g. during IBD or a sync after a node going offline
    if (!fFinalize || finalize_undo) {
        if (!FlushUndoFile(blockfile_num, finalize_undo)) {
            success = false;
        }
    }
    return success;
}

BlockfileType BlockManager::BlockfileTypeForHeight(int height)
{
    if (!m_snapshot_height) {
        return BlockfileType::NORMAL;
    }
    return (height >= *m_snapshot_height) ? BlockfileType::ASSUMED : BlockfileType::NORMAL;
}

bool BlockManager::FlushChainstateBlockFile(int tip_height)
{
    LOCK(cs_LastBlockFile);
    auto& cursor = m_blockfile_cursors[BlockfileTypeForHeight(tip_height)];
    // If the cursor does not exist, it means an assumeutxo snapshot is loaded,
    // but no blocks past the snapshot height have been written yet, so there
    // is no data associated with the chainstate, and it is safe not to flush.
    if (cursor) {
        return FlushBlockFile(cursor->file_num, /*fFinalize=*/false, /*finalize_undo=*/false);
    }
    // No need to log warnings in this case.
    return true;
}

uint64_t BlockManager::CalculateCurrentUsage()
{
    LOCK(cs_LastBlockFile);

    uint64_t retval = 0;
    for (const CBlockFileInfo& file : m_blockfile_info) {
        retval += file.nSize + file.nUndoSize;
    }
    return retval;
}

void UnlinkPrunedFiles(const std::set<int>& setFilesToPrune)
{
    std::error_code ec;
    for (std::set<int>::iterator it = setFilesToPrune.begin(); it != setFilesToPrune.end(); ++it) {
        FlatFilePos pos(*it, 0);
        const bool removed_blockfile{fs::remove(BlockFileSeq().FileName(pos), ec)};
        const bool removed_undofile{fs::remove(UndoFileSeq().FileName(pos), ec)};
        if (removed_blockfile || removed_undofile) {
            LogPrint(BCLog::BLOCKSTORE, "Prune: %s deleted blk/rev (%05u)\n", __func__, *it);
        }
    }
}

static FlatFileSeq BlockFileSeq()
{
    return FlatFileSeq(gArgs.GetBlocksDirPath(), "blk",
                       gArgs.GetBoolArg("-fastprune", false) ? 0x4000 /* 16kb */ :
                        (gArgs.GetBoolArg("-tinyblk", false) ? 0x10000 /* 64kb */ : BLOCKFILE_CHUNK_SIZE));
}

static FlatFileSeq UndoFileSeq()
{
    return FlatFileSeq(gArgs.GetBlocksDirPath(), "rev", gArgs.GetBoolArg("-tinyblk", false) ? 0x10000 /* 64kb */ : UNDOFILE_CHUNK_SIZE);
}

FILE* OpenBlockFile(const FlatFilePos& pos, bool fReadOnly)
{
    return BlockFileSeq().Open(pos, fReadOnly);
}

/** Open an undo file (rev?????.dat) */
static FILE* OpenUndoFile(const FlatFilePos& pos, bool fReadOnly)
{
    return UndoFileSeq().Open(pos, fReadOnly);
}

fs::path GetBlockPosFilename(const FlatFilePos& pos)
{
    return BlockFileSeq().FileName(pos);
}

bool BlockManager::FindBlockPos(FlatFilePos& pos, unsigned int nAddSize, unsigned int nHeight, uint64_t nTime, bool fKnown)
{
    LOCK(cs_LastBlockFile);

    const BlockfileType chain_type = BlockfileTypeForHeight(nHeight);

    if (!m_blockfile_cursors[chain_type]) {
        // If a snapshot is loaded during runtime, we may not have initialized this cursor yet.
        assert(chain_type == BlockfileType::ASSUMED);
        const auto new_cursor = BlockfileCursor{this->MaxBlockfileNum() + 1};
        m_blockfile_cursors[chain_type] = new_cursor;
        LogPrint(BCLog::BLOCKSTORE, "[%s] initializing blockfile cursor to %s\n", chain_type, new_cursor);
    }
    const int last_blockfile = m_blockfile_cursors[chain_type]->file_num;

    int nFile = fKnown ? pos.nFile : last_blockfile;
    if (static_cast<int>(m_blockfile_info.size()) <= nFile) {
        m_blockfile_info.resize(nFile + 1);
    }

    bool finalize_undo = false;
    if (!fKnown) {
        unsigned int max_blockfile_size{MAX_BLOCKFILE_SIZE};
        // Use smaller blockfiles in test-only -fastprune mode - but avoid
        // the possibility of having a block not fit into the block file.
        if (gArgs.GetBoolArg("-fastprune", false)) {
            max_blockfile_size = 0x10000; // 64kiB
            if (nAddSize >= max_blockfile_size) {
                // dynamically adjust the blockfile size to be larger than the added size
                max_blockfile_size = nAddSize + 1;
            }
        }
        assert(nAddSize < max_blockfile_size);

        while (m_blockfile_info[nFile].nSize + nAddSize >= max_blockfile_size) {
            // when the undo file is keeping up with the block file, we want to flush it explicitly
            // when it is lagging behind (more blocks arrive than are being connected), we let the
            // undo block write case handle it
            finalize_undo = (static_cast<int>(m_blockfile_info[nFile].nHeightLast) ==
                    Assert(m_blockfile_cursors[chain_type])->undo_height);

            // Try the next unclaimed blockfile number
            nFile = this->MaxBlockfileNum() + 1;
            // Set to increment MaxBlockfileNum() for next iteration
            m_blockfile_cursors[chain_type] = BlockfileCursor{nFile};

            if (static_cast<int>(m_blockfile_info.size()) <= nFile) {
                m_blockfile_info.resize(nFile + 1);
            }
        }
        pos.nFile = nFile;
        pos.nPos = m_blockfile_info[nFile].nSize;
    }

    if (nFile != last_blockfile) {
        if (!fKnown) {
            LogPrint(BCLog::BLOCKSTORE, "Leaving block file %i: %s (onto %i) (height %i)\n",
                last_blockfile, m_blockfile_info[last_blockfile].ToString(), nFile, nHeight);
        }

        // Do not propagate the return code. The flush concerns a previous block
        // and undo file that has already been written to. If a flush fails
        // here, and we crash, there is no expected additional block data
        // inconsistency arising from the flush failure here. However, the undo
        // data may be inconsistent after a crash if the flush is called during
        // a reindex. A flush error might also leave some of the data files
        // untrimmed.
        if (!FlushBlockFile(last_blockfile, !fKnown, finalize_undo)) {
            LogPrintLevel(BCLog::BLOCKSTORE, BCLog::Level::Warning, "Failed to flush previous block file %05i (finalize=%i, finalize_undo=%i) before opening new block file %05i\n",
                          last_blockfile, !fKnown, finalize_undo, nFile);
        }
        // No undo data yet in the new file, so reset our undo-height tracking.
        m_blockfile_cursors[chain_type] = BlockfileCursor{nFile};
    }

    m_blockfile_info[nFile].AddBlock(nHeight, nTime);
    if (fKnown) {
        m_blockfile_info[nFile].nSize = std::max(pos.nPos + nAddSize, m_blockfile_info[nFile].nSize);
    } else {
        m_blockfile_info[nFile].nSize += nAddSize;
    }

    if (!fKnown) {
        bool out_of_space;
        size_t bytes_allocated = BlockFileSeq().Allocate(pos, nAddSize, out_of_space);
        if (out_of_space) {
            return AbortNode("Disk space is too low!", _("Disk space is too low!"));
        }
        if (bytes_allocated != 0 && fPruneMode) {
            m_check_for_pruning = true;
        }
    }

    m_dirty_fileinfo.insert(nFile);
    return true;
}

bool BlockManager::FindUndoPos(BlockValidationState& state, int nFile, FlatFilePos& pos, unsigned int nAddSize)
{
    pos.nFile = nFile;

    LOCK(cs_LastBlockFile);

    pos.nPos = m_blockfile_info[nFile].nUndoSize;
    m_blockfile_info[nFile].nUndoSize += nAddSize;
    m_dirty_fileinfo.insert(nFile);

    bool out_of_space;
    size_t bytes_allocated = UndoFileSeq().Allocate(pos, nAddSize, out_of_space);
    if (out_of_space) {
        return AbortNode(state, "Disk space is too low!", _("Disk space is too low!"));
    }
    if (bytes_allocated != 0 && fPruneMode) {
        m_check_for_pruning = true;
    }

    return true;
}

static bool WriteBlockToDisk(const CBlock& block, FlatFilePos& pos, const CMessageHeader::MessageStartChars& messageStart)
{
    // Open history file to append
    CAutoFile fileout(OpenBlockFile(pos), SER_DISK, CLIENT_VERSION);
    if (fileout.IsNull()) {
        return error("WriteBlockToDisk: OpenBlockFile failed");
    }

    // Write index header
    unsigned int nSize = GetSerializeSize(block, fileout.GetVersion());
    fileout << messageStart << nSize;

    // Write block
    long fileOutPos = ftell(fileout.Get());
    if (fileOutPos < 0) {
        return error("WriteBlockToDisk: ftell failed");
    }
    pos.nPos = (unsigned int)fileOutPos;
    fileout << block;

    return true;
}

bool BlockManager::WriteUndoDataForBlock(const CBlockUndo& blockundo, BlockValidationState& state, CBlockIndex& block)
{
    AssertLockHeld(::cs_main);
    const BlockfileType type = BlockfileTypeForHeight(block.nHeight);
    auto& cursor = *Assert(WITH_LOCK(cs_LastBlockFile, return m_blockfile_cursors[type]));

    // Write undo information to disk
    if (block.GetUndoPos().IsNull()) {
        FlatFilePos _pos;
        if (!FindUndoPos(state, block.nFile, _pos, ::GetSerializeSize(blockundo, CLIENT_VERSION) + 40)) {
            return error("ConnectBlock(): FindUndoPos failed");
        }
        if (!UndoWriteToDisk(blockundo, _pos, block.pprev->GetBlockHash(), GetParams().MessageStart())) {
            return AbortNode(state, "Failed to write undo data");
        }
        // rev files are written in block height order, whereas blk files are written as blocks come in (often out of order)
        // we want to flush the rev (undo) file once we've written the last block, which is indicated by the last height
        // in the block file info as below; note that this does not catch the case where the undo writes are keeping up
        // with the block writes (usually when a synced up node is getting newly mined blocks) -- this case is caught in
        // the FindBlockPos function
        if (_pos.nFile < cursor.file_num && static_cast<uint32_t>(block.nHeight) == m_blockfile_info[_pos.nFile].nHeightLast) {
            // Do not propagate the return code, a failed flush here should not
            // be an indication for a failed write. If it were propagated here,
            // the caller would assume the undo data not to be written, when in
            // fact it is. Note though, that a failed flush might leave the data
            // file untrimmed.
            if (!FlushUndoFile(_pos.nFile, true)) {
                LogPrintLevel(BCLog::BLOCKSTORE, BCLog::Level::Warning, "Failed to flush undo file %05i\n", _pos.nFile);
            }
        } else if (_pos.nFile == cursor.file_num && block.nHeight > cursor.undo_height) {
            cursor.undo_height = block.nHeight;
        }
        // update nUndoPos in block index
        block.nUndoPos = _pos.nPos;
        block.nStatus |= BLOCK_HAVE_UNDO;
        m_dirty_blockindex.insert(&block);
    }

    return true;
}

std::optional<uint256> ReadBlockFromDisk(CBlock& block, const FlatFilePos& pos, const Consensus::Params& consensusParams)
{
    block.SetNull();

    // Open history file to read
    CAutoFile filein(OpenBlockFile(pos, true), SER_DISK, CLIENT_VERSION);
    if (filein.IsNull()) {
        error("ReadBlockFromDisk: OpenBlockFile failed for %s", pos.ToString());
        return std::nullopt;
    }

    // Read block
    try {
        filein >> block;
    } catch (const std::exception& e) {
        error("%s: Deserialize or I/O error - %s at %s", __func__, e.what(), pos.ToString());
        return std::nullopt;
    }

    // Check the header
    const uint256 hash{block.GetHash()};
    if (!CheckProofOfWork(hash, block.nBits, consensusParams)) {
        error("ReadBlockFromDisk: Errors in block header at %s", pos.ToString());
        return std::nullopt;
    }

    return hash;
}

bool ReadBlockFromDisk(CBlock& block, const CBlockIndex* pindex, const Consensus::Params& consensusParams)
{
    const FlatFilePos block_pos{WITH_LOCK(cs_main, return pindex->GetBlockPos())};

    const auto hash{ReadBlockFromDisk(block, block_pos, consensusParams)};
    if (!hash) {
        return false;
    }
    if (*hash != pindex->GetBlockHash()) {
        return error("ReadBlockFromDisk(CBlock&, CBlockIndex*): GetHash() doesn't match index for %s at %s",
                     pindex->ToString(), block_pos.ToString());
    }
    return true;
}

FlatFilePos BlockManager::SaveBlockToDisk(const CBlock& block, int nHeight, const FlatFilePos* dbp)
{
    unsigned int nBlockSize = ::GetSerializeSize(block, CLIENT_VERSION);
    FlatFilePos blockPos;
    const auto position_known {dbp != nullptr};
    if (position_known) {
        blockPos = *dbp;
    } else {
        // when known, blockPos.nPos points at the offset of the block data in the blk file. that already accounts for
        // the serialization header present in the file (the 4 magic message start bytes + the 4 length bytes = 8 bytes = BLOCK_SERIALIZATION_HEADER_SIZE).
        // we add BLOCK_SERIALIZATION_HEADER_SIZE only for new blocks since they will have the serialization header added when written to disk.
        nBlockSize += static_cast<unsigned int>(BLOCK_SERIALIZATION_HEADER_SIZE);
    }
    if (!FindBlockPos(blockPos, nBlockSize, nHeight, block.GetBlockTime(), position_known)) {
        error("%s: FindBlockPos failed", __func__);
        return FlatFilePos();
    }
    if (!position_known) {
        if (!WriteBlockToDisk(block, blockPos, GetParams().MessageStart())) {
            AbortNode("Failed to write block");
            return FlatFilePos();
        }
    }
    return blockPos;
}

struct CImportingNow {
    CImportingNow()
    {
        assert(fImporting == false);
        fImporting = true;
    }

    ~CImportingNow()
    {
        assert(fImporting == true);
        fImporting = false;
    }
};

void ThreadImport(ChainstateManager& chainman, std::vector<fs::path> vImportFiles, const ArgsManager& args, const fs::path& mempool_path)
{
    ScheduleBatchPriority();

    {
        CImportingNow imp;

        // -reindex
        if (fReindex) {
            int nFile = 0;
            // Map of disk positions for blocks with unknown parent (only used for reindex);
            // parent hash -> child disk position, multiple children can have the same parent.
            std::multimap<uint256, FlatFilePos> blocks_with_unknown_parent;
            while (true) {
                FlatFilePos pos(nFile, 0);
                if (!fs::exists(GetBlockPosFilename(pos))) {
                    break; // No block files left to reindex
                }
                FILE* file = OpenBlockFile(pos, true);
                if (!file) {
                    break; // This error is logged in OpenBlockFile
                }
                LogPrintf("Reindexing block file blk%05u.dat...\n", (unsigned int)nFile);
                chainman.LoadExternalBlockFile(file, &pos, &blocks_with_unknown_parent);
                if (ShutdownRequested()) {
                    LogPrintf("Shutdown requested. Exit %s\n", __func__);
                    return;
                }
                nFile++;
            }
            WITH_LOCK(::cs_main, chainman.m_blockman.m_block_tree_db->WriteReindexing(false));
            fReindex = false;
            LogPrintf("Reindexing finished\n");
            // To avoid ending up in a situation without genesis block, re-try initializing (no-op if reindexing worked):
            chainman.ActiveChainstate().LoadGenesisBlock();
        }

        // -loadblock=
        for (const fs::path& path : vImportFiles) {
            FILE *file = fsbridge::fopen(path, "rb");
            if (file) {
                LogPrintf("Importing blocks file %s...\n", fs::PathToString(path));
                chainman.LoadExternalBlockFile(file);
                if (ShutdownRequested()) {
                    LogPrintf("Shutdown requested. Exit %s\n", __func__);
                    return;
                }
            } else {
                LogPrintf("Warning: Could not open blocks file %s\n", fs::PathToString(path));
            }
        }

        // scan for better chains in the block chain database, that are not yet connected in the active best chain

        if (auto result{chainman.ActivateBestChains()}; !result) {
            LogPrintf("Failed to connect best block (%s)\n", util::ErrorString(result).original);
            StartShutdown();
            return;
        }

        if (args.GetBoolArg("-stopafterblockimport", DEFAULT_STOPAFTERBLOCKIMPORT)) {
            LogPrintf("Stopping after block import\n");
            StartShutdown();
            return;
        }
    } // End scope of CImportingNow
    chainman.ActiveChainstate().LoadMempool(mempool_path);
}

std::ostream& operator<<(std::ostream& os, const BlockfileType& type) {
    switch(type) {
        case BlockfileType::NORMAL: os << "normal"; break;
        case BlockfileType::ASSUMED: os << "assumed"; break;
        default: os.setstate(std::ios_base::failbit);
    }
    return os;
}

std::ostream& operator<<(std::ostream& os, const BlockfileCursor& cursor) {
    os << strprintf("BlockfileCursor(file_num=%d, undo_height=%d)", cursor.file_num, cursor.undo_height);
    return os;
}
} // namespace node
