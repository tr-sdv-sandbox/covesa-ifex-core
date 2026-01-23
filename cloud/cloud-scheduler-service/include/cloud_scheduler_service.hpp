#pragma once

#include "cloud-scheduler-service.grpc.pb.h"
#include "scheduler-types.pb.h"
#include "scheduler-sync-v2.pb.h"
#include "cloud_backend_transport_client.hpp"

// ifex-scheduler library (canonical job structure, hash, version vectors)
#include "version_vector.hpp"
#include "sync_engine.hpp"
#include "job.hpp"
#include "job_hash.hpp"

#include <grpcpp/grpcpp.h>
#include <glog/logging.h>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ifex::cloud {

// Alias for the IFEX-generated namespace
namespace sched = ::swdv::cloud_scheduler_service;
namespace scheduler_types = ::swdv::scheduler_types;

/// Configuration for CloudSchedulerService
struct CloudSchedulerServiceConfig {
    std::string backend_transport_address = "localhost:50100";
    uint32_t scheduler_content_id = 202;
};

/// In-memory cloud scheduler service for testing.
///
/// Provides the gRPC API defined by the IFEX cloud-scheduler-service spec.
/// Each method is a separate gRPC service per IFEX conventions.
/// Stores state in-memory instead of PostgreSQL.
/// Uses CloudBackendTransportClient for vehicle communication.
class CloudSchedulerService final
    : public sched::create_job_service::Service
    , public sched::update_job_service::Service
    , public sched::delete_job_service::Service
    , public sched::pause_job_service::Service
    , public sched::resume_job_service::Service
    , public sched::trigger_job_service::Service
    , public sched::get_job_service::Service
    , public sched::list_jobs_service::Service
    , public sched::get_job_executions_service::Service
    , public sched::create_fleet_job_service::Service
    , public sched::delete_fleet_job_service::Service
    , public sched::get_fleet_job_stats_service::Service
    , public sched::healthy_service::Service {
public:
    explicit CloudSchedulerService(const CloudSchedulerServiceConfig& config);
    ~CloudSchedulerService();

    // Non-copyable
    CloudSchedulerService(const CloudSchedulerService&) = delete;
    CloudSchedulerService& operator=(const CloudSchedulerService&) = delete;

    /// Start the service (connects to backend transport, starts sync handler)
    bool Start();

    /// Stop the service
    void Stop();

    /// Check if service is running
    bool IsRunning() const { return running_; }

    /// Register all services with a gRPC server builder
    void RegisterServices(grpc::ServerBuilder& builder);

    // =========================================================================
    // gRPC Service Methods (IFEX per-method services)
    // =========================================================================

    grpc::Status create_job(
        grpc::ServerContext* context,
        const sched::create_job_request* request,
        sched::create_job_response* response) override;

    grpc::Status update_job(
        grpc::ServerContext* context,
        const sched::update_job_request* request,
        sched::update_job_response* response) override;

    grpc::Status delete_job(
        grpc::ServerContext* context,
        const sched::delete_job_request* request,
        sched::delete_job_response* response) override;

    grpc::Status pause_job(
        grpc::ServerContext* context,
        const sched::pause_job_request* request,
        sched::pause_job_response* response) override;

    grpc::Status resume_job(
        grpc::ServerContext* context,
        const sched::resume_job_request* request,
        sched::resume_job_response* response) override;

    grpc::Status trigger_job(
        grpc::ServerContext* context,
        const sched::trigger_job_request* request,
        sched::trigger_job_response* response) override;

    grpc::Status get_job(
        grpc::ServerContext* context,
        const sched::get_job_request* request,
        sched::get_job_response* response) override;

    grpc::Status list_jobs(
        grpc::ServerContext* context,
        const sched::list_jobs_request* request,
        sched::list_jobs_response* response) override;

    grpc::Status get_job_executions(
        grpc::ServerContext* context,
        const sched::get_job_executions_request* request,
        sched::get_job_executions_response* response) override;

    grpc::Status create_fleet_job(
        grpc::ServerContext* context,
        const sched::create_fleet_job_request* request,
        sched::create_fleet_job_response* response) override;

    grpc::Status delete_fleet_job(
        grpc::ServerContext* context,
        const sched::delete_fleet_job_request* request,
        sched::delete_fleet_job_response* response) override;

    grpc::Status get_fleet_job_stats(
        grpc::ServerContext* context,
        const sched::get_fleet_job_stats_request* request,
        sched::get_fleet_job_stats_response* response) override;

    grpc::Status healthy(
        grpc::ServerContext* context,
        const sched::healthy_request* request,
        sched::healthy_response* response) override;

    // =========================================================================
    // Test Helpers
    // =========================================================================

    /// Get number of jobs for a specific vehicle (excludes tombstones)
    size_t GetJobCount(const std::string& vehicle_id) const;

    /// Get total job count across all vehicles (excludes tombstones)
    size_t GetTotalJobCount() const;

    /// Clear all jobs (for testing)
    void ClearAllJobs();

private:
    // =========================================================================
    // Internal Implementation
    // =========================================================================

    /// Generate unique job ID
    std::string GenerateJobId();

    /// Convert ISO8601 timestamp to epoch milliseconds
    static uint64_t Iso8601ToEpochMs(const std::string& iso_str);

    /// Convert epoch milliseconds to ISO8601 string
    static std::string EpochMsToIso8601(uint64_t epoch_ms);

    /// Send pending jobs to vehicle via sync protocol
    void SendPendingJobsToVehicle(const std::string& vehicle_id);

    /// Send trigger job request to vehicle
    bool SendTriggerJobRequest(
        const std::string& vehicle_id,
        const std::string& job_id,
        const std::string& requester_id);

    /// Handle incoming sync message from vehicle
    void HandleSyncMessage(
        const std::string& vehicle_id,
        const std::vector<uint8_t>& payload);

    // =========================================================================
    // Sync Protocol v2 Methods
    // =========================================================================

    /// Compute content hash for a job
    uint64_t ComputeJobHash(const sched::job_info_t& job);

    /// Compute state checksum for a vehicle
    uint64_t ComputeStateChecksum(const std::string& vehicle_id) const;

    /// Handle v2 sync message from vehicle
    void HandleV2SyncMessage(
        const std::string& vehicle_id,
        const swdv::scheduler_sync_v2::V2C_SyncMessage& msg);

    /// Process a job record from vehicle
    void ProcessVehicleJob(
        const std::string& vehicle_id,
        const swdv::scheduler_sync_v2::JobRecord& record);

    /// Process execution records from vehicle
    void ProcessVehicleExecutions(
        const std::string& vehicle_id,
        const google::protobuf::RepeatedPtrField<swdv::scheduler_sync_v2::ExecutionRecord>& executions);

    /// Send v2 sync message to vehicle
    void SendV2SyncMessage(const std::string& vehicle_id);

    /// Convert job_info_t to sync v2 JobRecord
    void JobInfoToRecord(
        const sched::job_info_t& job,
        const ifex::scheduler::VersionVector& version,
        swdv::scheduler_sync_v2::JobRecord* record);

    /// Convert sync v2 JobRecord to job_info_t
    void RecordToJobInfo(
        const swdv::scheduler_sync_v2::JobRecord& record,
        sched::job_info_t* job);

    // =========================================================================
    // State
    // =========================================================================

    CloudSchedulerServiceConfig config_;
    std::atomic<bool> running_{false};

    // Transport client for vehicle communication
    std::unique_ptr<CloudBackendTransportClient> transport_;

    // Job storage: vehicle_id -> job_id -> job
    mutable std::mutex jobs_mutex_;
    std::map<std::string, std::map<std::string, sched::job_info_t>> jobs_;

    // Version vectors for sync v2: vehicle_id -> job_id -> version
    std::map<std::string, std::map<std::string, ifex::scheduler::VersionVector>> job_versions_;

    // Sync state tracking: vehicle_id -> job_id -> sync_state
    std::map<std::string, std::map<std::string, scheduler_types::sync_state_t>> job_sync_states_;

    // Execution history: vehicle_id -> job_id -> executions
    mutable std::mutex executions_mutex_;
    std::map<std::string, std::map<std::string, std::vector<sched::execution_info_t>>> executions_;

    // Job ID counter
    std::atomic<uint64_t> job_counter_{0};
};

}  // namespace ifex::cloud
