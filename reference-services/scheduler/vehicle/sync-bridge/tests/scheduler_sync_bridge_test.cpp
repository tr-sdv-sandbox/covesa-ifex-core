/**
 * @file scheduler_sync_bridge_test.cpp
 * @brief Unit tests for SchedulerSyncBridge (v3.1 protocol)
 */

#include <gtest/gtest.h>
#include <glog/logging.h>

#include "scheduler_sync_bridge.hpp"
#include "ifex_content_ids.hpp"
#include "scheduler-sync-v3.pb.h"

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
    EXPECT_TRUE(config.state_persistence_path.empty());
}

TEST_F(SchedulerSyncBridgeTest, ContentIdConstants) {
    EXPECT_EQ(ifex::content_id::DISCOVERY_SYNC, 201);
    EXPECT_EQ(ifex::content_id::SCHEDULER_SYNC, 202);
}

// =============================================================================
// V3.2 Protocol Message Tests
// =============================================================================

TEST_F(SchedulerSyncBridgeTest, V3SyncMessageSerialization) {
    swdv::scheduler_sync_v3::SyncMessage sync_msg;
    sync_msg.set_vehicle_id("vehicle-001");
    sync_msg.set_state_checksum(0xABCD1234);

    // Add a job
    auto* job = sync_msg.add_jobs();
    job->set_job_id("job_001");
    job->set_title("Test Job");
    job->set_service("test-service");
    job->set_method("test_method");
    job->mutable_version()->set_cloud_seq(1);
    job->mutable_version()->set_vehicle_seq(2);

    // Add an ACK
    auto* ack = sync_msg.add_acked_jobs();
    ack->set_job_id("job_002");
    ack->set_cloud_seq(3);
    ack->set_vehicle_seq(4);

    // Wrap in V2C envelope
    swdv::scheduler_sync_v3::V2C_Envelope envelope;
    *envelope.mutable_sync() = sync_msg;

    std::string serialized;
    ASSERT_TRUE(envelope.SerializeToString(&serialized));

    swdv::scheduler_sync_v3::V2C_Envelope parsed;
    ASSERT_TRUE(parsed.ParseFromString(serialized));
    EXPECT_EQ(parsed.message_case(), swdv::scheduler_sync_v3::V2C_Envelope::kSync);
    EXPECT_EQ(parsed.sync().vehicle_id(), "vehicle-001");
    EXPECT_EQ(parsed.sync().state_checksum(), 0xABCD1234);
    EXPECT_EQ(parsed.sync().jobs_size(), 1);
    EXPECT_EQ(parsed.sync().jobs(0).job_id(), "job_001");
    EXPECT_EQ(parsed.sync().acked_jobs_size(), 1);
    EXPECT_EQ(parsed.sync().acked_jobs(0).job_id(), "job_002");
    EXPECT_EQ(parsed.sync().acked_jobs(0).cloud_seq(), 3);
    EXPECT_EQ(parsed.sync().acked_jobs(0).vehicle_seq(), 4);
}

TEST_F(SchedulerSyncBridgeTest, V3GapDetectSerialization) {
    swdv::scheduler_sync_v3::GapDetect gap_detect;
    gap_detect.set_vehicle_id("vehicle-001");
    gap_detect.add_job_ids("job_001");
    gap_detect.add_job_ids("job_002");
    gap_detect.add_job_ids("job_003");
    gap_detect.add_request_job_ids("job_004");

    // Wrap in V2C envelope
    swdv::scheduler_sync_v3::V2C_Envelope envelope;
    *envelope.mutable_gap_detect() = gap_detect;

    std::string serialized;
    ASSERT_TRUE(envelope.SerializeToString(&serialized));

    swdv::scheduler_sync_v3::V2C_Envelope parsed;
    ASSERT_TRUE(parsed.ParseFromString(serialized));
    EXPECT_EQ(parsed.message_case(), swdv::scheduler_sync_v3::V2C_Envelope::kGapDetect);
    EXPECT_EQ(parsed.gap_detect().vehicle_id(), "vehicle-001");
    EXPECT_EQ(parsed.gap_detect().job_ids_size(), 3);
    EXPECT_EQ(parsed.gap_detect().job_ids(0), "job_001");
    EXPECT_EQ(parsed.gap_detect().request_job_ids_size(), 1);
    EXPECT_EQ(parsed.gap_detect().request_job_ids(0), "job_004");
}

TEST_F(SchedulerSyncBridgeTest, V3JobVersionAckSerialization) {
    swdv::scheduler_sync_v3::JobVersionAck ack;
    ack.set_job_id("job_001");
    ack.set_cloud_seq(5);
    ack.set_vehicle_seq(3);

    std::string serialized;
    ASSERT_TRUE(ack.SerializeToString(&serialized));

    swdv::scheduler_sync_v3::JobVersionAck parsed;
    ASSERT_TRUE(parsed.ParseFromString(serialized));
    EXPECT_EQ(parsed.job_id(), "job_001");
    EXPECT_EQ(parsed.cloud_seq(), 5);
    EXPECT_EQ(parsed.vehicle_seq(), 3);
}

TEST_F(SchedulerSyncBridgeTest, V3C2VSyncMessageSerialization) {
    swdv::scheduler_sync_v3::SyncMessage sync_msg;
    sync_msg.set_vehicle_id("vehicle-001");
    sync_msg.set_state_checksum(0xDEADBEEF);

    // Add a job from cloud
    auto* job = sync_msg.add_jobs();
    job->set_job_id("cloud_job_001");
    job->set_title("Cloud Scheduled Task");
    job->set_authority(swdv::scheduler_sync_v3::AUTHORITY_CLOUD);
    job->mutable_version()->set_cloud_seq(10);
    job->mutable_version()->set_vehicle_seq(0);

    // Add ACKs
    auto* ack = sync_msg.add_acked_jobs();
    ack->set_job_id("job_001");
    ack->set_cloud_seq(1);
    ack->set_vehicle_seq(2);

    // Wrap in C2V envelope
    swdv::scheduler_sync_v3::C2V_Envelope envelope;
    *envelope.mutable_sync() = sync_msg;

    std::string serialized;
    ASSERT_TRUE(envelope.SerializeToString(&serialized));

    swdv::scheduler_sync_v3::C2V_Envelope parsed;
    ASSERT_TRUE(parsed.ParseFromString(serialized));
    EXPECT_EQ(parsed.message_case(), swdv::scheduler_sync_v3::C2V_Envelope::kSync);
    EXPECT_EQ(parsed.sync().vehicle_id(), "vehicle-001");
    EXPECT_EQ(parsed.sync().state_checksum(), 0xDEADBEEF);
    EXPECT_EQ(parsed.sync().jobs_size(), 1);
    EXPECT_EQ(parsed.sync().jobs(0).authority(), swdv::scheduler_sync_v3::AUTHORITY_CLOUD);
    EXPECT_EQ(parsed.sync().acked_jobs_size(), 1);
}

TEST_F(SchedulerSyncBridgeTest, V3C2VGapDetectSerialization) {
    swdv::scheduler_sync_v3::GapDetect gap_detect;
    gap_detect.set_vehicle_id("vehicle-001");
    gap_detect.add_job_ids("cloud_job_001");
    gap_detect.add_job_ids("cloud_job_002");
    gap_detect.add_request_job_ids("veh_job_001");

    // Wrap in C2V envelope
    swdv::scheduler_sync_v3::C2V_Envelope envelope;
    *envelope.mutable_gap_detect() = gap_detect;

    std::string serialized;
    ASSERT_TRUE(envelope.SerializeToString(&serialized));

    swdv::scheduler_sync_v3::C2V_Envelope parsed;
    ASSERT_TRUE(parsed.ParseFromString(serialized));
    EXPECT_EQ(parsed.message_case(), swdv::scheduler_sync_v3::C2V_Envelope::kGapDetect);
    EXPECT_EQ(parsed.gap_detect().job_ids_size(), 2);
    EXPECT_EQ(parsed.gap_detect().request_job_ids_size(), 1);
    EXPECT_EQ(parsed.gap_detect().request_job_ids(0), "veh_job_001");
}

// =============================================================================
// V3.1 Protocol Message Tests (deprecated - kept for compatibility)
// =============================================================================

TEST_F(SchedulerSyncBridgeTest, V3JobRecordSerialization) {
    swdv::scheduler_sync_v3::JobRecord record;
    record.set_job_id("job_001");
    record.set_title("Test Job");
    record.set_service("test-service");
    record.set_method("test_method");
    record.set_parameters_json("{}");
    record.set_scheduled_time_ms(1234567890000);
    record.set_status(swdv::scheduler_sync_v3::JOB_STATUS_PENDING);
    record.set_created_at_ms(1234567890000);
    record.set_updated_at_ms(1234567890000);
    record.set_authority(swdv::scheduler_sync_v3::AUTHORITY_VEHICLE);
    record.set_paused(false);

    // Set version vector
    auto* version = record.mutable_version();
    version->set_cloud_seq(1);
    version->set_vehicle_seq(2);

    std::string serialized;
    ASSERT_TRUE(record.SerializeToString(&serialized));
    EXPECT_GT(serialized.size(), 0);

    swdv::scheduler_sync_v3::JobRecord parsed;
    ASSERT_TRUE(parsed.ParseFromString(serialized));
    EXPECT_EQ(parsed.job_id(), "job_001");
    EXPECT_EQ(parsed.title(), "Test Job");
    EXPECT_EQ(parsed.service(), "test-service");
    EXPECT_EQ(parsed.status(), swdv::scheduler_sync_v3::JOB_STATUS_PENDING);
    EXPECT_EQ(parsed.version().cloud_seq(), 1);
    EXPECT_EQ(parsed.version().vehicle_seq(), 2);
    EXPECT_FALSE(parsed.paused());
}

TEST_F(SchedulerSyncBridgeTest, V3HelloMessageSerialization) {
    swdv::scheduler_sync_v3::V2C_Hello hello;
    hello.set_vehicle_id("vehicle-001");
    hello.set_bridge_instance_id("ssb_1234567890abcdef");
    hello.set_state_checksum(12345678);
    hello.set_last_seen_c2v_checksum(87654321);

    // Wrap in envelope
    swdv::scheduler_sync_v3::V2C_Envelope envelope;
    *envelope.mutable_hello() = hello;

    std::string serialized;
    ASSERT_TRUE(envelope.SerializeToString(&serialized));
    EXPECT_GT(serialized.size(), 0);

    swdv::scheduler_sync_v3::V2C_Envelope parsed;
    ASSERT_TRUE(parsed.ParseFromString(serialized));
    EXPECT_EQ(parsed.message_case(), swdv::scheduler_sync_v3::V2C_Envelope::kHello);
    EXPECT_EQ(parsed.hello().vehicle_id(), "vehicle-001");
    EXPECT_EQ(parsed.hello().bridge_instance_id(), "ssb_1234567890abcdef");
    EXPECT_EQ(parsed.hello().state_checksum(), 12345678);
    EXPECT_EQ(parsed.hello().last_seen_c2v_checksum(), 87654321);
}

// V3HashManifestSerialization test removed - hash manifests removed in v3.2

TEST_F(SchedulerSyncBridgeTest, V3JobDataSerialization) {
    swdv::scheduler_sync_v3::V2C_JobData job_data;
    job_data.set_vehicle_id("vehicle-001");
    job_data.set_state_checksum(12345678);

    auto* job = job_data.add_jobs();
    job->set_job_id("job_001");
    job->set_title("Test Job");
    job->set_service("test-service");
    job->set_method("test_method");
    job->set_status(swdv::scheduler_sync_v3::JOB_STATUS_PENDING);

    // Wrap in envelope
    swdv::scheduler_sync_v3::V2C_Envelope envelope;
    *envelope.mutable_job_data() = job_data;

    std::string serialized;
    ASSERT_TRUE(envelope.SerializeToString(&serialized));

    swdv::scheduler_sync_v3::V2C_Envelope parsed;
    ASSERT_TRUE(parsed.ParseFromString(serialized));
    EXPECT_EQ(parsed.message_case(), swdv::scheduler_sync_v3::V2C_Envelope::kJobData);
    EXPECT_EQ(parsed.job_data().jobs_size(), 1);
    EXPECT_EQ(parsed.job_data().jobs(0).job_id(), "job_001");
}

TEST_F(SchedulerSyncBridgeTest, V3ExecutionRecordSerialization) {
    swdv::scheduler_sync_v3::ExecutionRecord exec;
    exec.set_execution_id("exec_001");
    exec.set_job_id("job_001");
    exec.set_executed_at_ms(1234567890000);
    exec.set_duration_ms(500);
    exec.set_status(swdv::scheduler_sync_v3::JOB_STATUS_COMPLETED);
    exec.set_result_json("{\"success\": true}");

    std::string serialized;
    ASSERT_TRUE(exec.SerializeToString(&serialized));

    swdv::scheduler_sync_v3::ExecutionRecord parsed;
    ASSERT_TRUE(parsed.ParseFromString(serialized));
    EXPECT_EQ(parsed.job_id(), "job_001");
    EXPECT_EQ(parsed.status(), swdv::scheduler_sync_v3::JOB_STATUS_COMPLETED);
    EXPECT_EQ(parsed.duration_ms(), 500);
    EXPECT_EQ(parsed.result_json(), "{\"success\": true}");
}

TEST_F(SchedulerSyncBridgeTest, V3ExecutionsEnvelopeSerialization) {
    swdv::scheduler_sync_v3::V2C_Executions executions;
    executions.set_vehicle_id("vehicle-001");

    auto* exec = executions.add_executions();
    exec->set_execution_id("exec_001");
    exec->set_job_id("job_001");
    exec->set_executed_at_ms(1234567890000);
    exec->set_status(swdv::scheduler_sync_v3::JOB_STATUS_COMPLETED);

    // Wrap in envelope
    swdv::scheduler_sync_v3::V2C_Envelope envelope;
    *envelope.mutable_executions() = executions;

    std::string serialized;
    ASSERT_TRUE(envelope.SerializeToString(&serialized));

    swdv::scheduler_sync_v3::V2C_Envelope parsed;
    ASSERT_TRUE(parsed.ParseFromString(serialized));
    EXPECT_EQ(parsed.message_case(), swdv::scheduler_sync_v3::V2C_Envelope::kExecutions);
    EXPECT_EQ(parsed.executions().executions_size(), 1);
    EXPECT_EQ(parsed.executions().executions(0).execution_id(), "exec_001");
}

TEST_F(SchedulerSyncBridgeTest, V3JobStatusValues) {
    // Verify the enum values match the spec
    EXPECT_EQ(swdv::scheduler_sync_v3::JOB_STATUS_PENDING, 0);
    EXPECT_EQ(swdv::scheduler_sync_v3::JOB_STATUS_RUNNING, 1);
    EXPECT_EQ(swdv::scheduler_sync_v3::JOB_STATUS_COMPLETED, 2);
    EXPECT_EQ(swdv::scheduler_sync_v3::JOB_STATUS_FAILED, 3);
    EXPECT_EQ(swdv::scheduler_sync_v3::JOB_STATUS_CANCELLED, 4);
}

TEST_F(SchedulerSyncBridgeTest, V3AuthorityValues) {
    // Verify the enum values match the spec
    EXPECT_EQ(swdv::scheduler_sync_v3::AUTHORITY_CLOUD, 0);
    EXPECT_EQ(swdv::scheduler_sync_v3::AUTHORITY_VEHICLE, 1);
}

TEST_F(SchedulerSyncBridgeTest, V3WakeSleepPolicyValues) {
    // Verify wake policy values
    EXPECT_EQ(swdv::scheduler_sync_v3::WAKE_NO_WAKE, 0);
    EXPECT_EQ(swdv::scheduler_sync_v3::WAKE_REQUIRED, 1);

    // Verify sleep policy values
    EXPECT_EQ(swdv::scheduler_sync_v3::SLEEP_NORMAL, 0);
    EXPECT_EQ(swdv::scheduler_sync_v3::SLEEP_INHIBIT, 1);
}

// =============================================================================
// SyncedJobState Tests (uses v3 types)
// =============================================================================

TEST_F(SchedulerSyncBridgeTest, SyncedJobStateHash) {
    SyncedJobState state1;
    state1.job_id = "job_001";
    state1.title = "Test Job";
    state1.service = "test-service";
    state1.method = "test_method";
    state1.parameters = "{}";
    state1.scheduled_time_ms = 1704103200000;  // 2025-01-01T10:00:00Z
    state1.recurrence_rule = "";
    state1.status = swdv::scheduler_sync_v3::JOB_STATUS_PENDING;
    state1.created_at_ms = 1000;
    state1.updated_at_ms = 1000;

    SyncedJobState state2 = state1;

    // Same state should have same hash
    EXPECT_EQ(state1.ComputeHash(), state2.ComputeHash());

    // Per Scheduler Sync Protocol v3.1, status is EXCLUDED from hash
    // (it's execution state, not job content)
    state2.status = swdv::scheduler_sync_v3::JOB_STATUS_RUNNING;
    EXPECT_EQ(state1.ComputeHash(), state2.ComputeHash());

    // Different scheduled_time_ms should have different hash
    state2 = state1;
    state2.scheduled_time_ms = 1706781600000;  // Different timestamp
    EXPECT_NE(state1.ComputeHash(), state2.ComputeHash());

    // Different title should have different hash
    state2 = state1;
    state2.title = "Other Job";
    EXPECT_NE(state1.ComputeHash(), state2.ComputeHash());

    // updated_at_ms is excluded from hash (it's metadata, not content)
    state2 = state1;
    state2.updated_at_ms = 9999;
    EXPECT_EQ(state1.ComputeHash(), state2.ComputeHash());

    // created_at_ms is excluded from hash (it's metadata, not content)
    state2 = state1;
    state2.created_at_ms = 9999;
    EXPECT_EQ(state1.ComputeHash(), state2.ComputeHash());

    // Different paused should have different hash (paused IS a content field)
    state2 = state1;
    state2.paused = true;
    EXPECT_NE(state1.ComputeHash(), state2.ComputeHash());
}

TEST_F(SchedulerSyncBridgeTest, SyncedJobStateIsTerminal) {
    SyncedJobState state;

    state.status = swdv::scheduler_sync_v3::JOB_STATUS_PENDING;
    EXPECT_FALSE(state.IsTerminal());

    state.status = swdv::scheduler_sync_v3::JOB_STATUS_RUNNING;
    EXPECT_FALSE(state.IsTerminal());

    state.status = swdv::scheduler_sync_v3::JOB_STATUS_COMPLETED;
    EXPECT_TRUE(state.IsTerminal());

    state.status = swdv::scheduler_sync_v3::JOB_STATUS_FAILED;
    EXPECT_TRUE(state.IsTerminal());

    state.status = swdv::scheduler_sync_v3::JOB_STATUS_CANCELLED;
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
    state.status = swdv::scheduler_sync_v3::JOB_STATUS_PENDING;
    state.authority = swdv::scheduler_sync_v3::AUTHORITY_VEHICLE;
    state.paused = false;
    state.version = ifex::scheduler::VersionVector{1, 2};

    swdv::scheduler_sync_v3::JobRecord record;
    state.ToJobRecord(&record);

    EXPECT_EQ(record.job_id(), "job_001");
    EXPECT_EQ(record.title(), "Test Job");
    EXPECT_EQ(record.service(), "test-service");
    EXPECT_EQ(record.method(), "test_method");
    EXPECT_EQ(record.parameters_json(), "{\"key\": \"value\"}");
    EXPECT_EQ(record.scheduled_time_ms(), 1234567890000);
    EXPECT_EQ(record.status(), swdv::scheduler_sync_v3::JOB_STATUS_PENDING);
    EXPECT_EQ(record.authority(), swdv::scheduler_sync_v3::AUTHORITY_VEHICLE);
    EXPECT_FALSE(record.paused());
    EXPECT_EQ(record.version().cloud_seq(), 1);
    EXPECT_EQ(record.version().vehicle_seq(), 2);
}

TEST_F(SchedulerSyncBridgeTest, SyncedJobStateFromJobRecord) {
    swdv::scheduler_sync_v3::JobRecord record;
    record.set_job_id("job_002");
    record.set_title("Cloud Job");
    record.set_service("cloud-service");
    record.set_method("cloud_method");
    record.set_parameters_json("{\"cloud\": true}");
    record.set_scheduled_time_ms(9876543210000);
    record.set_status(swdv::scheduler_sync_v3::JOB_STATUS_COMPLETED);
    record.set_authority(swdv::scheduler_sync_v3::AUTHORITY_CLOUD);
    record.set_paused(true);
    record.mutable_version()->set_cloud_seq(5);
    record.mutable_version()->set_vehicle_seq(3);

    SyncedJobState state = SyncedJobState::FromJobRecord(record);

    EXPECT_EQ(state.job_id, "job_002");
    EXPECT_EQ(state.title, "Cloud Job");
    EXPECT_EQ(state.service, "cloud-service");
    EXPECT_EQ(state.method, "cloud_method");
    EXPECT_EQ(state.parameters, "{\"cloud\": true}");
    EXPECT_EQ(state.scheduled_time_ms, 9876543210000);
    EXPECT_EQ(state.status, swdv::scheduler_sync_v3::JOB_STATUS_COMPLETED);
    EXPECT_EQ(state.authority, swdv::scheduler_sync_v3::AUTHORITY_CLOUD);
    EXPECT_TRUE(state.paused);
    EXPECT_EQ(state.version.cloud_seq, 5);
    EXPECT_EQ(state.version.vehicle_seq, 3);
}

// =============================================================================
// V3.2 Cloud-to-Vehicle Message Tests
// =============================================================================

// V3RequestHashesSerialization test removed - C2V_RequestHashes removed in v3.2

TEST_F(SchedulerSyncBridgeTest, V3SyncDeltaSerialization) {
    swdv::scheduler_sync_v3::C2V_SyncDelta delta;
    delta.set_vehicle_id("vehicle-001");
    delta.set_state_checksum(87654321);
    delta.set_last_seen_v2c_checksum(12345678);

    // Request some job IDs from vehicle
    delta.add_request_job_ids("job_001");
    delta.add_request_job_ids("job_002");

    // Send a job from cloud
    auto* job = delta.add_jobs();
    job->set_job_id("cloud_job_001");
    job->set_title("Cloud Scheduled Task");
    job->set_status(swdv::scheduler_sync_v3::JOB_STATUS_PENDING);
    job->set_authority(swdv::scheduler_sync_v3::AUTHORITY_CLOUD);

    // Wrap in envelope
    swdv::scheduler_sync_v3::C2V_Envelope envelope;
    *envelope.mutable_sync_delta() = delta;

    std::string serialized;
    ASSERT_TRUE(envelope.SerializeToString(&serialized));

    swdv::scheduler_sync_v3::C2V_Envelope parsed;
    ASSERT_TRUE(parsed.ParseFromString(serialized));
    EXPECT_EQ(parsed.message_case(), swdv::scheduler_sync_v3::C2V_Envelope::kSyncDelta);
    EXPECT_EQ(parsed.sync_delta().vehicle_id(), "vehicle-001");
    EXPECT_EQ(parsed.sync_delta().state_checksum(), 87654321);
    EXPECT_EQ(parsed.sync_delta().last_seen_v2c_checksum(), 12345678);
    EXPECT_EQ(parsed.sync_delta().request_job_ids_size(), 2);
    EXPECT_EQ(parsed.sync_delta().request_job_ids(0), "job_001");
    EXPECT_EQ(parsed.sync_delta().jobs_size(), 1);
    EXPECT_EQ(parsed.sync_delta().jobs(0).authority(), swdv::scheduler_sync_v3::AUTHORITY_CLOUD);
}

TEST_F(SchedulerSyncBridgeTest, V3ExecutionAckSerialization) {
    swdv::scheduler_sync_v3::C2V_ExecutionAck ack;
    ack.set_vehicle_id("vehicle-001");
    ack.add_execution_ids("exec_001");
    ack.add_execution_ids("exec_002");

    // Wrap in envelope
    swdv::scheduler_sync_v3::C2V_Envelope envelope;
    *envelope.mutable_execution_ack() = ack;

    std::string serialized;
    ASSERT_TRUE(envelope.SerializeToString(&serialized));

    swdv::scheduler_sync_v3::C2V_Envelope parsed;
    ASSERT_TRUE(parsed.ParseFromString(serialized));
    EXPECT_EQ(parsed.message_case(), swdv::scheduler_sync_v3::C2V_Envelope::kExecutionAck);
    EXPECT_EQ(parsed.execution_ack().execution_ids_size(), 2);
    EXPECT_EQ(parsed.execution_ack().execution_ids(0), "exec_001");
    EXPECT_EQ(parsed.execution_ack().execution_ids(1), "exec_002");
}

TEST_F(SchedulerSyncBridgeTest, V3TriggerJobSerialization) {
    swdv::scheduler_sync_v3::C2V_TriggerJob trigger;
    trigger.set_vehicle_id("vehicle-001");
    trigger.set_job_id("job_001");
    trigger.set_request_id("req_001");
    trigger.set_requester_id("user@example.com");
    trigger.set_timestamp_ms(1234567890000);
    trigger.set_expires_at_ms(1234567900000);

    // Wrap in envelope
    swdv::scheduler_sync_v3::C2V_Envelope envelope;
    *envelope.mutable_trigger_job() = trigger;

    std::string serialized;
    ASSERT_TRUE(envelope.SerializeToString(&serialized));

    swdv::scheduler_sync_v3::C2V_Envelope parsed;
    ASSERT_TRUE(parsed.ParseFromString(serialized));
    EXPECT_EQ(parsed.message_case(), swdv::scheduler_sync_v3::C2V_Envelope::kTriggerJob);
    EXPECT_EQ(parsed.trigger_job().job_id(), "job_001");
    EXPECT_EQ(parsed.trigger_job().request_id(), "req_001");
    EXPECT_EQ(parsed.trigger_job().requester_id(), "user@example.com");
    EXPECT_EQ(parsed.trigger_job().timestamp_ms(), 1234567890000);
    EXPECT_EQ(parsed.trigger_job().expires_at_ms(), 1234567900000);
}

TEST_F(SchedulerSyncBridgeTest, V3TriggerResponseSerialization) {
    swdv::scheduler_sync_v3::V2C_TriggerResponse response;
    response.set_vehicle_id("vehicle-001");
    response.set_job_id("job_001");
    response.set_request_id("req_001");
    response.set_accepted(true);
    response.set_timestamp_ms(1234567890000);

    // Wrap in envelope
    swdv::scheduler_sync_v3::V2C_Envelope envelope;
    *envelope.mutable_trigger_response() = response;

    std::string serialized;
    ASSERT_TRUE(envelope.SerializeToString(&serialized));

    swdv::scheduler_sync_v3::V2C_Envelope parsed;
    ASSERT_TRUE(parsed.ParseFromString(serialized));
    EXPECT_EQ(parsed.message_case(), swdv::scheduler_sync_v3::V2C_Envelope::kTriggerResponse);
    EXPECT_EQ(parsed.trigger_response().job_id(), "job_001");
    EXPECT_EQ(parsed.trigger_response().request_id(), "req_001");
    EXPECT_TRUE(parsed.trigger_response().accepted());
    EXPECT_TRUE(parsed.trigger_response().error_message().empty());
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
    EXPECT_EQ(stats.hellos_sent, 0);
    // hash_manifests_sent removed in v3.2
    EXPECT_EQ(stats.job_data_sent, 0);
    EXPECT_EQ(stats.executions_sent, 0);
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
