/**
 * @file scheduler_sync_bridge_test.cpp
 * @brief Unit tests for SchedulerSyncBridge
 */

#include <gtest/gtest.h>
#include <glog/logging.h>

#include "scheduler_sync_bridge.hpp"
#include "ifex_content_ids.hpp"
#include "scheduler-sync-envelope.pb.h"

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

TEST_F(SchedulerSyncBridgeTest, SyncEventTypeSerialization) {
    swdv::scheduler_sync_envelope::sync_event_t event;
    event.set_event_type(swdv::scheduler_sync_envelope::JOB_CREATED);
    event.set_sequence_number(42);
    event.set_timestamp_ms(1234567890000);
    event.set_job_id("job_001");

    auto* info = event.mutable_job_info();
    info->set_job_id("job_001");
    info->set_title("Test Job");
    info->set_service("test-service");
    info->set_method("test_method");
    info->set_parameters("{}");
    info->set_scheduled_time("2025-01-01T10:00:00Z");
    info->set_status(swdv::scheduler_sync_envelope::PENDING);
    info->set_created_at_ms(1234567890000);
    info->set_updated_at_ms(1234567890000);

    std::string serialized;
    ASSERT_TRUE(event.SerializeToString(&serialized));
    EXPECT_GT(serialized.size(), 0);

    swdv::scheduler_sync_envelope::sync_event_t parsed;
    ASSERT_TRUE(parsed.ParseFromString(serialized));
    EXPECT_EQ(parsed.event_type(), swdv::scheduler_sync_envelope::JOB_CREATED);
    EXPECT_EQ(parsed.sequence_number(), 42);
    EXPECT_EQ(parsed.job_id(), "job_001");
    EXPECT_EQ(parsed.job_info().title(), "Test Job");
    EXPECT_EQ(parsed.job_info().service(), "test-service");
    EXPECT_EQ(parsed.job_info().status(), swdv::scheduler_sync_envelope::PENDING);
}

TEST_F(SchedulerSyncBridgeTest, SyncMessageSerialization) {
    swdv::scheduler_sync_envelope::sync_message_t message;
    message.set_vehicle_id("vehicle-001");
    message.set_bridge_instance_id("ssb_1234567890abcdef");
    message.set_state_checksum(0xDEADBEEF);
    message.set_active_jobs_count(5);

    // Add FULL_SYNC event
    auto* event = message.add_events();
    event->set_event_type(swdv::scheduler_sync_envelope::FULL_SYNC);
    event->set_sequence_number(1);
    event->set_timestamp_ms(1234567890000);

    std::string serialized;
    ASSERT_TRUE(message.SerializeToString(&serialized));
    EXPECT_GT(serialized.size(), 0);

    swdv::scheduler_sync_envelope::sync_message_t parsed;
    ASSERT_TRUE(parsed.ParseFromString(serialized));
    EXPECT_EQ(parsed.vehicle_id(), "vehicle-001");
    EXPECT_EQ(parsed.bridge_instance_id(), "ssb_1234567890abcdef");
    EXPECT_EQ(parsed.state_checksum(), 0xDEADBEEF);
    EXPECT_EQ(parsed.active_jobs_count(), 5);
    EXPECT_EQ(parsed.events_size(), 1);
    EXPECT_EQ(parsed.events(0).event_type(), swdv::scheduler_sync_envelope::FULL_SYNC);
}

TEST_F(SchedulerSyncBridgeTest, EventTypeValues) {
    EXPECT_EQ(swdv::scheduler_sync_envelope::FULL_SYNC, 0);
    EXPECT_EQ(swdv::scheduler_sync_envelope::JOB_CREATED, 1);
    EXPECT_EQ(swdv::scheduler_sync_envelope::JOB_UPDATED, 2);
    EXPECT_EQ(swdv::scheduler_sync_envelope::JOB_DELETED, 3);
    EXPECT_EQ(swdv::scheduler_sync_envelope::JOB_EXECUTED, 4);
    EXPECT_EQ(swdv::scheduler_sync_envelope::HEARTBEAT, 5);
}

TEST_F(SchedulerSyncBridgeTest, JobStatusValues) {
    EXPECT_EQ(swdv::scheduler_sync_envelope::PENDING, 0);
    EXPECT_EQ(swdv::scheduler_sync_envelope::RUNNING, 1);
    EXPECT_EQ(swdv::scheduler_sync_envelope::COMPLETED, 2);
    EXPECT_EQ(swdv::scheduler_sync_envelope::FAILED, 3);
    EXPECT_EQ(swdv::scheduler_sync_envelope::CANCELLED, 4);
}

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
    state1.status = swdv::scheduler_sync_envelope::PENDING;
    state1.created_at_ms = 1000;
    state1.updated_at_ms = 1000;

    SyncedJobState state2 = state1;

    // Same state should have same hash
    EXPECT_EQ(state1.ComputeHash(), state2.ComputeHash());

    // Different status should have different hash
    state2.status = swdv::scheduler_sync_envelope::RUNNING;
    EXPECT_NE(state1.ComputeHash(), state2.ComputeHash());

    // Different updated_at should have different hash
    state2 = state1;
    state2.updated_at_ms = 2000;
    EXPECT_NE(state1.ComputeHash(), state2.ComputeHash());

    // Different title should have different hash
    state2 = state1;
    state2.title = "Other Job";
    EXPECT_NE(state1.ComputeHash(), state2.ComputeHash());
}

TEST_F(SchedulerSyncBridgeTest, SyncedJobStateIsTerminal) {
    SyncedJobState state;

    state.status = swdv::scheduler_sync_envelope::PENDING;
    EXPECT_FALSE(state.IsTerminal());

    state.status = swdv::scheduler_sync_envelope::RUNNING;
    EXPECT_FALSE(state.IsTerminal());

    state.status = swdv::scheduler_sync_envelope::COMPLETED;
    EXPECT_TRUE(state.IsTerminal());

    state.status = swdv::scheduler_sync_envelope::FAILED;
    EXPECT_TRUE(state.IsTerminal());

    state.status = swdv::scheduler_sync_envelope::CANCELLED;
    EXPECT_TRUE(state.IsTerminal());
}

TEST_F(SchedulerSyncBridgeTest, ExecutionResultSerialization) {
    swdv::scheduler_sync_envelope::execution_result_t result;
    result.set_job_id("job_001");
    result.set_status(swdv::scheduler_sync_envelope::COMPLETED);
    result.set_executed_at_ms(1234567890000);
    result.set_duration_ms(500);
    result.set_result("{\"success\": true}");
    result.set_next_run_time("2025-01-02T10:00:00Z");

    std::string serialized;
    ASSERT_TRUE(result.SerializeToString(&serialized));

    swdv::scheduler_sync_envelope::execution_result_t parsed;
    ASSERT_TRUE(parsed.ParseFromString(serialized));
    EXPECT_EQ(parsed.job_id(), "job_001");
    EXPECT_EQ(parsed.status(), swdv::scheduler_sync_envelope::COMPLETED);
    EXPECT_EQ(parsed.executed_at_ms(), 1234567890000);
    EXPECT_EQ(parsed.duration_ms(), 500);
    EXPECT_EQ(parsed.result(), "{\"success\": true}");
    EXPECT_EQ(parsed.next_run_time(), "2025-01-02T10:00:00Z");
}

TEST_F(SchedulerSyncBridgeTest, SyncAckSerialization) {
    swdv::scheduler_sync_envelope::sync_ack_t ack;
    ack.set_last_sequence_received(100);
    ack.set_checksum_match(true);
    ack.set_request_full_sync(false);

    std::string serialized;
    ASSERT_TRUE(ack.SerializeToString(&serialized));

    swdv::scheduler_sync_envelope::sync_ack_t parsed;
    ASSERT_TRUE(parsed.ParseFromString(serialized));
    EXPECT_EQ(parsed.last_sequence_received(), 100);
    EXPECT_TRUE(parsed.checksum_match());
    EXPECT_FALSE(parsed.request_full_sync());
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
