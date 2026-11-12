// Copyright (c) 2018-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bench/bench.h>

#include <bls/bls_batchverifier.h>
#include <bls/bls_worker.h>
#include <dbwrapper.h>
#include <evo/deterministicmns.h>
#include <llmq/commitment.h>
#include <llmq/context.h>
#include <llmq/dkgsessionmgr.h>
#include <llmq/options_args.h>
#include <llmq/snapshot.h>
#include <llmq/utils.h>
#include <streams.h>
#include <test/util/setup_common.h>
#include <tinyformat.h>
#include <util/helpers.h>
#include <validation.h>

#include <random.h>

#include <array>
#include <map>
#include <memory>

struct Member {
    CBLSId id;

    BLSVerificationVectorPtr vvec;
    std::vector<CBLSSecretKey> skShares;
};

class DKG
{
private:
    std::vector<CBLSId> ids;
    std::vector<Member> members;
    std::vector<BLSVerificationVectorPtr> receivedVvecs;

    std::vector<CBLSSecretKey> receivedSkShares;
    BLSVerificationVectorPtr quorumVvec;
    CBLSWorker blsWorker;

    void ReceiveVvecs()
    {
        receivedVvecs.clear();
        for (const auto& member : members) {
            receivedVvecs.emplace_back(member.vvec);
        }
        quorumVvec = blsWorker.BuildQuorumVerificationVector(receivedVvecs);
    }

    void ReceiveShares(size_t whoAmI)
    {
        receivedSkShares.clear();
        for (const auto& member : members) {
            receivedSkShares.emplace_back(member.skShares[whoAmI]);
        }
    }

    void VerifyContributionShares(size_t whoAmI, const std::set<size_t>& invalidIndexes, bool aggregated)
    {
        auto result = blsWorker.VerifyContributionShares(members[whoAmI].id, receivedVvecs, receivedSkShares,
                                                         /*parallel=*/true, aggregated);
        for (const size_t i : util::irange(receivedVvecs.size())) {
            if (invalidIndexes.count(i)) {
                assert(!result[i]);
            } else {
                assert(result[i]);
            }
        }
    }

public:
    explicit DKG(int quorumSize)
    {
        members.reserve(quorumSize);
        ids.reserve(quorumSize);

        for (const int i : util::irange(quorumSize)) {
            uint256 id;
            WriteLE64(id.begin(), i + 1);
            members.push_back({CBLSId(id), {}, {}});
            ids.emplace_back(id);
        }

        blsWorker.Start(llmq::DEFAULT_WORKER_COUNT);
        for (auto& member : members) {
            blsWorker.GenerateContributions(quorumSize / 2 + 1, ids, member.vvec, member.skShares);
        }
    }
    ~DKG()
    {
        blsWorker.Stop();
    }

    void Bench_BuildQuorumVerificationVectors(benchmark::Bench& bench, uint32_t epoch_iters)
    {
        ReceiveVvecs();

        bench.minEpochIterations(epoch_iters).run([&] {
            quorumVvec = blsWorker.BuildQuorumVerificationVector(receivedVvecs, false);
        });
    }

    void Bench_VerifyContributionShares(benchmark::Bench& bench, int invalidCount, bool aggregated, uint32_t epoch_iters)
    {
        ReceiveVvecs();
        size_t memberIdx = 0;
        bench.minEpochIterations(epoch_iters).run([&] {
            ReceiveShares(memberIdx);

            std::set<size_t> invalidIndexes;
            for ([[maybe_unused]] const auto _ : util::irange(invalidCount)) {
                size_t shareIdx = GetRand<size_t>(receivedSkShares.size());
                receivedSkShares[shareIdx].MakeNewKey();
                invalidIndexes.emplace(shareIdx);
            }

            VerifyContributionShares(memberIdx, invalidIndexes, aggregated);

            memberIdx = (memberIdx + 1) % members.size();
        });
    }
};

class CommitStep
{
private:
    const bool previousScheme{bls::bls_legacy_scheme.exchange(false)};
    std::vector<CBLSId> ids;
    std::vector<BLSVerificationVectorPtr> vvecs;
    std::vector<CBLSSecretKey> skContributions;
    std::vector<std::byte> serializedVvecs;
    BLSVerificationVectorPtr quorumVvec;
    CBLSSignature quorumSig;
    std::vector<CBLSPublicKey> operatorPubKeys;
    std::vector<CBLSSignature> memberSigs;
    std::vector<CBLSSignature> sharedHashMemberSigs;
    std::vector<uint256> messageHashes;
    CBLSWorker blsWorker;

    std::vector<BLSVerificationVectorPtr> DeserializeVvecs() const
    {
        CDataStream stream(serializedVvecs, SER_DISK, CLIENT_VERSION);
        std::vector<BLSVerificationVectorPtr> decoded;
        decoded.reserve(vvecs.size());
        for ([[maybe_unused]] const auto i : util::irange(vvecs.size())) {
            const size_t vvecSize = ReadCompactSize(stream);
            auto vvec = std::make_shared<std::vector<CBLSPublicKey>>();
            CBLSPublicKey pubkey;
            for ([[maybe_unused]] const auto j : util::irange(vvecSize)) {
                stream >> CBLSPublicKeyVersionWrapper(pubkey, false);
                vvec->emplace_back(pubkey);
            }
            decoded.emplace_back(std::move(vvec));
        }
        return decoded;
    }

public:
    CommitStep(size_t memberCount, size_t threshold)
    {
        ids.reserve(memberCount);
        vvecs.reserve(memberCount);
        skContributions.reserve(memberCount);
        for (const auto i : util::irange(memberCount)) {
            ids.emplace_back(::SerializeHash(static_cast<uint32_t>(i + 1)));

            auto vvec = std::make_shared<std::vector<CBLSPublicKey>>();
            vvec->reserve(threshold);
            std::vector<CBLSSecretKey> coefficients;
            coefficients.reserve(threshold);
            for (const auto j : util::irange(threshold)) {
                uint256 seed = ::SerializeHash(std::make_pair(static_cast<uint32_t>(i), static_cast<uint32_t>(j)));
                *seed.begin() = 0;
                CBLSSecretKey sk{Span{seed}};
                vvec->emplace_back(sk.GetPublicKey());
                coefficients.emplace_back(sk);
            }
            vvecs.emplace_back(std::move(vvec));

            CBLSSecretKey sk;
            const bool built{sk.SecretKeyShare(coefficients, ids[0])};
            assert(built);
            skContributions.emplace_back(std::move(sk));
        }

        CDataStream stream(SER_DISK, CLIENT_VERSION);
        for (const auto& vvec : vvecs) {
            WriteCompactSize(stream, vvec->size());
            for (auto& pubkey : *vvec) {
                stream << CBLSPublicKeyVersionWrapper(pubkey, false);
            }
        }
        serializedVvecs.assign(stream.begin(), stream.end());
        vvecs = DeserializeVvecs();
        for (const auto& vvec : vvecs) {
            assert(CBLSWorker::VerifyVerificationVector(*vvec));
        }

        blsWorker.Start(llmq::DEFAULT_WORKER_COUNT);
        quorumVvec = blsWorker.BuildQuorumVerificationVector(vvecs, false);
        assert(quorumVvec);
        const auto skShare = blsWorker.AggregateSecretKeys(skContributions, false);
        quorumSig = skShare.Sign(uint256::ONE, false);
        assert(quorumSig.VerifyInsecure(CBLSWorker::BuildPubKeyShare(quorumVvec, ids[0]), uint256::ONE, false));
        for (const auto i : util::irange(8)) {
            CBLSSecretKey signer;
            signer.MakeNewKey();
            const auto hash = ::SerializeHash(i);
            operatorPubKeys.emplace_back(signer.GetPublicKey());
            messageHashes.emplace_back(hash);
            memberSigs.emplace_back(signer.Sign(hash, false));
            sharedHashMemberSigs.emplace_back(signer.Sign(uint256::ONE, false));
        }
    }

    ~CommitStep()
    {
        blsWorker.Stop();
        bls::bls_legacy_scheme.store(previousScheme);
    }

    void BenchDeserialize(benchmark::Bench& bench)
    {
        bench.epochs(3).minEpochIterations(1).run([&] {
            auto decoded = DeserializeVvecs();
            ankerl::nanobench::doNotOptimizeAway(decoded);
        });
    }

    void BenchBuildQuorumVerificationVector(benchmark::Bench& bench)
    {
        bench.epochs(3).minEpochIterations(1).run([&] {
            auto result = blsWorker.BuildQuorumVerificationVector(vvecs, false);
            ankerl::nanobench::doNotOptimizeAway(result);
        });
    }

    void BenchFindSharedCoefficients(benchmark::Bench& bench)
    {
        bench.epochs(3).minEpochIterations(1).run([&] {
            auto result = CBLSWorker::FindVerificationVectorsWithSharedCoefficients(vvecs);
            ankerl::nanobench::doNotOptimizeAway(result);
        });
    }

    void BenchAggregateSecretKeys(benchmark::Bench& bench)
    {
        bench.epochs(3).minEpochIterations(1).run([&] {
            auto result = blsWorker.AggregateSecretKeys(skContributions, false);
            ankerl::nanobench::doNotOptimizeAway(result);
        });
    }

    void BenchPrematureCommitmentVerification(benchmark::Bench& bench)
    {
        const auto expectedVvecHash = ::SerializeHash(*quorumVvec);
        bench.epochs(3).minEpochIterations(1).run([&] {
            const auto vvecHash = ::SerializeHash(*quorumVvec);
            assert(vvecHash == expectedVvecHash);
            const auto pubKeyShare = CBLSWorker::BuildPubKeyShare(quorumVvec, ids[0]);
            const bool quorumValid = quorumSig.VerifyInsecure(pubKeyShare, uint256::ONE, false);
            // Commitments sharing a sign hash make the message batch verifier fall back to individual checks.
            const bool memberValid = sharedHashMemberSigs[0].VerifyInsecure(operatorPubKeys[0], uint256::ONE, false);
            assert(quorumValid && memberValid);
            ankerl::nanobench::doNotOptimizeAway(quorumValid);
            ankerl::nanobench::doNotOptimizeAway(memberValid);
        });
    }

    void BenchMemberSignatures(benchmark::Bench& bench)
    {
        bench.batch(8).minEpochIterations(1).run([&] {
            FastRandomContext rng;
            auto weightedSigs = memberSigs;
            auto weightedPubKeys = operatorPubKeys;
            for (const auto i : util::irange(memberSigs.size())) {
                const auto weight = MakeBatchVerificationWeight(rng);
                weightedSigs[i].MulInsecure(weight);
                weightedPubKeys[i].MulInsecure(weight);
            }
            auto sig = CBLSSignature::AggregateInsecure(weightedSigs);
            const bool valid = sig.VerifyInsecureAggregated(weightedPubKeys, messageHashes);
            assert(valid);
            ankerl::nanobench::doNotOptimizeAway(valid);
        });
    }

    void BenchEndToEnd(benchmark::Bench& bench)
    {
        const std::string testParams = strprintf("-llmqtestparams=%d:%d", vvecs.size(), vvecs[0]->size());
        const auto setup = MakeNoLogFileContext<TestingSetup>(ChainType::REGTEST, {testParams.c_str(), "-dip3params=1:1",
                                                                                   "-testactivationheight=v20@1"});
        bls::bls_legacy_scheme.store(false);
        auto& node = setup->m_node;
        std::array<uint256, 25> hashes;
        std::array<CBlockIndex, 25> blocks;
        for (const auto i : util::irange(blocks.size())) {
            hashes[i] = ::SerializeHash(static_cast<uint32_t>(i));
            blocks[i].phashBlock = &hashes[i];
            blocks[i].nHeight = i;
            blocks[i].pprev = i ? &blocks[i - 1] : nullptr;
            blocks[i].BuildSkip();
        }
        const auto* base = &blocks.back();
        const auto* work = base->GetAncestor(base->nHeight - llmq::WORK_DIFF_DEPTH);
        CDeterministicMNList mnList{work->GetBlockHash(), work->nHeight, static_cast<uint32_t>(vvecs.size())};
        for (const auto i : util::irange(vvecs.size())) {
            auto dmn = std::make_shared<CDeterministicMN>(i, MnType::Regular);
            dmn->proTxHash = ::SerializeHash(static_cast<uint32_t>(i + 1000));
            dmn->collateralOutpoint = COutPoint(dmn->proTxHash, 0);
            auto state = std::make_shared<CDeterministicMNState>();
            uint160 owner;
            WriteLE64(owner.begin(), i + 1);
            state->keyIDOwner = CKeyID{owner};
            state->netInfo = NetInfoInterface::MakeNetInfo(ProTxVersion::LegacyBLS);
            state->UpdateConfirmedHash(dmn->proTxHash, base->GetBlockHash());
            state->pubKeyOperator.Set((*vvecs[i])[0], false);
            dmn->pdmnState = std::move(state);
            mnList.AddMN(dmn);
        }
        node.dmnman->SetListForBlockForTesting(mnList);
        const auto type = Consensus::LLMQType::LLMQ_TEST;
        const auto members =
            llmq::utils::GetAllQuorumMembers(type, {*node.dmnman, *node.llmq_ctx->qsnapman, *node.chainman, base}, true);
        assert(members.size() == vvecs.size());
        llmq::CDKGSessionManager manager{*node.dmnman, *node.llmq_ctx->qsnapman, *node.chainman, *node.sporkman,
                                         util::DbWrapperParams{.memory = true}};
        for (const auto i : util::irange(members.size())) {
            manager.WriteVerifiedVvecContribution(type, base, members[i]->proTxHash, vvecs[i]);
            manager.WriteVerifiedSkContribution(type, base, members[i]->proTxHash, skContributions[i]);
        }
        const std::vector<bool> validMembers(vvecs.size(), true);
        CBLSSecretKey operatorKey;
        operatorKey.MakeNewKey();
        const auto expectedVvecHash = ::SerializeHash(*quorumVvec);
        const auto expectedCommitmentHash = llmq::BuildCommitmentHash(type, base->GetBlockHash(), validMembers,
                                                                      (*quorumVvec)[0], expectedVvecHash);
        const auto expectedSkShare = blsWorker.AggregateSecretKeys(skContributions, false);
        // Measure the first lookup after writes; repeated iterations would hide the cold-read cost.
        bench.epochs(1).epochIterations(1).run([&] {
            const auto shared = CBLSWorker::FindVerificationVectorsWithSharedCoefficients(vvecs);
            assert(shared.empty());
            std::vector<uint16_t> memberIndexes;
            std::vector<BLSVerificationVectorPtr> verifiedVvecs;
            std::vector<CBLSSecretKey> verifiedShares;
            const bool found = manager.GetVerifiedContributions(type, base, validMembers, memberIndexes, verifiedVvecs,
                                                                verifiedShares);
            assert(found);
            const auto qv = blsWorker.BuildQuorumVerificationVector(verifiedVvecs, false);
            assert(qv);
            const auto sk = blsWorker.AggregateSecretKeys(verifiedShares, false);
            const auto vvecHash = ::SerializeHash(*qv);
            const auto hash = llmq::BuildCommitmentHash(type, base->GetBlockHash(), validMembers, (*qv)[0], vvecHash);
            assert(vvecHash == expectedVvecHash);
            assert(hash == expectedCommitmentHash);
            assert(sk.ToBytes(false) == expectedSkShare.ToBytes(false));
            const auto sig = operatorKey.Sign(hash, false);
            const auto qs = sk.Sign(hash, false);
            ankerl::nanobench::doNotOptimizeAway(sig);
            ankerl::nanobench::doNotOptimizeAway(qs);
        });
        if (auto* out = bench.output()) *out << "# commitment " << expectedCommitmentHash.ToString() << '\n';
    }
};

static void BenchCommitStep(benchmark::Bench& bench, size_t threshold, void (*fn)(CommitStep&, benchmark::Bench&))
{
    const size_t memberCount = bench.output() ? 400 : 3;
    const size_t actualThreshold = bench.output() ? threshold : 2;
    // The secure allocator must outlive the fixtures' secret keys.
    [[maybe_unused]] static const CBLSSecretKey allocatorLifetime;
    static std::map<std::pair<size_t, size_t>, std::unique_ptr<CommitStep>> inputs;
    auto& step = inputs[{memberCount, actualThreshold}];
    if (!step) step = std::make_unique<CommitStep>(memberCount, actualThreshold);
    fn(*step, bench);
}

#define BENCH_COMMIT_STEP(name, threshold, method)                                                        \
    static void BLSDKG_CommitStep_##name##_##threshold(benchmark::Bench& bench)                           \
    {                                                                                                     \
        BenchCommitStep(bench, threshold, [](CommitStep& step, benchmark::Bench& b) { step.method(b); }); \
    }                                                                                                     \
    BENCHMARK(BLSDKG_CommitStep_##name##_##threshold, benchmark::PriorityLevel::HIGH)

BENCH_COMMIT_STEP(EndToEnd, 340, BenchEndToEnd)
BENCH_COMMIT_STEP(Deserialize, 340, BenchDeserialize)
BENCH_COMMIT_STEP(Deserialize, 240, BenchDeserialize)
BENCH_COMMIT_STEP(BuildQuorumVvec, 340, BenchBuildQuorumVerificationVector)
BENCH_COMMIT_STEP(BuildQuorumVvec, 240, BenchBuildQuorumVerificationVector)
BENCH_COMMIT_STEP(FindSharedCoefficients, 340, BenchFindSharedCoefficients)
BENCH_COMMIT_STEP(FindSharedCoefficients, 240, BenchFindSharedCoefficients)
BENCH_COMMIT_STEP(AggregateSecretKeys, 340, BenchAggregateSecretKeys)
BENCH_COMMIT_STEP(AggregateSecretKeys, 240, BenchAggregateSecretKeys)
BENCH_COMMIT_STEP(PrematureCommitmentVerification, 340, BenchPrematureCommitmentVerification)
BENCH_COMMIT_STEP(PrematureCommitmentVerification, 240, BenchPrematureCommitmentVerification)
BENCH_COMMIT_STEP(DistinctHashMemberBatch, 340, BenchMemberSignatures)

static void BLSDKG_GenerateContributions(benchmark::Bench& bench, uint32_t epoch_iters, int quorumSize)
{
    CBLSWorker blsWorker;
    blsWorker.Start(llmq::DEFAULT_WORKER_COUNT);
    std::vector<CBLSId> ids;
    std::vector<Member> members;
    if (!bench.output()) {
        epoch_iters = 1;
        quorumSize = 1;
    }
    for (const int i : util::irange(quorumSize)) {
        uint256 id;
        WriteLE64(id.begin(), i + 1);
        members.push_back({CBLSId(id), {}, {}});
        ids.emplace_back(id);
    }
    bench.minEpochIterations(epoch_iters).run([&blsWorker, &quorumSize, &ids, &members] {
        for (auto& member : members) {
            blsWorker.GenerateContributions(quorumSize / 2 + 1, ids, member.vvec, member.skShares);
        }
    });
    blsWorker.Stop();
}

#define BENCH_GenerateContributions(name, quorumSize, epoch_iters)                          \
    static void BLSDKG_GenerateContributions_##name##_##quorumSize(benchmark::Bench& bench) \
    {                                                                                       \
        BLSDKG_GenerateContributions(bench, epoch_iters, quorumSize);                       \
    }                                                                                       \
    BENCHMARK(BLSDKG_GenerateContributions_##name##_##quorumSize, benchmark::PriorityLevel::HIGH)

static void BLSDKG_InitDKG(benchmark::Bench& bench, uint32_t epoch_iters, int quorumSize)
{
    if (!bench.output()) {
        epoch_iters = 1;
        quorumSize = 1;
    }
    bench.minEpochIterations(epoch_iters).run([&] {
        DKG d(quorumSize);
    });
}

#define BENCH_InitDKG(name, quorumSize, epoch_iters)                          \
    static void BLSDKG_InitDKG_##name##_##quorumSize(benchmark::Bench& bench) \
    {                                                                         \
        BLSDKG_InitDKG(bench, epoch_iters, quorumSize);                       \
    }                                                                         \
    BENCHMARK(BLSDKG_InitDKG_##name##_##quorumSize, benchmark::PriorityLevel::HIGH)

#define BENCH_BuildQuorumVerificationVectors(name, quorumSize, epoch_iters)                          \
    static void BLSDKG_BuildQuorumVerificationVectors_##name##_##quorumSize(benchmark::Bench& bench) \
    {                                                                                                \
        if (!bench.output()) {                                                                       \
            std::unique_ptr<DKG> ptr = std::make_unique<DKG>(1);                                     \
            ptr->Bench_BuildQuorumVerificationVectors(bench, 1);                                     \
            ptr.reset();                                                                             \
            return;                                                                                  \
        }                                                                                            \
        std::unique_ptr<DKG> ptr = std::make_unique<DKG>(quorumSize);                                \
        ptr->Bench_BuildQuorumVerificationVectors(bench, epoch_iters);                               \
        ptr.reset();                                                                                 \
    }                                                                                                \
    BENCHMARK(BLSDKG_BuildQuorumVerificationVectors_##name##_##quorumSize, benchmark::PriorityLevel::HIGH)

#define BENCH_VerifyContributionShares(name, quorumSize, invalidCount, aggregated, epoch_iters) \
    static void BLSDKG_VerifyContributionShares_##name##_##quorumSize(benchmark::Bench& bench)  \
    {                                                                                           \
        if (!bench.output()) {                                                                  \
            std::unique_ptr<DKG> ptr = std::make_unique<DKG>(1);                                \
            ptr->Bench_VerifyContributionShares(bench, invalidCount, aggregated, 1);            \
            ptr.reset();                                                                        \
            return;                                                                             \
        }                                                                                       \
        std::unique_ptr<DKG> ptr = std::make_unique<DKG>(quorumSize);                           \
        ptr->Bench_VerifyContributionShares(bench, invalidCount, aggregated, epoch_iters);      \
        ptr.reset();                                                                            \
    }                                                                                           \
    BENCHMARK(BLSDKG_VerifyContributionShares_##name##_##quorumSize, benchmark::PriorityLevel::HIGH)

BENCH_GenerateContributions(simple, 50, 50);
BENCH_GenerateContributions(simple, 100, 5);

BENCH_InitDKG(simple, 10, 100)
BENCH_InitDKG(simple, 50, 10)

BENCH_BuildQuorumVerificationVectors(simple, 10, 1000)
BENCH_BuildQuorumVerificationVectors(simple, 100, 10)
BENCH_BuildQuorumVerificationVectors(simple, 400, 1)

BENCH_VerifyContributionShares(simple, 10, 5, false, 10)
BENCH_VerifyContributionShares(simple, 100, 5, false, 10)
BENCH_VerifyContributionShares(simple, 400, 5, false, 1)

BENCH_VerifyContributionShares(aggregated, 10, 5, true, 100)
BENCH_VerifyContributionShares(aggregated, 100, 5, true, 10)
BENCH_VerifyContributionShares(aggregated, 400, 5, true, 1)
