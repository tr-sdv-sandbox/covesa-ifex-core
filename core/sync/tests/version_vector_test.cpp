// Unit tests for Scheduler Sync Protocol v2 - Version Vector and Sync Engine

#include <gtest/gtest.h>
#include "version_vector.hpp"
#include "sync_engine.hpp"

using namespace ifex::sync;

// =============================================================================
// Version Vector Tests
// =============================================================================

class VersionVectorTest : public ::testing::Test {};

TEST_F(VersionVectorTest, DefaultConstruction) {
    VersionVector v;
    EXPECT_EQ(v.cloud_seq, 0);
    EXPECT_EQ(v.vehicle_seq, 0);
}

TEST_F(VersionVectorTest, ParameterizedConstruction) {
    VersionVector v(5, 3);
    EXPECT_EQ(v.cloud_seq, 5);
    EXPECT_EQ(v.vehicle_seq, 3);
}

TEST_F(VersionVectorTest, Equality) {
    VersionVector a(1, 2);
    VersionVector b(1, 2);
    VersionVector c(1, 3);

    EXPECT_TRUE(a.equals(b));
    EXPECT_TRUE(a == b);
    EXPECT_FALSE(a.equals(c));
    EXPECT_TRUE(a != c);
}

TEST_F(VersionVectorTest, Dominance_CloudHigher) {
    VersionVector a(5, 3);
    VersionVector b(4, 3);

    EXPECT_TRUE(a.dominates(b));
    EXPECT_FALSE(b.dominates(a));
}

TEST_F(VersionVectorTest, Dominance_VehicleHigher) {
    VersionVector a(3, 5);
    VersionVector b(3, 4);

    EXPECT_TRUE(a.dominates(b));
    EXPECT_FALSE(b.dominates(a));
}

TEST_F(VersionVectorTest, Dominance_BothHigher) {
    VersionVector a(5, 5);
    VersionVector b(4, 4);

    EXPECT_TRUE(a.dominates(b));
    EXPECT_FALSE(b.dominates(a));
}

TEST_F(VersionVectorTest, Dominance_Equal) {
    VersionVector a(3, 3);
    VersionVector b(3, 3);

    // Equal versions don't dominate each other
    EXPECT_FALSE(a.dominates(b));
    EXPECT_FALSE(b.dominates(a));
}

TEST_F(VersionVectorTest, Conflict_ConcurrentModifications) {
    // Cloud modified but not vehicle
    VersionVector a(5, 3);
    // Vehicle modified but not cloud
    VersionVector b(4, 4);

    // Neither dominates the other - this is a conflict
    EXPECT_FALSE(a.dominates(b));
    EXPECT_FALSE(b.dominates(a));
}

TEST_F(VersionVectorTest, Merge) {
    VersionVector a(5, 3);
    VersionVector b(4, 6);

    VersionVector merged = VersionVector::merge(a, b);

    EXPECT_EQ(merged.cloud_seq, 5);   // max(5, 4)
    EXPECT_EQ(merged.vehicle_seq, 6); // max(3, 6)

    // Merged version dominates both inputs
    EXPECT_TRUE(merged.dominates(a));
    EXPECT_TRUE(merged.dominates(b));
}

TEST_F(VersionVectorTest, Increment) {
    VersionVector v(3, 5);

    v.increment_cloud();
    EXPECT_EQ(v.cloud_seq, 4);
    EXPECT_EQ(v.vehicle_seq, 5);

    v.increment_vehicle();
    EXPECT_EQ(v.cloud_seq, 4);
    EXPECT_EQ(v.vehicle_seq, 6);
}

TEST_F(VersionVectorTest, ToString) {
    VersionVector v(10, 20);
    std::string s = v.to_string();
    EXPECT_NE(s.find("10"), std::string::npos);
    EXPECT_NE(s.find("20"), std::string::npos);
}

// =============================================================================
// Compare Function Tests
// =============================================================================

class CompareTest : public ::testing::Test {};

TEST_F(CompareTest, Equal) {
    VersionVector a(3, 3);
    VersionVector b(3, 3);
    EXPECT_EQ(compare(a, b), CompareResult::EQUAL);
}

TEST_F(CompareTest, LocalDominates) {
    VersionVector local(5, 4);
    VersionVector remote(4, 4);
    EXPECT_EQ(compare(local, remote), CompareResult::LOCAL_DOMINATES);
}

TEST_F(CompareTest, RemoteDominates) {
    VersionVector local(4, 4);
    VersionVector remote(5, 4);
    EXPECT_EQ(compare(local, remote), CompareResult::REMOTE_DOMINATES);
}

TEST_F(CompareTest, Conflict) {
    VersionVector local(5, 3);
    VersionVector remote(4, 4);
    EXPECT_EQ(compare(local, remote), CompareResult::CONFLICT);
}

// =============================================================================
// Sync Engine Tests
// =============================================================================

class SyncEngineTest : public ::testing::Test {};

TEST_F(SyncEngineTest, NewJob_Accept) {
    VersionVector remote(1, 0);  // Cloud created job

    SyncResult result = SyncEngine::process_remote(
        remote,
        std::nullopt,  // No local version - new job
        JobAuthority::CLOUD,
        false  // We are vehicle
    );

    EXPECT_EQ(result.action, SyncResult::ACCEPT_REMOTE);
}

TEST_F(SyncEngineTest, SameVersion_NoAction) {
    VersionVector version(5, 3);

    SyncResult result = SyncEngine::process_remote(
        version,
        version,  // Same local and remote
        JobAuthority::CLOUD,
        false
    );

    EXPECT_EQ(result.action, SyncResult::NO_ACTION);
}

TEST_F(SyncEngineTest, RemoteDominates_Accept) {
    VersionVector local(4, 3);
    VersionVector remote(5, 3);

    SyncResult result = SyncEngine::process_remote(
        remote,
        local,
        JobAuthority::CLOUD,
        false
    );

    EXPECT_EQ(result.action, SyncResult::ACCEPT_REMOTE);
}

TEST_F(SyncEngineTest, LocalDominates_Reject) {
    VersionVector local(5, 3);
    VersionVector remote(4, 3);

    SyncResult result = SyncEngine::process_remote(
        remote,
        local,
        JobAuthority::CLOUD,
        false
    );

    EXPECT_EQ(result.action, SyncResult::REJECT_REMOTE);
}

TEST_F(SyncEngineTest, Conflict_CloudAuthority_CloudWins) {
    VersionVector local(5, 3);   // Cloud modified
    VersionVector remote(4, 4);  // Vehicle modified concurrently

    // On vehicle side, cloud-authoritative job
    SyncResult result = SyncEngine::process_remote(
        remote,
        local,
        JobAuthority::CLOUD,
        false  // We are vehicle
    );

    EXPECT_EQ(result.action, SyncResult::CONFLICT_RESOLVED);
    EXPECT_EQ(result.winner, "cloud");
}

TEST_F(SyncEngineTest, Conflict_VehicleAuthority_VehicleWins) {
    VersionVector local(5, 3);   // Cloud modified
    VersionVector remote(4, 4);  // Vehicle modified concurrently

    // On cloud side, vehicle-authoritative job
    SyncResult result = SyncEngine::process_remote(
        remote,
        local,
        JobAuthority::VEHICLE,
        true  // We are cloud
    );

    EXPECT_EQ(result.action, SyncResult::CONFLICT_RESOLVED);
    EXPECT_EQ(result.winner, "vehicle");
}

TEST_F(SyncEngineTest, ConflictResolution_MergesVersions) {
    VersionVector local(5, 3);
    VersionVector remote(4, 6);

    SyncResult result = SyncEngine::resolve_conflict(
        local, remote,
        JobAuthority::CLOUD,
        true  // We are cloud
    );

    // After merge and increment, cloud should be 6 (max 5,4 + 1)
    EXPECT_EQ(result.resolved_version.cloud_seq, 6);
    // Vehicle stays at max: 6
    EXPECT_EQ(result.resolved_version.vehicle_seq, 6);
}

TEST_F(SyncEngineTest, PrepareForLocalChange_Cloud) {
    VersionVector current(5, 3);

    VersionVector next = SyncEngine::prepare_for_local_change(current, true);

    EXPECT_EQ(next.cloud_seq, 6);
    EXPECT_EQ(next.vehicle_seq, 3);
}

TEST_F(SyncEngineTest, PrepareForLocalChange_Vehicle) {
    VersionVector current(5, 3);

    VersionVector next = SyncEngine::prepare_for_local_change(current, false);

    EXPECT_EQ(next.cloud_seq, 5);
    EXPECT_EQ(next.vehicle_seq, 4);
}

TEST_F(SyncEngineTest, ValidateNewJob_CloudSide) {
    EXPECT_TRUE(SyncEngine::validate_new_job(JobAuthority::CLOUD, true));
    EXPECT_FALSE(SyncEngine::validate_new_job(JobAuthority::VEHICLE, true));
}

TEST_F(SyncEngineTest, ValidateNewJob_VehicleSide) {
    EXPECT_TRUE(SyncEngine::validate_new_job(JobAuthority::VEHICLE, false));
    EXPECT_FALSE(SyncEngine::validate_new_job(JobAuthority::CLOUD, false));
}

TEST_F(SyncEngineTest, GenerateJobId_Cloud) {
    std::string id = SyncEngine::generate_job_id("abc123", JobAuthority::CLOUD);
    EXPECT_EQ(id, "cloud-abc123");
}

TEST_F(SyncEngineTest, GenerateJobId_Vehicle) {
    std::string id = SyncEngine::generate_job_id("xyz789", JobAuthority::VEHICLE, "VIN123");
    EXPECT_EQ(id, "veh-VIN123-xyz789");
}

// =============================================================================
// Edge Case Tests (from spec Appendix B)
// =============================================================================

class SyncEdgeCaseTest : public ::testing::Test {};

// Scenario: Both offline, both modify same job, cloud-authoritative
TEST_F(SyncEdgeCaseTest, BothOffline_SameJob_CloudWins) {
    // Initial synced state
    VersionVector initial(3, 3);

    // Cloud modifies while offline
    VersionVector cloud_version(4, 3);

    // Vehicle modifies while offline
    VersionVector vehicle_version(3, 4);

    // Cloud receives vehicle's version
    SyncResult cloud_result = SyncEngine::process_remote(
        vehicle_version, cloud_version,
        JobAuthority::CLOUD,
        true  // We are cloud
    );

    EXPECT_EQ(cloud_result.action, SyncResult::CONFLICT_RESOLVED);
    EXPECT_EQ(cloud_result.winner, "cloud");

    // Vehicle receives cloud's version
    SyncResult vehicle_result = SyncEngine::process_remote(
        cloud_version, vehicle_version,
        JobAuthority::CLOUD,
        false  // We are vehicle
    );

    EXPECT_EQ(vehicle_result.action, SyncResult::CONFLICT_RESOLVED);
    EXPECT_EQ(vehicle_result.winner, "cloud");
}

// Scenario: Delete vs modify conflict
TEST_F(SyncEdgeCaseTest, DeleteVsModify_AuthorityWins) {
    // Cloud deleted the job (has deleted flag)
    VersionVector cloud_version(5, 3);  // deleted=true in job record

    // Vehicle modified the job while offline
    VersionVector vehicle_version(4, 4);

    // Both sides should resolve to same winner based on authority
    // (actual delete handling is in the job record, not version vector)
    SyncResult result = SyncEngine::process_remote(
        vehicle_version, cloud_version,
        JobAuthority::CLOUD,
        true
    );

    EXPECT_EQ(result.action, SyncResult::CONFLICT_RESOLVED);
    EXPECT_EQ(result.winner, "cloud");
}

// Scenario: Multiple rapid changes on one side
TEST_F(SyncEdgeCaseTest, MultipleRapidChanges) {
    VersionVector initial(0, 0);

    // Cloud makes 5 rapid changes
    VersionVector after_5_changes = initial;
    for (int i = 0; i < 5; i++) {
        after_5_changes = SyncEngine::prepare_for_local_change(after_5_changes, true);
    }

    EXPECT_EQ(after_5_changes.cloud_seq, 5);
    EXPECT_EQ(after_5_changes.vehicle_seq, 0);

    // Vehicle with initial version receives this
    SyncResult result = SyncEngine::process_remote(
        after_5_changes,
        initial,
        JobAuthority::CLOUD,
        false
    );

    // Should accept - remote clearly dominates
    EXPECT_EQ(result.action, SyncResult::ACCEPT_REMOTE);
}

// Scenario: Long offline period with many changes on both sides
TEST_F(SyncEdgeCaseTest, LongOffline_ManyChanges) {
    VersionVector initial(10, 10);

    // Simulate 100 cloud changes
    VersionVector cloud_version(110, 10);

    // Simulate 50 vehicle changes
    VersionVector vehicle_version(10, 60);

    // This is a conflict - neither dominates
    EXPECT_EQ(compare(cloud_version, vehicle_version), CompareResult::CONFLICT);

    // Resolution should work regardless of magnitude
    SyncResult result = SyncEngine::resolve_conflict(
        cloud_version, vehicle_version,
        JobAuthority::CLOUD,
        false  // We are vehicle
    );

    EXPECT_EQ(result.action, SyncResult::CONFLICT_RESOLVED);
    EXPECT_EQ(result.winner, "cloud");

    // Merged version should dominate both original versions
    EXPECT_GE(result.resolved_version.cloud_seq, 110);
    EXPECT_GE(result.resolved_version.vehicle_seq, 60);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
