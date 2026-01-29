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
#include "scheduler-sync-v3.pb.h"
#include "scheduler-service.grpc.pb.h"
#include "scheduler-types.pb.h"

// ifex-scheduler library (canonical job structure, hash, version vectors)
#include "version_vector.hpp"
#include "sync_engine.hpp"
#include "job.hpp"
#include "job_hash.hpp"

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
 * @brief Cached state of a synced job (v3.1 protocol with version vectors)
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

    // Job status and power management (v3 types)
    swdv::scheduler_sync_v3::JobStatus status = swdv::scheduler_sync_v3::JOB_STATUS_PENDING;
    swdv::scheduler_sync_v3::WakePolicy wake_policy = swdv::scheduler_sync_v3::WAKE_NO_WAKE;
    swdv::scheduler_sync_v3::SleepPolicy sleep_policy = swdv::scheduler_sync_v3::SLEEP_NORMAL;
    uint32_t wake_lead_time_s = 0;
    uint64_t created_at_ms = 0;
    uint64_t updated_at_ms = 0;
    uint64_t last_synced_sequence = 0;
    bool paused = false;  // User intent: "don't schedule this job"

    // Sync Protocol v3 fields
    ifex::scheduler::VersionVector version;
    ifex::scheduler::VersionVector synced_version;  // Last version confirmed by cloud
    swdv::scheduler_sync_v3::JobAuthority authority =
        swdv::scheduler_sync_v3::AUTHORITY_VEHICLE;
    bool deleted = false;
    uint64_t scheduled_time_ms = 0;
    uint64_t end_time_ms = 0;
    // Dirty flag: true if local version differs from last confirmed remote version
    bool is_dirty() const { return version != synced_version; }

    /// Compute hash for change detection
    uint64_t ComputeHash() const;

    /// Check if job is in terminal state
    bool IsTerminal() const {
        return status == swdv::scheduler_sync_v3::JOB_STATUS_COMPLETED ||
               status == swdv::scheduler_sync_v3::JOB_STATUS_FAILED ||
               status == swdv::scheduler_sync_v3::JOB_STATUS_CANCELLED;
    }

    /// Convert to v3 JobRecord protobuf
    void ToJobRecord(swdv::scheduler_sync_v3::JobRecord* record) const;

    /// Create from v3 JobRecord protobuf
    static SyncedJobState FromJobRecord(const swdv::scheduler_sync_v3::JobRecord& record);
};

/**
 * @brief Statistics for monitoring sync bridge health
 */
struct SchedulerSyncStats {
    // v2c sync stats (v3.2 protocol)
    uint64_t events_sent = 0;
    uint64_t hellos_sent = 0;            // V2C_Hello messages (v3.1 deprecated)
    uint64_t job_data_sent = 0;          // V2C_JobData messages (v3.1 deprecated)
    uint64_t sync_messages_sent = 0;     // SyncMessage (v3.2)
    uint64_t gap_detects_sent = 0;       // GapDetect (v3.2)
    uint64_t executions_sent = 0;        // V2C_Executions messages
    uint64_t trigger_responses_sent = 0; // V2C_TriggerResponse messages
    uint64_t heartbeats_sent = 0;
    uint64_t bytes_sent = 0;
    uint64_t active_jobs_tracked = 0;
    uint64_t last_sync_timestamp_ms = 0;
    uint64_t current_sequence = 0;
    bool is_initialized = false;
    bool is_connected = false;

    // c2v sync stats (v3.2 protocol)
    uint64_t sync_deltas_received = 0;     // C2V_SyncDelta (v3.1 deprecated)
    uint64_t sync_messages_received = 0;   // SyncMessage (v3.2)
    uint64_t gap_detects_received = 0;     // GapDetect (v3.2)
    uint64_t execution_acks_received = 0;  // C2V_ExecutionAck
    uint64_t jobs_created_from_cloud = 0;
    uint64_t jobs_updated_from_cloud = 0;
    uint64_t jobs_deleted_from_cloud = 0;
    uint64_t jobs_rejected = 0;  // Jobs from cloud that couldn't be created
    uint64_t trigger_requests_received = 0;
    uint64_t trigger_requests_succeeded = 0;
    uint64_t trigger_requests_failed = 0;
    uint64_t quiescent_count = 0;          // How many times we reached quiescent state
};

/**
 * @brief Bidirectional bridge for Scheduler state sync (v3.2 dirty-first protocol)
 *
 * v2c Lifecycle (vehicle → cloud):
 * 1. Start() - begins initialization phase
 * 2. After initialization_delay_ms, sends V2C_Hello with state checksum
 * 3. Responds to C2V_SyncDelta:
 *    - Applies jobs sent by cloud
 *    - Sends V2C_JobData with dirty jobs + requested jobs
 *    - Gap detection: exchanges job_ids when needed
 * 4. Sends V2C_Executions immediately when jobs complete
 * 5. Enters QUIESCENT state when checksums match (no traffic until change)
 *
 * c2v Lifecycle (cloud → vehicle):
 * 1. Subscribes to Backend Transport on_content callback
 * 2. Handles C2V_Envelope messages:
 *    - C2V_SyncDelta: apply jobs, send dirty + requested jobs, gap detection
 *    - C2V_ExecutionAck: stop retrying acknowledged executions
 *    - C2V_TriggerJob: execute job immediately
 *
 * State Machine:
 *   SEND_HELLO → WAIT_RESPONSE → APPLY_CHANGES → QUIESCENT
 *                    ↑                               │
 *                    └────── local change ───────────┘
 *
 * Thread model:
 * - Poll thread: detects local changes, sends messages (v2c)
 * - Execution retry: periodically retries unacked executions
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
     * @brief Get current state checksum (xxHash64)
     */
    uint64_t GetStateChecksum() const;

    /**
     * @brief Check if sync bridge is in quiescent state
     * @return true if currently quiescent (checksums match, no pending changes)
     */
    bool IsQuiescent() const;

    /**
     * @brief Get last seen cloud checksum
     * @return The checksum from the most recent C2V message
     */
    uint64_t GetLastSeenCloudChecksum() const;

private:
    // =========================================================================
    // v3.2 Protocol State Machine
    // =========================================================================

    /// Sync protocol state machine states (v3.2 simplified)
    ///
    /// v3.2 uses a simple two-state model:
    /// - SYNCING: Actively exchanging messages until convergence
    /// - QUIESCENT: In sync, no traffic until local change detected
    ///
    /// Legacy states kept for compatibility but mapped to SYNCING:
    /// - DISCONNECTED → treated as SYNCING on reconnect
    /// - SEND_HELLO, WAIT_RESPONSE, SEND_JOB_DATA, APPLY_CHANGES → all SYNCING
    enum class SyncState {
        DISCONNECTED,      // Not connected to transport
        SEND_HELLO,        // [legacy] mapped to SYNCING
        WAIT_RESPONSE,     // [legacy] mapped to SYNCING
        SEND_JOB_DATA,     // [legacy] mapped to SYNCING
        APPLY_CHANGES,     // [legacy] mapped to SYNCING
        QUIESCENT,         // In sync, no traffic until change
        SYNCING            // v3.2: Actively exchanging messages
    };

    /// Current sync state
    std::atomic<SyncState> sync_state_{SyncState::DISCONNECTED};

    /// Last seen cloud checksum (for quiescence detection)
    std::atomic<uint64_t> last_seen_c2v_checksum_{0};

    /// Jobs requested by cloud (from C2V_SyncDelta.request_job_ids)
    std::vector<std::string> pending_job_requests_;
    std::mutex pending_requests_mutex_;

    /// Configuration
    SchedulerSyncBridgeConfig config_;

    /// Backend Transport client for publishing
    std::unique_ptr<client::BackendTransportClient> transport_client_;

    /// gRPC channel to Scheduler service
    std::shared_ptr<grpc::Channel> scheduler_channel_;

    /// Scheduler service stubs
    std::unique_ptr<swdv::ifex_scheduler::list_jobs_service::Stub> list_jobs_stub_;
    std::unique_ptr<swdv::ifex_scheduler::create_job_service::Stub> create_job_stub_;
    std::unique_ptr<swdv::ifex_scheduler::update_job_service::Stub> update_job_stub_;
    std::unique_ptr<swdv::ifex_scheduler::delete_job_service::Stub> delete_job_stub_;
    std::unique_ptr<swdv::ifex_scheduler::pause_job_service::Stub> pause_job_stub_;
    std::unique_ptr<swdv::ifex_scheduler::resume_job_service::Stub> resume_job_stub_;
    std::unique_ptr<swdv::ifex_scheduler::trigger_job_service::Stub> trigger_job_stub_;
    std::unique_ptr<swdv::ifex_scheduler::get_job_service::Stub> get_job_stub_;
    std::unique_ptr<swdv::ifex_scheduler::set_job_remote_version_service::Stub> set_remote_version_stub_;

    /// Cached sync state for ALL jobs including tombstones (job_id -> state)
    std::unordered_map<std::string, SyncedJobState> synced_state_;
    mutable std::mutex state_mutex_;

    /// Pending executions waiting for acknowledgment (execution_id -> record)
    std::unordered_map<std::string, swdv::scheduler_sync_v3::ExecutionRecord> pending_execution_acks_;
    mutable std::mutex executions_mutex_;

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

    /// Worker threads
    std::thread poll_thread_;
    std::thread execution_retry_thread_;

    /// Condition variable for signaling
    std::condition_variable cv_;
    std::mutex cv_mutex_;

    /// Last activity time (for heartbeat)
    std::chrono::steady_clock::time_point last_activity_time_;

    // Internal methods

    /// Main polling loop (detects local changes, drives state machine)
    void PollLoop();

    /// Execution retry loop (resends unacknowledged executions)
    void ExecutionRetryLoop();

    /// Query Scheduler for current jobs
    std::vector<SyncedJobState> QuerySchedulerJobs();

    /// Query Scheduler for a single job by ID
    /// Returns nullopt if job not found
    std::optional<SyncedJobState> GetJobFromScheduler(const std::string& job_id);

    /// Compare current state with synced state, detect changes
    void DetectChanges(const std::vector<SyncedJobState>& current);

    /// Send heartbeat if no recent activity (sends V2C_Hello)
    void MaybeSendHeartbeat();

    /// Generate unique instance ID
    static std::string GenerateInstanceId();

    /// Load persisted state (if configured)
    void LoadPersistedState();

    /// Save state to disk (if configured)
    void PersistState();

    /// Map scheduler status to v3 status
    static swdv::scheduler_sync_v3::JobStatus MapStatus(
        swdv::scheduler_types::job_status_t status);

    /// Map scheduler wake policy to v3 wake policy
    static swdv::scheduler_sync_v3::WakePolicy MapWakePolicy(
        swdv::scheduler_types::wake_policy_t policy);

    /// Map scheduler sleep policy to v3 sleep policy
    static swdv::scheduler_sync_v3::SleepPolicy MapSleepPolicy(
        swdv::scheduler_types::sleep_policy_t policy);

    // =========================================================================
    // v3.2 Protocol: V2C (Vehicle → Cloud) Messages
    // =========================================================================

    /// Send V2C_Envelope to cloud
    void SendV2CEnvelope(const swdv::scheduler_sync_v3::V2C_Envelope& envelope);

    /// Send SyncMessage (v3.2 - jobs + acks + checksum)
    void SendSyncMessage(
        const std::vector<swdv::scheduler_sync_v3::JobRecord>& jobs,
        const std::vector<swdv::scheduler_sync_v3::JobVersionAck>& acked_jobs);

    /// Send GapDetect (v3.2 - job_ids for gap detection)
    void SendGapDetect(
        const std::vector<std::string>& job_ids,
        const std::vector<std::string>& request_job_ids);

    /// Send V2C_Executions (execution results, independent stream)
    void SendExecutions();

    /// Send V2C_TriggerResponse
    void SendTriggerResponse(const std::string& job_id,
                             const std::string& request_id,
                             bool accepted,
                             const std::string& error_message);

    // =========================================================================
    // v3.2 Protocol: C2V (Cloud → Vehicle) Message Handling
    // =========================================================================

    /// Handle incoming cloud message (called from on_content callback)
    void HandleCloudMessage(const std::vector<uint8_t>& payload);

    /// Handle SyncMessage from cloud (v3.2 - process jobs, ACKs, check quiescence)
    void HandleSyncMessage(const swdv::scheduler_sync_v3::SyncMessage& msg);

    /// Handle GapDetect from cloud (v3.2 - compare job_ids, send missing jobs)
    void HandleGapDetect(const swdv::scheduler_sync_v3::GapDetect& msg);

    /// Handle C2V_ExecutionAck (remove from retry queue)
    void HandleExecutionAck(const swdv::scheduler_sync_v3::C2V_ExecutionAck& ack);

    /// Handle C2V_TriggerJob (execute job immediately)
    void HandleTriggerJob(const swdv::scheduler_sync_v3::C2V_TriggerJob& trigger);

    // =========================================================================
    // Job Operations (Scheduler gRPC)
    // =========================================================================

    /// Operation result for internal use
    struct OperationResult {
        bool success = false;
        std::string job_id;
        std::string error_message;
    };

    /// Create a job in the local Scheduler from cloud sync
    OperationResult CreateJobFromCloud(const swdv::scheduler_sync_v3::JobRecord& job);

    /// Update a job in the local Scheduler from cloud sync
    OperationResult UpdateJobFromCloud(const swdv::scheduler_sync_v3::JobRecord& job);

    /// Delete a job from the local Scheduler
    OperationResult DeleteJobFromScheduler(const std::string& job_id);

    /// Apply a cloud job to local state (create or update)
    void ApplyCloudJob(const swdv::scheduler_sync_v3::JobRecord& remote_job);

    /// Process a single job from cloud using sync engine
    void ProcessCloudJob(const swdv::scheduler_sync_v3::JobRecord& remote_job);

    // =========================================================================
    // State & Checksum
    // =========================================================================

    /// Compute state checksum for quiescence detection (xxHash64)
    uint64_t ComputeStateChecksumXxHash() const;

    /// Record an execution (queues for sending to cloud)
    void RecordExecution(const std::string& job_id,
                        uint64_t executed_at_ms,
                        uint64_t duration_ms,
                        swdv::scheduler_sync_v3::JobStatus status,
                        const std::string& result_json,
                        const std::string& error_message);

    /// Generate unique execution ID
    static std::string GenerateExecutionId();

    // =========================================================================
    // v3.2 Helper Methods (Dirty-first sync)
    // =========================================================================

    /// Get all dirty jobs (where version != synced_version)
    std::vector<SyncedJobState> GetDirtyJobs() const;

    /// Get all job IDs (for gap detection)
    std::vector<std::string> GetAllJobIds() const;

    /// Set remote_version for a job to track what cloud has confirmed (v3.2)
    void SetJobRemoteVersion(const std::string& job_id, uint64_t cloud_seq, uint64_t vehicle_seq);

    /// Convert SyncedJobState to JobRecord protobuf
    swdv::scheduler_sync_v3::JobRecord StateToJobRecord(const SyncedJobState& state) const;
};

}  // namespace ifex::reference
