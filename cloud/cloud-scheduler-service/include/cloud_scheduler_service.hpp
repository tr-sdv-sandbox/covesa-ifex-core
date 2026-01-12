#pragma once

#include "cloud-scheduler-service.grpc.pb.h"
#include "scheduler-command-envelope.pb.h"
#include "scheduler-sync-v2.pb.h"
#include "cloud_backend_transport_client.hpp"
#include "version_vector.hpp"
#include "sync_engine.hpp"

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

/// Configuration for CloudSchedulerService
struct CloudSchedulerServiceConfig {
    std::string backend_transport_address = "localhost:50100";
    uint32_t scheduler_content_id = 202;
};

/// In-memory cloud scheduler service for testing.
///
/// Provides the same gRPC API as the production cloud scheduler,
/// but stores state in-memory instead of PostgreSQL.
/// Uses CloudBackendTransportClient for vehicle communication.
class CloudSchedulerService final : public ::ifex::cloud::scheduler::CloudSchedulerService::Service {
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

    // =========================================================================
    // gRPC Service Methods
    // =========================================================================

    grpc::Status CreateJob(
        grpc::ServerContext* context,
        const ::ifex::cloud::scheduler::CreateJobRequest* request,
        ::ifex::cloud::scheduler::CreateJobResponse* response) override;

    grpc::Status UpdateJob(
        grpc::ServerContext* context,
        const ::ifex::cloud::scheduler::UpdateJobRequest* request,
        ::ifex::cloud::scheduler::UpdateJobResponse* response) override;

    grpc::Status DeleteJob(
        grpc::ServerContext* context,
        const ::ifex::cloud::scheduler::DeleteJobRequest* request,
        ::ifex::cloud::scheduler::DeleteJobResponse* response) override;

    grpc::Status PauseJob(
        grpc::ServerContext* context,
        const ::ifex::cloud::scheduler::PauseJobRequest* request,
        ::ifex::cloud::scheduler::PauseJobResponse* response) override;

    grpc::Status ResumeJob(
        grpc::ServerContext* context,
        const ::ifex::cloud::scheduler::ResumeJobRequest* request,
        ::ifex::cloud::scheduler::ResumeJobResponse* response) override;

    grpc::Status TriggerJob(
        grpc::ServerContext* context,
        const ::ifex::cloud::scheduler::TriggerJobRequest* request,
        ::ifex::cloud::scheduler::TriggerJobResponse* response) override;

    grpc::Status GetJob(
        grpc::ServerContext* context,
        const ::ifex::cloud::scheduler::GetJobRequest* request,
        ::ifex::cloud::scheduler::GetJobResponse* response) override;

    grpc::Status ListJobs(
        grpc::ServerContext* context,
        const ::ifex::cloud::scheduler::ListJobsRequest* request,
        ::ifex::cloud::scheduler::ListJobsResponse* response) override;

    grpc::Status GetJobExecutions(
        grpc::ServerContext* context,
        const ::ifex::cloud::scheduler::GetJobExecutionsRequest* request,
        ::ifex::cloud::scheduler::GetJobExecutionsResponse* response) override;

    // Fleet operations (simplified for testing)
    grpc::Status CreateFleetJob(
        grpc::ServerContext* context,
        const ::ifex::cloud::scheduler::CreateFleetJobRequest* request,
        ::ifex::cloud::scheduler::CreateFleetJobResponse* response) override;

    grpc::Status DeleteFleetJob(
        grpc::ServerContext* context,
        const ::ifex::cloud::scheduler::DeleteFleetJobRequest* request,
        ::ifex::cloud::scheduler::DeleteFleetJobResponse* response) override;

    grpc::Status GetFleetJobStats(
        grpc::ServerContext* context,
        const ::ifex::cloud::scheduler::GetFleetJobStatsRequest* request,
        ::ifex::cloud::scheduler::GetFleetJobStatsResponse* response) override;

    // =========================================================================
    // Test Helpers
    // =========================================================================

    /// Get count of jobs for a vehicle
    size_t GetJobCount(const std::string& vehicle_id) const;

    /// Get count of all jobs
    size_t GetTotalJobCount() const;

    /// Clear all in-memory state
    void ClearAllJobs();

private:
    /// Generate a unique job ID
    std::string GenerateJobId();

    /// Generate a unique command ID
    std::string GenerateCommandId();

    /// Send a scheduler command to a vehicle
    bool SendCommand(const std::string& vehicle_id,
                     const swdv::scheduler_command_envelope::scheduler_command_t& command);

    /// Handle incoming sync message from vehicle
    void HandleSyncMessage(const std::string& vehicle_id,
                           const std::vector<uint8_t>& payload);

    /// Convert ISO8601 string to epoch milliseconds
    static uint64_t Iso8601ToEpochMs(const std::string& iso_str);

    /// Convert epoch milliseconds to ISO8601 string
    static std::string EpochMsToIso8601(uint64_t epoch_ms);

    CloudSchedulerServiceConfig config_;
    std::unique_ptr<CloudBackendTransportClient> transport_;

    // In-memory job storage: vehicle_id -> job_id -> JobInfo
    mutable std::mutex jobs_mutex_;
    std::map<std::string, std::map<std::string, ::ifex::cloud::scheduler::JobInfo>> jobs_;

    // Execution history: vehicle_id -> job_id -> [ExecutionInfo]
    mutable std::mutex executions_mutex_;
    std::map<std::string, std::map<std::string, std::vector<::ifex::cloud::scheduler::ExecutionInfo>>> executions_;

    // Sync v2: version vectors per job (vehicle_id -> job_id -> version)
    std::map<std::string, std::map<std::string, sync::VersionVector>> job_versions_;

    // Sync v2: sync state per job
    std::map<std::string, std::map<std::string, swdv::scheduler_sync_v2::SyncState>> job_sync_states_;

    std::atomic<bool> running_{false};
    std::atomic<uint64_t> job_counter_{0};
    std::atomic<uint64_t> command_counter_{0};
    std::atomic<uint64_t> sync_id_counter_{0};

    // =========================================================================
    // Sync Protocol v2 Methods
    // =========================================================================

    /// Handle incoming V2C_SyncMessage from vehicle
    void HandleV2SyncMessage(const std::string& vehicle_id,
                             const swdv::scheduler_sync_v2::V2C_SyncMessage& msg);

    /// Process a single job record from vehicle sync
    void ProcessVehicleJob(const std::string& vehicle_id,
                          const swdv::scheduler_sync_v2::JobRecord& record);

    /// Process execution records from vehicle
    void ProcessVehicleExecutions(const std::string& vehicle_id,
                                  const google::protobuf::RepeatedPtrField<swdv::scheduler_sync_v2::ExecutionRecord>& executions);

    /// Send C2V_SyncMessage to vehicle with pending changes
    void SendV2SyncMessage(const std::string& vehicle_id);

    /// Send sync acknowledgment
    void SendSyncAck(const std::string& vehicle_id,
                    const std::string& sync_id,
                    bool success,
                    const std::string& error_message = "");

    /// Generate unique sync ID
    std::string GenerateSyncId();

    /// Convert JobInfo to v2 JobRecord
    void JobInfoToRecord(const ::ifex::cloud::scheduler::JobInfo& job,
                        const sync::VersionVector& version,
                        swdv::scheduler_sync_v2::JobRecord* record);

    /// Convert v2 JobRecord to JobInfo
    void RecordToJobInfo(const swdv::scheduler_sync_v2::JobRecord& record,
                        ::ifex::cloud::scheduler::JobInfo* job);
};

}  // namespace ifex::cloud
