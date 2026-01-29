// Version vector tests

#include "version_vector.hpp"
#include "sync_engine.hpp"
#include <gtest/gtest.h>

using namespace ifex::scheduler;

TEST(VersionVectorTest, DefaultConstruction) {
    VersionVector v;
    EXPECT_EQ(v.cloud_seq, 0);
    EXPECT_EQ(v.vehicle_seq, 0);
}

TEST(VersionVectorTest, Construction) {
    VersionVector v(5, 3);
    EXPECT_EQ(v.cloud_seq, 5);
    EXPECT_EQ(v.vehicle_seq, 3);
}

TEST(VersionVectorTest, Equality) {
    VersionVector v1(1, 2);
    VersionVector v2(1, 2);
    VersionVector v3(1, 3);

    EXPECT_EQ(v1, v2);
    EXPECT_NE(v1, v3);
}

TEST(VersionVectorTest, DominatesBasic) {
    VersionVector v1(2, 2);
    VersionVector v2(1, 1);

    EXPECT_TRUE(v1.dominates(v2));
    EXPECT_FALSE(v2.dominates(v1));
}

TEST(VersionVectorTest, DominatesOneComponentGreater) {
    VersionVector v1(2, 1);
    VersionVector v2(1, 1);

    EXPECT_TRUE(v1.dominates(v2));
    EXPECT_FALSE(v2.dominates(v1));
}

TEST(VersionVectorTest, NeitherDominates) {
    VersionVector v1(2, 1);
    VersionVector v2(1, 2);

    EXPECT_FALSE(v1.dominates(v2));
    EXPECT_FALSE(v2.dominates(v1));
}

TEST(VersionVectorTest, EqualDoesNotDominate) {
    VersionVector v1(1, 1);
    VersionVector v2(1, 1);

    EXPECT_FALSE(v1.dominates(v2));
    EXPECT_FALSE(v2.dominates(v1));
}

TEST(VersionVectorTest, Merge) {
    VersionVector v1(2, 1);
    VersionVector v2(1, 3);
    VersionVector merged = VersionVector::merge(v1, v2);

    EXPECT_EQ(merged.cloud_seq, 2);
    EXPECT_EQ(merged.vehicle_seq, 3);
}

TEST(VersionVectorTest, Increment) {
    VersionVector v(1, 1);

    v.increment_cloud();
    EXPECT_EQ(v.cloud_seq, 2);
    EXPECT_EQ(v.vehicle_seq, 1);

    v.increment_vehicle();
    EXPECT_EQ(v.cloud_seq, 2);
    EXPECT_EQ(v.vehicle_seq, 2);
}

TEST(VersionVectorTest, CompareEqual) {
    VersionVector v1(1, 1);
    VersionVector v2(1, 1);

    EXPECT_EQ(compare(v1, v2), CompareResult::EQUAL);
}

TEST(VersionVectorTest, CompareLocalDominates) {
    VersionVector local(2, 2);
    VersionVector remote(1, 1);

    EXPECT_EQ(compare(local, remote), CompareResult::LOCAL_DOMINATES);
}

TEST(VersionVectorTest, CompareRemoteDominates) {
    VersionVector local(1, 1);
    VersionVector remote(2, 2);

    EXPECT_EQ(compare(local, remote), CompareResult::REMOTE_DOMINATES);
}

TEST(VersionVectorTest, CompareConflict) {
    VersionVector local(2, 1);
    VersionVector remote(1, 2);

    EXPECT_EQ(compare(local, remote), CompareResult::CONFLICT);
}

// SyncEngine tests

TEST(SyncEngineTest, NewJobAccepted) {
    VersionVector remote(1, 0);
    auto result = SyncEngine::process_remote(remote, std::nullopt, JobAuthority::CLOUD, false);

    EXPECT_EQ(result.action, SyncResult::ACCEPT_REMOTE);
}

TEST(SyncEngineTest, RemoteDominatesAccepted) {
    VersionVector remote(2, 2);
    VersionVector local(1, 1);

    auto result = SyncEngine::process_remote(remote, local, JobAuthority::CLOUD, false);

    EXPECT_EQ(result.action, SyncResult::ACCEPT_REMOTE);
}

TEST(SyncEngineTest, LocalDominatesRejected) {
    VersionVector remote(1, 1);
    VersionVector local(2, 2);

    auto result = SyncEngine::process_remote(remote, local, JobAuthority::CLOUD, false);

    EXPECT_EQ(result.action, SyncResult::REJECT_REMOTE);
}

TEST(SyncEngineTest, ConflictCloudAuthorityCloudWins) {
    VersionVector remote(2, 1);
    VersionVector local(1, 2);

    // Cloud authority - cloud wins
    auto result = SyncEngine::process_remote(remote, local, JobAuthority::CLOUD, true);

    EXPECT_EQ(result.action, SyncResult::CONFLICT_RESOLVED);
    EXPECT_EQ(result.winner, JobAuthority::CLOUD);
}

TEST(SyncEngineTest, ConflictVehicleAuthorityVehicleWins) {
    VersionVector remote(2, 1);
    VersionVector local(1, 2);

    // Vehicle authority - vehicle wins
    auto result = SyncEngine::process_remote(remote, local, JobAuthority::VEHICLE, false);

    EXPECT_EQ(result.action, SyncResult::CONFLICT_RESOLVED);
    EXPECT_EQ(result.winner, JobAuthority::VEHICLE);
}

TEST(SyncEngineTest, PrepareForLocalChange) {
    VersionVector v(1, 2);

    auto cloud_changed = SyncEngine::prepare_for_local_change(v, true);
    EXPECT_EQ(cloud_changed.cloud_seq, 2);
    EXPECT_EQ(cloud_changed.vehicle_seq, 2);

    auto vehicle_changed = SyncEngine::prepare_for_local_change(v, false);
    EXPECT_EQ(vehicle_changed.cloud_seq, 1);
    EXPECT_EQ(vehicle_changed.vehicle_seq, 3);
}

TEST(SyncEngineTest, GenerateJobId) {
    auto cloud_id = SyncEngine::generate_job_id("abc123", JobAuthority::CLOUD);
    EXPECT_EQ(cloud_id, "cloud-abc123");

    auto vehicle_id = SyncEngine::generate_job_id("def456", JobAuthority::VEHICLE, "VIN001");
    EXPECT_EQ(vehicle_id, "veh-VIN001-def456");
}
