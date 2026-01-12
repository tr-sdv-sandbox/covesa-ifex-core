/**
 * @file scheduler_sync_bridge_test.cpp
 * @brief Unit tests for SchedulerSyncBridge (v2 protocol)
 */

#include <gtest/gtest.h>
#include <glog/logging.h>

#include "scheduler_sync_bridge.hpp"
#include "ifex_content_ids.hpp"
#include "scheduler-sync-v2.pb.h"

namespace ifex::reference {
namespace {

class SchedulerSyncBridgeTest : public ::testing::Test {
protected:
    void SetUp() override {
    }

    void TearDown() override {
    }
};

// =============================================================================
// Unit Tests (no external dependencies)
// =============================================================================

TEST_F(SchedulerSyncBridgeTest, ConfigDefaults) {
    SchedulerSyncBridgeConfig config;
    EXPECT_EQ(config.scheduler_endpoint, "localhost:50053");
    EXPECT_EQ(config.backend_transport_endpoint, "localhost:50060");
    EXPECT_EQ(config.sync_content_id, 202);
    EXPECT_EQ(config.vehicle_id, "vehicle-001");
    EXPECT_EQ(config.initialization_delay_ms, 5000);
    EXPECT_EQ(config.poll_interval_ms, 1000);
    EXPECT_EQ(config.batch_window_ms, 100);
    EXPECT_EQ(config.heartbeat_interval_ms, 30000);
    EXPECT_TRUE(config.terminal_states_only);
    EXPECT_TRUE(config.state_persistence_path.empty());
}

TEST_F(SchedulerSyncBridgeTest, ContentIdConstants) {
    EXPECT_EQ(ifex::content_id::DISCOVERY_SYNC, 201);
    EXPECT_EQ(ifex::content_id::SCHEDULER_SYNC, 202);
}

// =============================================================================
// V2 Protocol Message Tests
// =============================================================================

TEST_F(SchedulerSyncBridgeTest, V2JobRecordSerialization) {
    swdv::scheduler_sync_v2::JobRecord record;
    record.set_job_id("job_001");
    record.set_title("Test Job");
    record.set_service("test-service");
    record.set_method("test_method");
    record.set_parameters_json("{}");
    record.set_scheduled_time_ms(1234567890000);
    record.set_status(swdv::scheduler_sync_v2::JOB_STATUS_PENDING);
    record.set_created_at_ms(1234567890000);
    record.set_updated_at_ms(1234567890000);
    record.set_authority(swdv::scheduler_sync_v2::AUTHORITY_VEHICLE);
    record.set_sync_state(swdv::scheduler_sync_v2::SYNC_STATE_SYNCED);

    // Set version vector
    auto* version = record.mutable_version();
    version->set_cloud_seq(1);
    version->set_vehicle_seq(2);

    std::string serialized;
    ASSERT_TRUE(record.SerializeToString(&serialized));
    EXPECT_GT(serialized.size(), 0);

    swdv::scheduler_sync_v2::JobRecord parsed;
    ASSERT_TRUE(parsed.ParseFromString(serialized));
    EXPECT_EQ(parsed.job_id(), "job_001");
    EXPECT_EQ(parsed.title(), "Test Job");
    EXPECT_EQ(parsed.service(), "test-service");
    EXPECT_EQ(parsed.status(), swdv::scheduler_sync_v2::JOB_STATUS_PENDING);
    EXPECT_EQ(parsed.version().cloud_seq(), 1);
    EXPECT_EQ(parsed.version().vehicle_seq(), 2);
}

TEST_F(SchedulerSyncBridgeTest, V2SyncMessageSerialization) {
    swdv::scheduler_sync_v2::V2C_SyncMessage message;
    message.set_sync_id("sync_001");
    message.set_vehicle_id("vehicle-001");
    message.set_bridge_instance_id("ssb_1234567890abcdef");
    message.set_sync_timestamp_ms(1234567890000);

    // Add a job record
    auto* job = message.add_jobs();
    job->set_job_id("job_001");
    job->set_title("Test Job");
    job->set_status(swdv::scheduler_sync_v2::JOB_STATUS_PENDING);

    std::string serialized;
    ASSERT_TRUE(message.SerializeToString(&serialized));
    EXPECT_GT(serialized.size(), 0);

    swdv::scheduler_sync_v2::V2C_SyncMessage parsed;
    ASSERT_TRUE(parsed.ParseFromString(serialized));
    EXPECT_EQ(parsed.sync_id(), "sync_001");
    EXPECT_EQ(parsed.vehicle_id(), "vehicle-001");
    EXPECT_EQ(parsed.bridge_instance_id(), "ssb_1234567890abcdef");
    EXPECT_EQ(parsed.jobs_size(), 1);
    EXPECT_EQ(parsed.jobs(0).job_id(), "job_001");
}

TEST_F(SchedulerSyncBridgeTest, V2ExecutionRecordSerialization) {
    swdv::scheduler_sync_v2::ExecutionRecord exec;
    exec.set_execution_id("exec_001");
    exec.set_job_id("job_001");
    exec.set_executed_at_ms(1234567890000);
    exec.set_duration_ms(500);
    exec.set_status(swdv::scheduler_sync_v2::JOB_STATUS_COMPLETED);
    exec.set_result_json("{\"success\": true}");

    std::string serialized;
    ASSERT_TRUE(exec.SerializeToString(&serialized));

    swdv::scheduler_sync_v2::ExecutionRecord parsed;
    ASSERT_TRUE(parsed.ParseFromString(serialized));
    EXPECT_EQ(parsed.job_id(), "job_001");
    EXPECT_EQ(parsed.status(), swdv::scheduler_sync_v2::JOB_STATUS_COMPLETED);
    EXPECT_EQ(parsed.duration_ms(), 500);
    EXPECT_EQ(parsed.result_json(), "{\"success\": true}");
}

TEST_F(SchedulerSyncBridgeTest, V2JobStatusValues) {
    EXPECT_EQ(swdv::scheduler_sync_v2::JOB_STATUS_UNKNOWN, 0);
    EXPECT_EQ(swdv::scheduler_sync_v2::JOB_STATUS_PENDING, 1);
    EXPECT_EQ(swdv::scheduler_sync_v2::JOB_STATUS_RUNNING, 2);
    EXPECT_EQ(swdv::scheduler_sync_v2::JOB_STATUS_COMPLETED, 3);
    EXPECT_EQ(swdv::scheduler_sync_v2::JOB_STATUS_FAILED, 4);
    EXPECT_EQ(swdv::scheduler_sync_v2::JOB_STATUS_CANCELLED, 5);
}

TEST_F(SchedulerSyncBridgeTest, V2AuthorityValues) {
    EXPECT_EQ(swdv::scheduler_sync_v2::AUTHORITY_UNKNOWN, 0);
    EXPECT_EQ(swdv::scheduler_sync_v2::AUTHORITY_CLOUD, 1);
    EXPECT_EQ(swdv::scheduler_sync_v2::AUTHORITY_VEHICLE, 2);
}

TEST_F(SchedulerSyncBridgeTest, V2SyncStateValues) {
    EXPECT_EQ(swdv::scheduler_sync_v2::SYNC_STATE_UNKNOWN, 0);
    EXPECT_EQ(swdv::scheduler_sync_v2::SYNC_STATE_PENDING, 1);
    EXPECT_EQ(swdv::scheduler_sync_v2::SYNC_STATE_SYNCED, 2);
    EXPECT_EQ(swdv::scheduler_sync_v2::SYNC_STATE_CONFLICT, 3);
}

// =============================================================================
// SyncedJobState Tests (uses v2 types)
// =============================================================================

TEST_F(SchedulerSyncBridgeTest, SyncedJobStateHash) {
    SyncedJobState state1;
    state1.job_id = "job_001";
    state1.title = "Test Job";
    state1.service = "test-service";
    state1.method = "test_method";
    state1.parameters = "{}";
    state1.scheduled_time = "2025-01-01T10:00:00Z";
    state1.recurrence_rule = "";
    state1.next_run_time = "";
    state1.status = swdv::scheduler_sync_v2::JOB_STATUS_PENDING;
    state1.created_at_ms = 1000;
    state1.updated_at_ms = 1000;

    SyncedJobState state2 = state1;

    // Same state should have same hash
    EXPECT_EQ(state1.ComputeHash(), state2.ComputeHash());

    // Different status should have different hash
    state2.status = swdv::scheduler_sync_v2::JOB_STATUS_RUNNING;
    EXPECT_NE(state1.ComputeHash(), state2.ComputeHash());

    // Different scheduled_time should have different hash
    state2 = state1;
    state2.scheduled_time = "2025-02-01T12:00:00Z";
    EXPECT_NE(state1.ComputeHash(), state2.ComputeHash());

    // Different title should have different hash
    state2 = state1;
    state2.title = "Other Job";
    EXPECT_NE(state1.ComputeHash(), state2.ComputeHash());

    // updated_at_ms is excluded from hash (it's metadata, not content)
    state2 = state1;
    state2.updated_at_ms = 9999;
    EXPECT_EQ(state1.ComputeHash(), state2.ComputeHash());
}

TEST_F(SchedulerSyncBridgeTest, SyncedJobStateIsTerminal) {
    SyncedJobState state;

    state.status = swdv::scheduler_sync_v2::JOB_STATUS_PENDING;
    EXPECT_FALSE(state.IsTerminal());

    state.status = swdv::scheduler_sync_v2::JOB_STATUS_RUNNING;
    EXPECT_FALSE(state.IsTerminal());

    state.status = swdv::scheduler_sync_v2::JOB_STATUS_COMPLETED;
    EXPECT_TRUE(state.IsTerminal());

    state.status = swdv::scheduler_sync_v2::JOB_STATUS_FAILED;
    EXPECT_TRUE(state.IsTerminal());

    state.status = swdv::scheduler_sync_v2::JOB_STATUS_CANCELLED;
    EXPECT_TRUE(state.IsTerminal());
}

TEST_F(SchedulerSyncBridgeTest, SyncedJobStateToJobRecord) {
    SyncedJobState state;
    state.job_id = "job_001";
    state.title = "Test Job";
    state.service = "test-service";
    state.method = "test_method";
    state.parameters = "{\"key\": \"value\"}";
    state.scheduled_time_ms = 1234567890000;
    state.status = swdv::scheduler_sync_v2::JOB_STATUS_PENDING;
    state.authority = swdv::scheduler_sync_v2::AUTHORITY_VEHICLE;
    state.sync_state = swdv::scheduler_sync_v2::SYNC_STATE_SYNCED;
    state.version = sync::VersionVector{1, 2};

    swdv::scheduler_sync_v2::JobRecord record;
    state.ToJobRecord(&record);

    EXPECT_EQ(record.job_id(), "job_001");
    EXPECT_EQ(record.title(), "Test Job");
    EXPECT_EQ(record.service(), "test-service");
    EXPECT_EQ(record.method(), "test_method");
    EXPECT_EQ(record.parameters_json(), "{\"key\": \"value\"}");
    EXPECT_EQ(record.scheduled_time_ms(), 1234567890000);
    EXPECT_EQ(record.status(), swdv::scheduler_sync_v2::JOB_STATUS_PENDING);
    EXPECT_EQ(record.authority(), swdv::scheduler_sync_v2::AUTHORITY_VEHICLE);
    EXPECT_EQ(record.sync_state(), swdv::scheduler_sync_v2::SYNC_STATE_SYNCED);
    EXPECT_EQ(record.version().cloud_seq(), 1);
    EXPECT_EQ(record.version().vehicle_seq(), 2);
}

TEST_F(SchedulerSyncBridgeTest, SyncedJobStateFromJobRecord) {
    swdv::scheduler_sync_v2::JobRecord record;
    record.set_job_id("job_002");
    record.set_title("Cloud Job");
    record.set_service("cloud-service");
    record.set_method("cloud_method");
    record.set_parameters_json("{\"cloud\": true}");
    record.set_scheduled_time_ms(9876543210000);
    record.set_status(swdv::scheduler_sync_v2::JOB_STATUS_COMPLETED);
    record.set_authority(swdv::scheduler_sync_v2::AUTHORITY_CLOUD);
    record.set_sync_state(swdv::scheduler_sync_v2::SYNC_STATE_PENDING);
    record.mutable_version()->set_cloud_seq(5);
    record.mutable_version()->set_vehicle_seq(3);

    SyncedJobState state = SyncedJobState::FromJobRecord(record);

    EXPECT_EQ(state.job_id, "job_002");
    EXPECT_EQ(state.title, "Cloud Job");
    EXPECT_EQ(state.service, "cloud-service");
    EXPECT_EQ(state.method, "cloud_method");
    EXPECT_EQ(state.parameters, "{\"cloud\": true}");
    EXPECT_EQ(state.scheduled_time_ms, 9876543210000);
    EXPECT_EQ(state.status, swdv::scheduler_sync_v2::JOB_STATUS_COMPLETED);
    EXPECT_EQ(state.authority, swdv::scheduler_sync_v2::AUTHORITY_CLOUD);
    EXPECT_EQ(state.sync_state, swdv::scheduler_sync_v2::SYNC_STATE_PENDING);
    EXPECT_EQ(state.version.cloud_seq, 5);
    EXPECT_EQ(state.version.vehicle_seq, 3);
}

// =============================================================================
// V2 Cloud-to-Vehicle Message Tests
// =============================================================================

TEST_F(SchedulerSyncBridgeTest, V2C2VSyncMessageSerialization) {
    swdv::scheduler_sync_v2::C2V_SyncMessage message;
    message.set_sync_id("c2v_sync_001");
    message.set_vehicle_id("vehicle-001");
    message.set_sync_timestamp_ms(1234567890000);

    // Add a job from cloud
    auto* job = message.add_jobs();
    job->set_job_id("cloud_job_001");
    job->set_title("Cloud Scheduled Task");
    job->set_status(swdv::scheduler_sync_v2::JOB_STATUS_PENDING);
    job->set_authority(swdv::scheduler_sync_v2::AUTHORITY_CLOUD);

    std::string serialized;
    ASSERT_TRUE(message.SerializeToString(&serialized));

    swdv::scheduler_sync_v2::C2V_SyncMessage parsed;
    ASSERT_TRUE(parsed.ParseFromString(serialized));
    EXPECT_EQ(parsed.sync_id(), "c2v_sync_001");
    EXPECT_EQ(parsed.vehicle_id(), "vehicle-001");
    EXPECT_EQ(parsed.jobs_size(), 1);
    EXPECT_EQ(parsed.jobs(0).authority(), swdv::scheduler_sync_v2::AUTHORITY_CLOUD);
}

TEST_F(SchedulerSyncBridgeTest, V2SyncAckSerialization) {
    swdv::scheduler_sync_v2::SyncAck ack;
    ack.set_sync_id("sync_001");
    ack.set_success(true);
    ack.set_ack_timestamp_ms(1234567890000);

    // Add a conflict resolution
    auto* conflict = ack.add_conflicts();
    conflict->set_job_id("job_001");
    conflict->set_winner("cloud");
    conflict->set_reason("authority");

    std::string serialized;
    ASSERT_TRUE(ack.SerializeToString(&serialized));

    swdv::scheduler_sync_v2::SyncAck parsed;
    ASSERT_TRUE(parsed.ParseFromString(serialized));
    EXPECT_EQ(parsed.sync_id(), "sync_001");
    EXPECT_TRUE(parsed.success());
    EXPECT_EQ(parsed.conflicts_size(), 1);
    EXPECT_EQ(parsed.conflicts(0).winner(), "cloud");
    EXPECT_EQ(parsed.conflicts(0).reason(), "authority");
}

// =============================================================================
// Integration Tests (require running services + Docker for MQTT)
// =============================================================================

// These tests are marked as DISABLED_ because they require:
// 1. Backend Transport service running
// 2. Scheduler service running
// 3. MQTT broker (via Docker)
// Run them manually with: --gtest_also_run_disabled_tests

TEST_F(SchedulerSyncBridgeTest, DISABLED_StartStop) {
    SchedulerSyncBridgeConfig config;
    config.scheduler_endpoint = "localhost:50053";
    config.backend_transport_endpoint = "localhost:50060";
    config.initialization_delay_ms = 100;  // Short delay for testing

    SchedulerSyncBridge bridge(config);
    ASSERT_TRUE(bridge.Start());
    EXPECT_TRUE(bridge.IsRunning());

    // Wait for initialization
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_TRUE(bridge.IsInitialized());

    bridge.Stop();
    EXPECT_FALSE(bridge.IsRunning());
}

TEST_F(SchedulerSyncBridgeTest, DISABLED_StatsInitiallyZero) {
    SchedulerSyncBridgeConfig config;
    SchedulerSyncBridge bridge(config);

    auto stats = bridge.GetStats();
    EXPECT_EQ(stats.events_sent, 0);
    EXPECT_EQ(stats.full_syncs_sent, 0);
    EXPECT_EQ(stats.delta_syncs_sent, 0);
    EXPECT_EQ(stats.execution_results_sent, 0);
    EXPECT_EQ(stats.heartbeats_sent, 0);
    EXPECT_EQ(stats.bytes_sent, 0);
    EXPECT_FALSE(stats.is_initialized);
    EXPECT_FALSE(stats.is_connected);
}

}  // namespace
}  // namespace ifex::reference

int main(int argc, char** argv) {
    google::InitGoogleLogging(argv[0]);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
