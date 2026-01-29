#pragma once

#include "cloud-scheduler-sync-bridge.grpc.pb.h"
#include "cloud-scheduler-service.grpc.pb.h"
#include "cloud-backend-transport-service.grpc.pb.h"
#include "scheduler-sync-v3.pb.h"
#include "scheduler-types.pb.h"

// ifex-scheduler library (version vectors, sync engine)
#include "version_vector.hpp"
#include "sync_engine.hpp"
#include "job.hpp"
#include "job_hash.hpp"

#include <grpcpp/grpcpp.h>
#include <glog/logging.h>

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <condition_variable>
#include <thread>
#include <vector>

namespace ifex::cloud {

// Aliases for generated namespaces
namespace bridge_pb = ::swdv::cloud_scheduler_sync_bridge;
namespace sched_pb = ::swdv::cloud_scheduler_service;
namespace transport_pb = ::swdv::cloud_backend_transport_service;
namespace sync_v3 = ::swdv::scheduler_sync_v3;
namespace scheduler_types = ::swdv::scheduler_types;

/// Configuration for CloudSchedulerSyncBridge
struct CloudSchedulerSyncBridgeConfig {
    std::string scheduler_address = "localhost:50102";
    std::string transport_address = "localhost:50100";
    uint32_t content_id = 202;  // Scheduler sync content ID
    std::string bridge_instance_id;  // Unique instance ID (for restart detection)
    uint32_t poll_interval_ms = 1000;  // How often to poll for pending syncs
};

/// Cloud-side scheduler sync bridge (v3.2 protocol).
///
/// Handles bidirectional sync between cloud scheduler and vehicles using
/// the dirty-first v3.2 protocol for bandwidth efficiency.
///
/// Protocol flow:
/// 1. Subscribe to V2C messages from vehicles via backend transport
/// 2. Handle V2C_Envelope messages:
///    - V2C_Hello: Compare checksums, send dirty jobs or job_ids for gap detection
///    - V2C_JobData: Accept jobs, process gap detection requests
///    - V2C_Executions: Store executions, send ack
///    - V2C_TriggerResponse: Record trigger result
/// 3. Send C2V_Envelope responses:
///    - C2V_SyncDelta: Send jobs and/or request jobs (with job_ids for gap detection)
///    - C2V_ExecutionAck: Acknowledge received executions
///    - C2V_TriggerJob: Request immediate job execution
class CloudSchedulerSyncBridge final
    : public bridge_pb::get_stats_service::Service
    , public bridge_pb::get_health_service::Service
    , public bridge_pb::get_vehicle_sync_info_service::Service
    , public bridge_pb::force_sync_service::Service
    , public bridge_pb::trigger_job_service::Service
    , public bridge_pb::healthy_service::Service {
public:
    explicit CloudSchedulerSyncBridge(const CloudSchedulerSyncBridgeConfig& config);
    ~CloudSchedulerSyncBridge();

    // Non-copyable
    CloudSchedulerSyncBridge(const CloudSchedulerSyncBridge&) = delete;
    CloudSchedulerSyncBridge& operator=(const CloudSchedulerSyncBridge&) = delete;

    /// Start the sync bridge
    bool Start();

    /// Stop the sync bridge
    void Stop();

    /// Check if running
    bool IsRunning() const { return running_; }

    /// Register gRPC services with server builder
    void RegisterServices(grpc::ServerBuilder& builder);

    // =========================================================================
    // gRPC Service Methods (Bridge API)
    // =========================================================================

    grpc::Status get_stats(
        grpc::ServerContext* context,
        const bridge_pb::get_stats_request* request,
        bridge_pb::get_stats_response* response) override;

    grpc::Status get_health(
        grpc::ServerContext* context,
        const bridge_pb::get_health_request* request,
        bridge_pb::get_health_response* response) override;

    grpc::Status get_vehicle_sync_info(
        grpc::ServerContext* context,
        const bridge_pb::get_vehicle_sync_info_request* request,
        bridge_pb::get_vehicle_sync_info_response* response) override;

    grpc::Status force_sync(
        grpc::ServerContext* context,
        const bridge_pb::force_sync_request* request,
        bridge_pb::force_sync_response* response) override;

    grpc::Status trigger_job(
        grpc::ServerContext* context,
        const bridge_pb::trigger_job_request* request,
        bridge_pb::trigger_job_response* response) override;

    grpc::Status healthy(
        grpc::ServerContext* context,
        const bridge_pb::healthy_request* request,
        bridge_pb::healthy_response* response) override;

    // =========================================================================
    // Testing / Monitoring API
    // =========================================================================

    /// Get vehicle sync state (for testing and monitoring)
    /// Returns the cloud's view of sync state for a vehicle
    sched_pb::vehicle_sync_state_t GetVehicleSyncState(const std::string& vehicle_id);

private:
    // =========================================================================
    // Internal Implementation
    // =========================================================================

    /// Create gRPC channels and stubs
    bool ConnectToServices();

    /// Subscribe to vehicle messages
    void StartMessageSubscription();

    /// Start polling for pending syncs
    void StartPendingSyncsPoll();

    /// Poll loop for pending syncs
    void PollLoop();

    // =========================================================================
    // V2C Message Handling (v3.2 Protocol)
    // =========================================================================

    /// Handle incoming V2C envelope message
    void HandleV2CEnvelope(
        const std::string& vehicle_id,
        const std::vector<uint8_t>& payload);

    /// Handle SyncMessage from vehicle (v3.2 - process jobs, ACKs, check quiescence)
    void HandleSyncMessage(
        const std::string& vehicle_id,
        const sync_v3::SyncMessage& msg);

    /// Handle GapDetect from vehicle (v3.2 - compare job_ids, send missing jobs)
    void HandleGapDetect(
        const std::string& vehicle_id,
        const sync_v3::GapDetect& msg);

    /// Handle V2C_Executions
    void HandleExecutions(
        const std::string& vehicle_id,
        const sync_v3::V2C_Executions& executions);

    /// Handle V2C_TriggerResponse
    void HandleTriggerResponse(
        const std::string& vehicle_id,
        const sync_v3::V2C_TriggerResponse& response);

    // =========================================================================
    // C2V Message Sending (v3.2 Protocol)
    // =========================================================================

    /// Send C2V_Envelope to vehicle
    void SendC2VEnvelope(const std::string& vehicle_id,
                         const sync_v3::C2V_Envelope& envelope);

    /// Send SyncMessage to vehicle (v3.2 - jobs + acks + checksum)
    void SendSyncMessage(const std::string& vehicle_id,
                         const std::vector<sync_v3::JobRecord>& jobs,
                         const std::vector<sync_v3::JobVersionAck>& acked_jobs);

    /// Send GapDetect to vehicle (v3.2 - job_ids for gap detection)
    void SendGapDetect(const std::string& vehicle_id,
                       const std::vector<std::string>& job_ids,
                       const std::vector<std::string>& request_job_ids);

    /// Send C2V_ExecutionAck
    void SendExecutionAck(const std::string& vehicle_id,
                          const std::vector<std::string>& execution_ids);

    // =========================================================================
    // Scheduler API
    // =========================================================================

    /// Get jobs for vehicle from scheduler
    std::vector<scheduler_types::job_t> GetCloudJobs(const std::string& vehicle_id);

    /// Upsert a job to scheduler
    bool UpsertJob(const scheduler_types::job_t& job);

    /// Record an execution to scheduler
    bool RecordExecution(
        const std::string& vehicle_id,
        const std::string& job_id,
        const sync_v3::ExecutionRecord& execution);

    /// Update vehicle sync state in scheduler
    void UpdateVehicleSyncState(const std::string& vehicle_id, uint64_t v2c_checksum);

    // =========================================================================
    // Type Conversions
    // =========================================================================

    /// Convert V2C JobRecord to scheduler job_t
    scheduler_types::job_t V2CRecordToJobInfo(
        const std::string& vehicle_id,
        const sync_v3::JobRecord& record);

    /// Convert scheduler job_t to C2V JobRecord
    sync_v3::JobRecord JobInfoToC2VRecord(const scheduler_types::job_t& job);

    /// Compute state checksum
    uint64_t ComputeStateChecksum(const std::vector<scheduler_types::job_t>& jobs);

    /// Check if sync is quiescent (checksums match)
    bool IsQuiescent(
        const sched_pb::vehicle_sync_state_t& state,
        uint64_t v2c_checksum);

    // =========================================================================
    // v3.2 Dirty Tracking and Gap Detection
    // =========================================================================

    /// Get jobs that are dirty (version != synced_version) for a vehicle
    std::vector<scheduler_types::job_t> GetDirtyJobs(const std::string& vehicle_id);

    /// Get all job IDs for a vehicle (for gap detection)
    std::vector<std::string> GetAllJobIds(const std::string& vehicle_id);

    /// Update remote_version for a job to track what vehicle has confirmed
    void SetJobRemoteVersion(const std::string& vehicle_id, const std::string& job_id,
                             uint64_t cloud_seq, uint64_t vehicle_seq);

    // =========================================================================
    // State
    // =========================================================================

    CloudSchedulerSyncBridgeConfig config_;
    std::atomic<bool> running_{false};
    std::chrono::steady_clock::time_point start_time_;

    // gRPC channels and stubs
    std::shared_ptr<grpc::Channel> scheduler_channel_;
    std::shared_ptr<grpc::Channel> transport_channel_;

    // Scheduler internal API stubs
    std::unique_ptr<sched_pb::get_jobs_for_vehicle_service::Stub> get_jobs_stub_;
    std::unique_ptr<sched_pb::upsert_job_service::Stub> upsert_job_stub_;
    std::unique_ptr<sched_pb::record_execution_service::Stub> record_execution_stub_;
    std::unique_ptr<sched_pb::get_vehicle_sync_state_service::Stub> get_sync_state_stub_;
    std::unique_ptr<sched_pb::update_vehicle_sync_state_service::Stub> update_sync_state_stub_;
    std::unique_ptr<sched_pb::get_pending_syncs_service::Stub> get_pending_syncs_stub_;
    std::unique_ptr<sched_pb::set_job_remote_version_service::Stub> set_remote_version_stub_;

    // Backend transport stubs
    std::unique_ptr<transport_pb::send_to_vehicle_service::Stub> send_stub_;
    std::unique_ptr<transport_pb::on_vehicle_message_service::Stub> subscribe_stub_;

    // Message subscription thread
    std::thread subscription_thread_;
    std::atomic<bool> subscription_running_{false};
    std::unique_ptr<grpc::ClientContext> subscription_context_;
    std::mutex subscription_context_mutex_;

    // Pending syncs polling thread
    std::thread poll_thread_;
    std::condition_variable poll_cv_;
    std::mutex poll_mutex_;

    // Statistics
    mutable std::mutex stats_mutex_;
    std::atomic<uint64_t> v2c_messages_received_{0};
    std::atomic<uint64_t> v2c_messages_processed_{0};
    std::atomic<uint64_t> c2v_messages_sent_{0};
    std::atomic<uint64_t> jobs_upserted_{0};
    std::atomic<uint64_t> executions_recorded_{0};
    std::atomic<uint64_t> quiescent_skipped_{0};
    std::atomic<uint64_t> conflicts_resolved_{0};
    std::atomic<uint64_t> errors_{0};
    std::set<std::string> vehicles_seen_;

    // Connection state
    std::atomic<bool> scheduler_connected_{false};
    std::atomic<bool> transport_connected_{false};
    std::string last_error_;
    uint64_t last_error_timestamp_ms_{0};

    // Per-vehicle sync tracking
    mutable std::mutex vehicle_info_mutex_;
    std::map<std::string, bridge_pb::vehicle_sync_info_t> vehicle_sync_info_;
};

}  // namespace ifex::cloud
