// Copyright (c) 2017-2021 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QUORUMS_SNAPSHOT_H
#define BITCOIN_QUORUMS_SNAPSHOT_H

#include <evo/simplifiedmns.h>

class CGetQuorumSnapshot
{
public:
    uint16_t heightsNb;
    std::vector<uint16_t> knownHeights;

    SERIALIZE_METHODS(CGetQuorumSnapshot, obj)
    {
        READWRITE(obj.heightsNb, obj.knownHeights);
    }
};

//TODO Maybe we should split the following class:
// CQuorumSnaphot should include {creationHeight, activeQuorumMembers H_C H_2C H_3C, and skipLists H_C H_2C H3_C}
// Maybe we need to include also blockHash for heights H_C H_2C H_3C
// CSnapshotInfo should include CQuorumSnaphot + mnListDiff Tip H H_C H_2C H3_C
class CQuorumSnapshot
{
public:
    int creationHeight;
    CSimplifiedMNListDiff mnListDiffTip;
    CSimplifiedMNListDiff mnListDiffH;
    CSimplifiedMNListDiff mnListDiffH_C;
    CSimplifiedMNListDiff mnListDiffH_2C;
    CSimplifiedMNListDiff mnListDiffH_3C;
    //TODO investigate replacement of std::vector<bool> with CFixedBitSet
    std::vector<bool> activeQuorumMembersH_C;
    std::vector<bool> activeQuorumMembersH_2C;
    std::vector<bool> activeQuorumMembersH_3C;
    //TODO need to fill skiplist. Selected mode is configured ?
    uint mnSkipListModeH_C;
    std::vector<uint16_t> mnSkipListH_C;
    uint mnSkipListModeH_2C;
    std::vector<uint16_t> mnSkipListH_2C;
    uint mnSkipListModeH_3C;
    std::vector<uint16_t> mnSkipListH_3C;

    SERIALIZE_METHODS(CQuorumSnapshot, obj)
    {
        READWRITE(obj.creationHeight,
                  obj.mnListDiffTip,
                  obj.mnListDiffH,
                  obj.mnListDiffH_C,
                  obj.mnListDiffH_2C,
                  obj.mnListDiffH_3C,
                  obj.activeQuorumMembersH_C,
                  obj.activeQuorumMembersH_2C,
                  obj.activeQuorumMembersH_3C,
                  obj.mnSkipListModeH_C,
                  obj.mnSkipListH_C,
                  obj.mnSkipListModeH_2C,
                  obj.mnSkipListH_2C,
                  obj.mnSkipListModeH_3C,
                  obj.mnSkipListH_3C);
    }

    CQuorumSnapshot() = default;
    explicit CQuorumSnapshot(const CQuorumSnapshot& dmn) {}

    static bool BuildQuorumMembersBitSet(const CDeterministicMNList& mnList, const llmq::CQuorumCPtr qu, std::vector<bool>& activeQuorumMembers);

    void ToJson(UniValue& obj) const;

    /*bool operator==(const CQuorumSnapshot& rhs) const
    {
        return creationHeight == rhs.creationHeight;
    }

    bool operator!=(const CQuorumSnapshot& rhs) const
    {
        return !(rhs == *this);
    }*/

};

bool BuildQuorumSnapshot(const uint16_t heightsNb, const std::vector<uint16_t>& knownHeights, CQuorumSnapshot& quorumSnapshotRet, std::string& errorRet);

#endif //BITCOIN_QUORUMS_SNAPSHOT_H
