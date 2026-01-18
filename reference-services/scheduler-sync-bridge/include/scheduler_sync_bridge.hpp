/**
 * @file scheduler_sync_bridge.hpp
 * @brief Bidirectional bridge for Scheduler state sync (pure state sync model)
 *
 * The SchedulerSyncBridge provides bidirectional state synchronization between
 * the vehicle Scheduler service and the cloud:
 *
 * Vehicle-to-Cloud (v2c):
 * - Monitors Scheduler for job state changes
 * - Publishes V2C_SyncMessage with job state and execution records
 * - Sends heartbeats for liveness detection
 *
 * Cloud-to-Vehicle (c2v):
 * - Receives C2V_SyncMessage with cloud job state
 * - Merges cloud jobs into local Scheduler
 * - Handles TriggerJobRequest for immediate job execution
 *
 * Design principles:
 * - Pure state sync: No imperative commands except TriggerJob
 * - Bidirectional: Cloud and vehicle each maintain state, sync replicates
 * - Version vectors: Conflict detection without wall-clock dependency
 * - Authority-based resolution: Cloud or vehicle wins based on job origin
 * - Append-only executions: Execution records are facts, never conflict
 */

#pragma once

#include "backend_transport_client.hpp"
#include "scheduler-sync-v2.pb.h"
#include "ifex-scheduler-service.grpc.pb.h"
#include "version_vector.hpp"
#include "sync_engine.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace grpc {
class Channel;
}

namespace ifex::reference {

/**
 * @brief Configuration for SchedulerSyncBridge
 */
struct SchedulerSyncBridgeConfig {
    /// Scheduler service endpoint
    std::string scheduler_endpoint = "localhost:50053";

    /// Backend Transport endpoint for publishing
    std::string backend_transport_endpoint = "localhost:50060";

    /// Content ID for sync messages (default: SCHEDULER_SYNC = 202)
    uint32_t sync_content_id = 202;

    /// Vehicle identifier for messages
    std::string vehicle_id = "vehicle-001";

    /// Initialization delay before starting sync (ms)
    /// Allows scheduler to load jobs before we capture initial state
    uint32_t initialization_delay_ms = 5000;

    /// Polling interval for Scheduler changes (ms)
    uint32_t poll_interval_ms = 1000;

    /// How long to batch events before sending (ms)
    /// 0 = send immediately
    uint32_t batch_window_ms = 100;

    /// Heartbeat interval when no changes (ms)
    /// 0 = no heartbeat
    uint32_t heartbeat_interval_ms = 30000;

    /// Path to persist sync state (empty = no persistence)
    std::string state_persistence_path;

    // =========================================================================
    // Cloud Sync Settings (c2v)
    // =========================================================================

    /// Enable receiving and processing cloud sync messages
    bool enable_cloud_sync = true;

    /// Timeout for executing scheduler operations (ms)
    uint32_t operation_timeout_ms = 5000;
};

/**
 * @brief Cached state of a synced job (v2 protocol with version vectors)
 */
struct SyncedJobState {
    std::string job_id;
    std::string title;
    std::string service;
    std::string method;
    std::string parameters;
    std::string scheduled_time;
    std::string recurrence_rule;
    std::string next_run_time;

    // Job status and power management (v2 types)
    swdv::scheduler_sync_v2::JobStatus status = swdv::scheduler_sync_v2::JOB_STATUS_PENDING;
    swdv::scheduler_sync_v2::WakePolicy wake_policy = swdv::scheduler_sync_v2::WAKE_NO_WAKE;
    swdv::scheduler_sync_v2::SleepPolicy sleep_policy = swdv::scheduler_sync_v2::SLEEP_NORMAL;
    uint32_t wake_lead_time_s = 0;
    uint64_t created_at_ms = 0;
    uint64_t updated_at_ms = 0;
    uint64_t last_synced_sequence = 0;
    bool paused = false;  // User intent: "don't schedule this job"

    // Sync Protocol v2 fields
    sync::VersionVector version;
    swdv::scheduler_sync_v2::JobAuthority authority =
        swdv::scheduler_sync_v2::AUTHORITY_VEHICLE;
    bool deleted = false;
    uint64_t deleted_at_ms = 0;
    uint64_t scheduled_time_ms = 0;
    uint64_t end_time_ms = 0;
    bool needs_sync = false;  // Local tracking: needs to be sent to remote

    /// Compute hash for change detection
    uint64_t ComputeHash() const;

    /// Check if job is in terminal state
    bool IsTerminal() const {
        return status == swdv::scheduler_sync_v2::JOB_STATUS_COMPLETED ||
               status == swdv::scheduler_sync_v2::JOB_STATUS_FAILED ||
               status == swdv::scheduler_sync_v2::JOB_STATUS_CANCELLED;
    }

    /// Convert to v2 JobRecord protobuf
    void ToJobRecord(swdv::scheduler_sync_v2::JobRecord* record) const;

    /// Create from v2 JobRecord protobuf
    static SyncedJobState FromJobRecord(const swdv::scheduler_sync_v2::JobRecord& record);
};

/**
 * @brief Statistics for monitoring sync bridge health
 */
struct SchedulerSyncStats {
    // v2c sync stats
    uint64_t events_sent = 0;
    uint64_t full_syncs_sent = 0;
    uint64_t delta_syncs_sent = 0;
    uint64_t execution_results_sent = 0;
    uint64_t heartbeats_sent = 0;
    uint64_t bytes_sent = 0;
    uint64_t active_jobs_tracked = 0;
    uint64_t last_sync_timestamp_ms = 0;
    uint64_t current_sequence = 0;
    bool is_initialized = false;
    bool is_connected = false;

    // c2v sync stats
    uint64_t syncs_received = 0;
    uint64_t jobs_created_from_cloud = 0;
    uint64_t jobs_updated_from_cloud = 0;
    uint64_t jobs_deleted_from_cloud = 0;
    uint64_t jobs_rejected = 0;  // Jobs from cloud that couldn't be created
    uint64_t trigger_requests_received = 0;
    uint64_t trigger_requests_succeeded = 0;
    uint64_t trigger_requests_failed = 0;
    uint64_t sync_acks_sent = 0;
};

/**
 * @brief Bidirectional bridge for Scheduler state sync (pure state sync model)
 *
 * v2c Lifecycle (vehicle → cloud):
 * 1. Start() - begins initialization phase
 * 2. After initialization_delay_ms, captures initial state
 * 3. Publishes V2C_SyncMessage with all active jobs
 * 4. Polls Scheduler at poll_interval_ms
 * 5. Publishes delta V2C_SyncMessage for job changes
 * 6. Includes ExecutionRecords for completed/failed jobs
 *
 * c2v Lifecycle (cloud → vehicle):
 * 1. Subscribes to Backend Transport on_content callback
 * 2. Decodes C2V_SyncMessage with cloud job state
 * 3. Merges cloud jobs into local Scheduler via gRPC
 * 4. Handles TriggerJobRequest for immediate execution
 * 5. Sends SyncAck to confirm receipt
 *
 * Thread model:
 * - Poll thread: queries Scheduler for state changes (v2c)
 * - Batch thread: batches and sends sync events (v2c)
 */
class SchedulerSyncBridge {
public:
    explicit SchedulerSyncBridge(const SchedulerSyncBridgeConfig& config);
    ~SchedulerSyncBridge();

    // Non-copyable, non-movable
    SchedulerSyncBridge(const SchedulerSyncBridge&) = delete;
    SchedulerSyncBridge& operator=(const SchedulerSyncBridge&) = delete;

    /**
     * @brief Start the sync bridge
     * @return true if started successfully
     */
    bool Start();

    /**
     * @brief Stop the sync bridge gracefully
     */
    void Stop();

    /**
     * @brief Check if bridge is running
     */
    bool IsRunning() const { return running_.load(); }

    /**
     * @brief Check if initialization phase is complete
     */
    bool IsInitialized() const { return initialized_.load(); }

    /**
     * @brief Check if connected to Backend Transport
     */
    bool IsConnected() const;

    /**
     * @brief Get current statistics
     */
    SchedulerSyncStats GetStats() const;

    /**
     * @brief Force a full sync (for testing or recovery)
     */
    void ForceFullSync();

    /**
     * @brief Get current state checksum
     */
    uint32_t GetStateChecksum() const;

private:
    /// Configuration
    SchedulerSyncBridgeConfig config_;

    /// Backend Transport client for publishing
    std::unique_ptr<client::BackendTransportClient> transport_client_;

    /// gRPC channel to Scheduler service
    std::shared_ptr<grpc::Channel> scheduler_channel_;

    /// Scheduler service stubs
    std::unique_ptr<swdv::ifex_scheduler::get_jobs_service::Stub> get_jobs_stub_;
    std::unique_ptr<swdv::ifex_scheduler::create_job_service::Stub> create_job_stub_;
    std::unique_ptr<swdv::ifex_scheduler::update_job_service::Stub> update_job_stub_;
    std::unique_ptr<swdv::ifex_scheduler::delete_job_service::Stub> delete_job_stub_;
    std::unique_ptr<swdv::ifex_scheduler::pause_job_service::Stub> pause_job_stub_;
    std::unique_ptr<swdv::ifex_scheduler::resume_job_service::Stub> resume_job_stub_;
    std::unique_ptr<swdv::ifex_scheduler::trigger_job_service::Stub> trigger_job_stub_;

    /// Cached sync state for ACTIVE jobs only (job_id -> state)
    /// Terminal jobs are removed after syncing their execution result
    std::unordered_map<std::string, SyncedJobState> synced_state_;
    mutable std::mutex state_mutex_;

    /// Mutex for events
    mutable std::mutex events_mutex_;

    /// Statistics
    mutable SchedulerSyncStats stats_;
    mutable std::mutex stats_mutex_;

    /// Monotonic sequence number
    std::atomic<uint64_t> sequence_number_{0};

    /// Unique instance ID (for restart detection)
    std::string instance_id_;

    /// Running state
    std::atomic<bool> running_{false};
    std::atomic<bool> initialized_{false};
    std::atomic<bool> stop_requested_{false};

    /// Worker threads (v2c sync)
    std::thread poll_thread_;
    std::thread batch_thread_;

    /// Condition variable for signaling
    std::condition_variable cv_;
    std::mutex cv_mutex_;

    /// Last heartbeat time
    std::chrono::steady_clock::time_point last_activity_time_;

    // Internal methods

    /// Main polling loop
    void PollLoop();

    /// Batch sending loop
    void BatchLoop();

    /// Query Scheduler for current jobs
    std::vector<SyncedJobState> QuerySchedulerJobs();

    /// Compare current state with synced state, detect changes
    void DetectChanges(const std::vector<SyncedJobState>& current);

    /// Send heartbeat if no recent activity
    void MaybeSendHeartbeat();

    /// Compute CRC32 checksum of current active job state
    uint32_t ComputeStateChecksum() const;

    /// Generate unique instance ID
    static std::string GenerateInstanceId();

    /// Load persisted state (if configured)
    void LoadPersistedState();

    /// Save state to disk (if configured)
    void PersistState();

    /// Update statistics
    void UpdateStats(uint64_t bytes_sent, bool is_full_sync, bool is_execution_result);

    /// Map scheduler status to v2 status
    static swdv::scheduler_sync_v2::JobStatus MapStatus(
        swdv::ifex_scheduler::job_status_t status);

    /// Map scheduler wake policy to v2 wake policy
    static swdv::scheduler_sync_v2::WakePolicy MapWakePolicy(
        swdv::ifex_scheduler::wake_policy_t policy);

    /// Map scheduler sleep policy to v2 sleep policy
    static swdv::scheduler_sync_v2::SleepPolicy MapSleepPolicy(
        swdv::ifex_scheduler::sleep_policy_t policy);

    // =========================================================================
    // Cloud Sync Handling (c2v)
    // =========================================================================

    /// Handle incoming cloud message (called from on_content callback)
    void HandleCloudMessage(const std::vector<uint8_t>& payload);

    /// Operation result for internal use
    struct OperationResult {
        bool success = false;
        std::string job_id;
        std::string error_message;
    };

    /// Create a job in the local Scheduler from cloud sync
    OperationResult CreateJobFromCloud(const swdv::scheduler_sync_v2::JobRecord& job);

    /// Update a job in the local Scheduler from cloud sync
    OperationResult UpdateJobFromCloud(const swdv::scheduler_sync_v2::JobRecord& job);

    /// Delete a job from the local Scheduler
    OperationResult DeleteJobFromScheduler(const std::string& job_id);

    /// Handle TriggerJobRequest (the only imperative command)
    void HandleTriggerJobRequest(const swdv::scheduler_sync_v2::TriggerJobRequest& request);

    /// Send TriggerJobResponse to cloud
    void SendTriggerJobResponse(const std::string& job_id, bool accepted,
                                const std::string& error_message);

    // =========================================================================
    // Sync Protocol v2 Methods
    // =========================================================================

    /// Pending execution records to send
    std::vector<swdv::scheduler_sync_v2::ExecutionRecord> pending_executions_;

    /// Send v2 sync message (V2C_SyncMessage)
    void SendV2SyncMessage(const std::vector<SyncedJobState>& jobs,
                           bool include_all_jobs = false);

    /// Send a deletion sync message (V2C_SyncMessage with deleted_job_ids)
    void SendDeleteSyncMessage(const std::string& job_id);

    /// Handle incoming v2 sync message from cloud (C2V_SyncMessage)
    void HandleV2SyncMessage(const swdv::scheduler_sync_v2::C2V_SyncMessage& msg);

    /// Process a single job from cloud using sync engine
    void ProcessCloudJob(const swdv::scheduler_sync_v2::JobRecord& remote_job);

    /// Compute state checksum for quiescence detection (xxHash64)
    uint64_t ComputeStateChecksumXxHash() const;

    /// Record an execution (for sending to cloud)
    void RecordExecution(const std::string& job_id,
                        uint64_t executed_at_ms,
                        uint64_t duration_ms,
                        swdv::scheduler_sync_v2::JobStatus status,
                        const std::string& result_json,
                        const std::string& error_message);

    /// Generate unique sync ID
    static std::string GenerateSyncId();

    /// Generate unique execution ID
    static std::string GenerateExecutionId();

    /// Apply a cloud job to local state
    void ApplyCloudJob(const swdv::scheduler_sync_v2::JobRecord& remote_job);
};

}  // namespace ifex::reference
