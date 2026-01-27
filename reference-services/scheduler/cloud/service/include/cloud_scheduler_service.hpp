#pragma once

#include "cloud-scheduler-service.grpc.pb.h"
#include "scheduler-types.pb.h"
#include "scheduler-sync-v2.pb.h"

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
    // No transport config - sync bridge handles backend communication
};

/// In-memory cloud scheduler service for testing.
///
/// Provides the gRPC API defined by the IFEX cloud-scheduler-service spec.
/// Each method is a separate gRPC service per IFEX conventions.
/// Stores state in-memory instead of PostgreSQL.
/// Vehicle communication is handled by CloudSchedulerSyncBridge (not this service).
///
/// Includes internal API for sync bridge (get_jobs_for_vehicle, upsert_job, etc.)
class CloudSchedulerService final
    : public sched::create_job_service::Service
    , public sched::update_job_service::Service
    , public sched::delete_job_service::Service
    , public sched::pause_job_service::Service
    , public sched::resume_job_service::Service
    , public sched::trigger_job_service::Service
    , public sched::get_job_service::Service
    , public sched::list_jobs_service::Service
    , public sched::list_jobs_hash_service::Service
    , public sched::list_executions_service::Service
    , public sched::list_executions_hash_service::Service
    , public sched::healthy_service::Service
    // Internal API for sync bridge
    , public sched::get_jobs_for_vehicle_service::Service
    , public sched::upsert_job_service::Service
    , public sched::record_execution_service::Service
    , public sched::get_vehicle_sync_state_service::Service
    , public sched::update_vehicle_sync_state_service::Service
    , public sched::get_pending_syncs_service::Service {
public:
    explicit CloudSchedulerService(const CloudSchedulerServiceConfig& config);
    ~CloudSchedulerService();

    // Non-copyable
    CloudSchedulerService(const CloudSchedulerService&) = delete;
    CloudSchedulerService& operator=(const CloudSchedulerService&) = delete;

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

    grpc::Status list_jobs_hash(
        grpc::ServerContext* context,
        const sched::list_jobs_hash_request* request,
        sched::list_jobs_hash_response* response) override;

    grpc::Status list_executions(
        grpc::ServerContext* context,
        const sched::list_executions_request* request,
        sched::list_executions_response* response) override;

    grpc::Status list_executions_hash(
        grpc::ServerContext* context,
        const sched::list_executions_hash_request* request,
        sched::list_executions_hash_response* response) override;

    grpc::Status healthy(
        grpc::ServerContext* context,
        const sched::healthy_request* request,
        sched::healthy_response* response) override;

    // =========================================================================
    // Internal API for Sync Bridge
    // =========================================================================

    grpc::Status get_jobs_for_vehicle(
        grpc::ServerContext* context,
        const sched::get_jobs_for_vehicle_request* request,
        sched::get_jobs_for_vehicle_response* response) override;

    grpc::Status upsert_job(
        grpc::ServerContext* context,
        const sched::upsert_job_request* request,
        sched::upsert_job_response* response) override;

    grpc::Status record_execution(
        grpc::ServerContext* context,
        const sched::record_execution_request* request,
        sched::record_execution_response* response) override;

    grpc::Status get_vehicle_sync_state(
        grpc::ServerContext* context,
        const sched::get_vehicle_sync_state_request* request,
        sched::get_vehicle_sync_state_response* response) override;

    grpc::Status update_vehicle_sync_state(
        grpc::ServerContext* context,
        const sched::update_vehicle_sync_state_request* request,
        sched::update_vehicle_sync_state_response* response) override;

    grpc::Status get_pending_syncs(
        grpc::ServerContext* context,
        const sched::get_pending_syncs_request* request,
        sched::get_pending_syncs_response* response) override;

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

    // =========================================================================
    // Sync Protocol v2 Helpers (used by internal API)
    // =========================================================================

    /// Compute content hash for a job
    uint64_t ComputeJobHash(const scheduler_types::job_t& job);

    /// Compute state checksum for a vehicle
    uint64_t ComputeStateChecksum(const std::string& vehicle_id) const;

    /// Convert job_info_t to sync v2 JobRecord
    void JobInfoToRecord(
        const scheduler_types::job_t& job,
        const ifex::scheduler::VersionVector& version,
        swdv::scheduler_sync_v2::JobRecord* record);

    /// Convert sync v2 JobRecord to job_info_t
    void RecordToJobInfo(
        const swdv::scheduler_sync_v2::JobRecord& record,
        scheduler_types::job_t* job);

    // =========================================================================
    // State
    // =========================================================================

    CloudSchedulerServiceConfig config_;

    // Job storage: vehicle_id -> job_id -> job
    // Version is stored directly in job_info_t (cloud_seq, vehicle_seq fields)
    mutable std::mutex jobs_mutex_;
    std::map<std::string, std::map<std::string, scheduler_types::job_t>> jobs_;

    // Sync state tracking: vehicle_id -> job_id -> sync_state
    std::map<std::string, std::map<std::string, scheduler_types::sync_state_t>> job_sync_states_;

    // Execution history: vehicle_id -> job_id -> executions
    mutable std::mutex executions_mutex_;
    std::map<std::string, std::map<std::string, std::vector<scheduler_types::execution_record_t>>> executions_;

    // Vehicle sync state: vehicle_id -> sync_state (checksums for quiescence)
    mutable std::mutex sync_state_mutex_;
    std::map<std::string, sched::vehicle_sync_state_t> vehicle_sync_states_;

    // Job ID counter
    std::atomic<uint64_t> job_counter_{0};

    // =========================================================================
    // Internal Helpers
    // =========================================================================

    /// Recompute and update cloud_checksum for a vehicle after job changes
    void UpdateCloudChecksum(const std::string& vehicle_id);
};

}  // namespace ifex::cloud
