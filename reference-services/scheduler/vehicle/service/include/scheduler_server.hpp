#pragma once

#include <grpcpp/grpcpp.h>
#include <glog/logging.h>
#include <nlohmann/json.hpp>
#include <yaml-cpp/yaml.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <queue>
#include <optional>
#include <regex>

#include "scheduler-service.grpc.pb.h"
#include "scheduler-types.pb.h"
#include "dispatcher-service.grpc.pb.h"
#include "scheduler-sync-v2.pb.h"
#include <ifex/discovery.hpp>
#include "version_vector.hpp"

namespace ifex::reference {

using json = nlohmann::json;

// Internal job representation
struct Job {
    std::string id;
    std::string title;
    std::string service_name;
    std::string method_name;
    json parameters;

    // Scheduling
    std::chrono::system_clock::time_point scheduled_time;
    std::string recurrence_rule;  // Cron expression or empty
    std::optional<std::chrono::system_clock::time_point> end_time;
    std::optional<std::chrono::system_clock::time_point> next_run_time;

    // Status tracking
    swdv::scheduler_types::job_status_t status = swdv::scheduler_types::JOB_STATUS_PENDING;

    // Wake/Sleep policies
    swdv::scheduler_types::wake_policy_t wake_policy = swdv::scheduler_types::WAKE_NO_WAKE;
    swdv::scheduler_types::sleep_policy_t sleep_policy = swdv::scheduler_types::SLEEP_NORMAL;
    uint32_t wake_lead_time_s = 0;

    // Timestamps
    std::chrono::system_clock::time_point created_at;
    std::chrono::system_clock::time_point updated_at;
    std::optional<std::chrono::system_clock::time_point> executed_at;
    std::optional<std::string> error_message;
    std::optional<std::string> result;  // Response from service call

    // User intent: paused jobs should not be scheduled
    bool paused = false;

    // --- Sync Protocol v2 fields ---
    // Version vector for conflict detection (see docs/scheduler-sync-protocol-v2.md)
    sync::VersionVector version;

    // Who created this job - determines conflict winner (immutable after creation)
    swdv::scheduler_sync_v2::JobAuthority authority =
        swdv::scheduler_sync_v2::AUTHORITY_VEHICLE;

    // Local tracking: job needs to be synced
    bool needs_sync = false;

    // Soft delete flag (tombstone for sync)
    bool deleted = false;
    std::optional<std::chrono::system_clock::time_point> deleted_at;

    // Convert to protobuf message
    void ToProto(swdv::scheduler_types::job_t* proto) const;

    // Convert to sync v2 JobRecord
    void ToSyncProto(swdv::scheduler_sync_v2::JobRecord* proto) const;

    // Create from protobuf message
    static std::unique_ptr<Job> FromProto(const swdv::ifex_scheduler::job_create_t& proto);

    // Create from sync v2 JobRecord (for receiving from cloud)
    static std::unique_ptr<Job> FromSyncProto(const swdv::scheduler_sync_v2::JobRecord& proto);

    // JSON serialization for persistence
    json ToJson() const;
    static std::unique_ptr<Job> FromJson(const json& j);

    // Increment version for local change (call before any modification)
    void IncrementVersion() {
        version.increment_vehicle();
        updated_at = std::chrono::system_clock::now();
        needs_sync = true;
    }
};

class SchedulerServer final : public swdv::ifex_scheduler::create_job_service::Service,
                              public swdv::ifex_scheduler::list_jobs_service::Service,
                              public swdv::ifex_scheduler::list_jobs_hash_service::Service,
                              public swdv::ifex_scheduler::get_job_service::Service,
                              public swdv::ifex_scheduler::update_job_service::Service,
                              public swdv::ifex_scheduler::delete_job_service::Service,
                              public swdv::ifex_scheduler::pause_job_service::Service,
                              public swdv::ifex_scheduler::resume_job_service::Service,
                              public swdv::ifex_scheduler::trigger_job_service::Service,
                              public swdv::ifex_scheduler::list_executions_service::Service,
                              public swdv::ifex_scheduler::list_executions_hash_service::Service {
public:
    struct Config {
        std::string discovery_endpoint;
        std::string persistence_dir;  // Empty = no persistence
    };

    explicit SchedulerServer(const std::string& service_discovery_endpoint);
    explicit SchedulerServer(const Config& config);
    ~SchedulerServer();

    // gRPC service methods - CRUD operations
    grpc::Status create_job(grpc::ServerContext* context,
                           const swdv::ifex_scheduler::create_job_request* request,
                           swdv::ifex_scheduler::create_job_response* response) override;

    grpc::Status list_jobs(grpc::ServerContext* context,
                          const swdv::ifex_scheduler::list_jobs_request* request,
                          swdv::ifex_scheduler::list_jobs_response* response) override;

    grpc::Status list_jobs_hash(grpc::ServerContext* context,
                               const swdv::ifex_scheduler::list_jobs_hash_request* request,
                               swdv::ifex_scheduler::list_jobs_hash_response* response) override;

    grpc::Status get_job(grpc::ServerContext* context,
                        const swdv::ifex_scheduler::get_job_request* request,
                        swdv::ifex_scheduler::get_job_response* response) override;

    grpc::Status update_job(grpc::ServerContext* context,
                           const swdv::ifex_scheduler::update_job_request* request,
                           swdv::ifex_scheduler::update_job_response* response) override;

    grpc::Status delete_job(grpc::ServerContext* context,
                           const swdv::ifex_scheduler::delete_job_request* request,
                           swdv::ifex_scheduler::delete_job_response* response) override;

    grpc::Status pause_job(grpc::ServerContext* context,
                          const swdv::ifex_scheduler::pause_job_request* request,
                          swdv::ifex_scheduler::pause_job_response* response) override;

    grpc::Status resume_job(grpc::ServerContext* context,
                           const swdv::ifex_scheduler::resume_job_request* request,
                           swdv::ifex_scheduler::resume_job_response* response) override;

    grpc::Status trigger_job(grpc::ServerContext* context,
                            const swdv::ifex_scheduler::trigger_job_request* request,
                            swdv::ifex_scheduler::trigger_job_response* response) override;

    // Execution history methods
    grpc::Status list_executions(grpc::ServerContext* context,
                                const swdv::ifex_scheduler::list_executions_request* request,
                                swdv::ifex_scheduler::list_executions_response* response) override;

    grpc::Status list_executions_hash(grpc::ServerContext* context,
                                     const swdv::ifex_scheduler::list_executions_hash_request* request,
                                     swdv::ifex_scheduler::list_executions_hash_response* response) override;

    // Service lifecycle
    void StartExecutor();
    void StopExecutor();
    bool RegisterWithDiscovery(int port, const std::string& ifex_schema);

    // Persistence - call during graceful shutdown
    void PersistJobs();

    // Check if running
    bool is_running() const { return running_; }

private:
    // Job storage
    std::unordered_map<std::string, std::unique_ptr<Job>> jobs_;
    std::mutex jobs_mutex_;

    // Execution history storage: job_id -> list of executions (newest first)
    std::unordered_map<std::string, std::vector<swdv::ifex_scheduler::execution_t>> executions_;
    mutable std::mutex executions_mutex_;

    // Job ID counter
    std::atomic<uint64_t> job_counter_{0};

    // Job executor
    std::thread executor_thread_;
    std::atomic<bool> running_{false};

    // Service discovery client (using ifex-core API)
    std::unique_ptr<ifex::DiscoveryClient> discovery_client_;
    std::unique_ptr<swdv::ifex_dispatcher::call_method_service::Stub> dispatcher_stub_;

    // Registration info
    std::string registration_id_;
    std::string discovery_endpoint_;

    // Persistence
    std::string persistence_dir_;
    std::string GetPersistenceFilePath() const;
    void SaveJobs();
    void LoadJobs();

    // Generate unique job ID
    std::string GenerateJobId();

    // Execute jobs in background
    void JobExecutor();

    // Execute a single job
    void ExecuteJob(Job* job);

    // Call service method using dispatcher
    bool CallServiceMethod(Job* job);

    // Calculate next run time for recurring jobs
    std::optional<std::chrono::system_clock::time_point> CalculateNextRunTime(
        const Job& job,
        const std::chrono::system_clock::time_point& after_time);

    // Parse ISO 8601 datetime
    std::chrono::system_clock::time_point ParseISO8601(const std::string& datetime);

    // Format datetime as ISO 8601
    std::string FormatISO8601(const std::chrono::system_clock::time_point& time);

    // Apply job filter
    bool MatchesFilter(const Job& job, const swdv::ifex_scheduler::job_filter_t& filter);
};

} // namespace ifex::reference
