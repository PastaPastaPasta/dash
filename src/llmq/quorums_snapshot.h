// Copyright (c) 2017-2021 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QUORUMS_SNAPSHOT_H
#define BITCOIN_QUORUMS_SNAPSHOT_H

#include <evo/simplifiedmns.h>

class CQuorumSnapshot
{
public:
    //TODO investigate replacement of std::vector<bool> with CFixedBitSet
    std::vector<bool> activeQuorumMembers;
    int mnSkipListMode;
    std::vector<int> mnSkipList;

    CQuorumSnapshot() = default;
    explicit CQuorumSnapshot(const std::vector<bool>& _activeQuorumMembers, int _mnSkipListMode, const std::vector<int>& _mnSkipList) :
            activeQuorumMembers(_activeQuorumMembers),
            mnSkipListMode(_mnSkipListMode),
            mnSkipList(_mnSkipList)
    {
    }

    template <typename Stream, typename Operation>
    inline void SerializationOpBase(Stream& s, Operation ser_action)
    {
        READWRITE(mnSkipListMode);
    }

    template<typename Stream>
    void Serialize(Stream& s) const
    {
        const_cast<CQuorumSnapshot*>(this)->SerializationOpBase(s, CSerActionSerialize());

        WriteCompactSize(s, activeQuorumMembers.size());
        for (const auto& obj : activeQuorumMembers) {
            s << static_cast<int>(obj);
        }
        WriteCompactSize(s, mnSkipList.size());
        for (const auto& obj : mnSkipList) {
            s << obj;
        }
    }

    template<typename Stream>
    void Unserialize(Stream& s) {
        SerializationOpBase(s, CSerActionUnserialize());

        size_t cnt = {};
        cnt = ReadCompactSize(s);
        activeQuorumMembers.resize(cnt);
        for (size_t i = 0; i < cnt; i++) {
            int obj;
            s >> obj;
            activeQuorumMembers.push_back(static_cast<bool>(obj));
        }

        cnt = ReadCompactSize(s);
        mnSkipList.resize(cnt);
        for (size_t i = 0; i < cnt; i++) {
            int obj;
            s >> obj;
            mnSkipList.push_back(obj);
        }
    }

    void ToJson(UniValue& obj) const;
};

class CGetQuorumRotationInfo
{
public:
    int heightsNb;
    std::vector<int> knownHeights;

    SERIALIZE_METHODS(CGetQuorumRotationInfo, obj)
    {
        READWRITE(obj.heightsNb, obj.knownHeights);
    }
};

class CQuorumRotationInfo
{
public:
    int creationHeight;
    CQuorumSnapshot quorumSnaphotAtHMinusC;
    CQuorumSnapshot quorumSnaphotAtHMinus2C;
    CQuorumSnapshot quorumSnaphotAtHMinus3C;
    CSimplifiedMNListDiff mnListDiffTip;
    CSimplifiedMNListDiff mnListDiffAtH;
    CSimplifiedMNListDiff mnListDiffAtHMinusC;
    CSimplifiedMNListDiff mnListDiffAtHMinus2C;
    CSimplifiedMNListDiff mnListDiffAtHMinus3C;

    SERIALIZE_METHODS(CQuorumRotationInfo, obj)
    {
        READWRITE(obj.creationHeight,
                  obj.quorumSnaphotAtHMinusC,
                  obj.quorumSnaphotAtHMinus2C,
                  obj.quorumSnaphotAtHMinus3C,
                  obj.mnListDiffTip,
                  obj.mnListDiffAtH,
                  obj.mnListDiffAtHMinusC,
                  obj.mnListDiffAtHMinus2C,
                  obj.mnListDiffAtHMinus3C);
    }

    CQuorumRotationInfo() = default;
    explicit CQuorumRotationInfo(const CQuorumRotationInfo& dmn) {}

    void ToJson(UniValue& obj) const;
};

bool BuildQuorumRotationInfo(const CGetQuorumRotationInfo& request, CQuorumRotationInfo& quorumRotationInfoRet, std::string& errorRet);

class CQuorumSnapshotManager
{
private:
    CCriticalSection cs;

    CEvoDB& evoDb;

    std::unordered_map<uint256, CQuorumSnapshot, StaticSaltedHasher> quorumSnapshotCache;

public:
    explicit CQuorumSnapshotManager(CEvoDB& _evoDb);

    CQuorumSnapshot GetSnapshotForBlock(const Consensus::LLMQType llmqType, const CBlockIndex* pindex);
    void StoreSnapshotForBlock(const Consensus::LLMQType llmqType, const CBlockIndex* pindex, CQuorumSnapshot& snapshot);
};

extern std::unique_ptr<CQuorumSnapshotManager> quorumSnapshotManager;

#endif //BITCOIN_QUORUMS_SNAPSHOT_H
