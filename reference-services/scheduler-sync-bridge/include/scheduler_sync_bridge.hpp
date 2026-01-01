/**
 * @file scheduler_sync_bridge.hpp
 * @brief Bridge service for synchronizing Scheduler state to cloud
 *
 * The SchedulerSyncBridge monitors the Scheduler service and publishes
 * job state changes to the cloud via Backend Transport.
 *
 * Design principles for traffic minimization:
 * - Delta sync: Only send changes, not full state
 * - Terminal state sync: Sync COMPLETED/FAILED once with result, then forget
 * - Batch execution results: Group multiple job completions
 * - Sequence numbers: Enable gap detection and ordering
 * - State checksum: Verify sync without full resync
 */

#pragma once

#include "backend_transport_client.hpp"
#include "scheduler-sync-envelope.pb.h"
#include "ifex-scheduler-service.grpc.pb.h"

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

    /// Only sync jobs that change to terminal states (COMPLETED/FAILED)
    /// If false, also syncs RUNNING state changes
    bool terminal_states_only = true;

    /// Path to persist sync state (empty = no persistence)
    std::string state_persistence_path;
};

/**
 * @brief Cached state of a synced job
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
    swdv::scheduler_sync_envelope::job_sync_status_t status;
    uint64_t created_at_ms = 0;
    uint64_t updated_at_ms = 0;
    uint64_t last_synced_sequence = 0;

    /// Compute hash for change detection
    uint64_t ComputeHash() const;

    /// Check if job is in terminal state
    bool IsTerminal() const {
        return status == swdv::scheduler_sync_envelope::COMPLETED ||
               status == swdv::scheduler_sync_envelope::FAILED ||
               status == swdv::scheduler_sync_envelope::CANCELLED;
    }
};

/**
 * @brief Statistics for monitoring sync bridge health
 */
struct SchedulerSyncStats {
    uint64_t events_sent = 0;
    uint64_t full_syncs_sent = 0;
    uint64_t delta_syncs_sent = 0;
    uint64_t execution_results_sent = 0;
    uint64_t heartbeats_sent = 0;
    uint64_t bytes_sent = 0;
    uint64_t active_jobs_tracked = 0;
    uint64_t last_sync_timestamp_ns = 0;
    uint64_t current_sequence = 0;
    bool is_initialized = false;
    bool is_connected = false;
};

/**
 * @brief Bridge for synchronizing Scheduler state to cloud
 *
 * Lifecycle:
 * 1. Start() - begins initialization phase
 * 2. After initialization_delay_ms, captures initial state
 * 3. Publishes FULL_SYNC event with all active jobs
 * 4. Polls Scheduler at poll_interval_ms
 * 5. Publishes delta events for job changes
 * 6. Publishes JOB_EXECUTED for completed/failed jobs
 * 7. Stop() - graceful shutdown
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

    /// Scheduler service stub (for get_jobs)
    std::unique_ptr<swdv::ifex_scheduler::get_jobs_service::Stub> get_jobs_stub_;

    /// Cached sync state for ACTIVE jobs only (job_id -> state)
    /// Terminal jobs are removed after syncing their execution result
    std::unordered_map<std::string, SyncedJobState> synced_state_;
    mutable std::mutex state_mutex_;

    /// Jobs that have been synced in terminal state (to avoid re-syncing)
    std::unordered_set<std::string> synced_terminal_jobs_;

    /// Pending events to batch
    std::vector<swdv::scheduler_sync_envelope::sync_event_t> pending_events_;
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

    /// Worker threads
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

    /// Compare current state with synced state, generate events
    void DetectChanges(const std::vector<SyncedJobState>& current);

    /// Queue an event for sending
    void QueueEvent(swdv::scheduler_sync_envelope::sync_event_t event);

    /// Send queued events
    void FlushEvents();

    /// Send a full sync message
    void SendFullSync(const std::vector<SyncedJobState>& jobs);

    /// Send heartbeat if no recent activity
    void MaybeSendHeartbeat();

    /// Build job_info_t from cached state
    swdv::scheduler_sync_envelope::job_info_t BuildJobInfo(
        const SyncedJobState& state);

    /// Build execution_result_t for a completed/failed job
    swdv::scheduler_sync_envelope::execution_result_t BuildExecutionResult(
        const SyncedJobState& current,
        const SyncedJobState& previous);

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

    /// Map scheduler status to sync status
    static swdv::scheduler_sync_envelope::job_sync_status_t MapStatus(
        swdv::ifex_scheduler::job_status_t status);
};

}  // namespace ifex::reference
