// Copyright (c) 2016-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_DEPLOYMENTINFO_H
#define BITCOIN_DEPLOYMENTINFO_H

#include <consensus/params.h>

#include <array>
#include <string>

struct VBDeploymentInfo {
    /** Deployment name */
    const char *name;
    /** Whether GBT clients can safely ignore this rule in simplified usage */
    bool gbt_force;
};

constexpr std::array<VBDeploymentInfo, Consensus::MAX_VERSION_BITS_DEPLOYMENTS> VersionBitsDeploymentInfo{{
    {
        /*.name =*/ "testdummy",
        /*.gbt_force =*/ true,
    },
    {
        /*.name =*/ "taproot",
        /*.gbt_force =*/ true,
    },
}};

std::string DeploymentName(Consensus::BuriedDeployment dep);

inline std::string DeploymentName(Consensus::DeploymentPos pos)
{
    assert(Consensus::ValidDeployment(pos));
    return VersionBitsDeploymentInfo.at(pos).name;
}

#endif // BITCOIN_DEPLOYMENTINFO_H
