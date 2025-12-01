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

#include "ifex-scheduler-service.grpc.pb.h"
#include "ifex-dispatcher-service.grpc.pb.h"
#include <ifex/discovery.hpp>

namespace ifex::reference {

using json = nlohmann::json;

// Internal job representation
struct Job {
    std::string id;
    std::string title;
    std::string service_name;
    std::string method_name;
    json parameters;
    std::string service_address;

    // Scheduling
    std::chrono::system_clock::time_point scheduled_time;
    std::string recurrence_rule;  // Cron expression or empty
    std::optional<std::chrono::system_clock::time_point> end_time;
    std::optional<std::chrono::system_clock::time_point> next_run_time;

    // Status tracking
    swdv::ifex_scheduler::job_status_t status = swdv::ifex_scheduler::PENDING;

    // Timestamps
    std::chrono::system_clock::time_point created_at;
    std::chrono::system_clock::time_point updated_at;
    std::optional<std::chrono::system_clock::time_point> executed_at;
    std::optional<std::string> error_message;
    std::optional<std::string> result;  // Response from service call

    // Convert to protobuf message
    void ToProto(swdv::ifex_scheduler::job_t* proto) const;

    // Create from protobuf message
    static std::unique_ptr<Job> FromProto(const swdv::ifex_scheduler::job_create_t& proto);
};

class SchedulerServer final : public swdv::ifex_scheduler::create_job_service::Service,
                              public swdv::ifex_scheduler::get_jobs_service::Service,
                              public swdv::ifex_scheduler::get_job_service::Service,
                              public swdv::ifex_scheduler::update_job_service::Service,
                              public swdv::ifex_scheduler::delete_job_service::Service,
                              public swdv::ifex_scheduler::get_calendar_view_service::Service {
public:
    explicit SchedulerServer(const std::string& service_discovery_endpoint);
    ~SchedulerServer();

    // gRPC service methods - CRUD operations
    grpc::Status create_job(grpc::ServerContext* context,
                           const swdv::ifex_scheduler::create_job_request* request,
                           swdv::ifex_scheduler::create_job_response* response) override;

    grpc::Status get_jobs(grpc::ServerContext* context,
                         const swdv::ifex_scheduler::get_jobs_request* request,
                         swdv::ifex_scheduler::get_jobs_response* response) override;

    grpc::Status get_job(grpc::ServerContext* context,
                        const swdv::ifex_scheduler::get_job_request* request,
                        swdv::ifex_scheduler::get_job_response* response) override;

    grpc::Status update_job(grpc::ServerContext* context,
                           const swdv::ifex_scheduler::update_job_request* request,
                           swdv::ifex_scheduler::update_job_response* response) override;

    grpc::Status delete_job(grpc::ServerContext* context,
                           const swdv::ifex_scheduler::delete_job_request* request,
                           swdv::ifex_scheduler::delete_job_response* response) override;

    grpc::Status get_calendar_view(grpc::ServerContext* context,
                                  const swdv::ifex_scheduler::get_calendar_view_request* request,
                                  swdv::ifex_scheduler::get_calendar_view_response* response) override;

    // Service lifecycle
    void StartExecutor();
    void StopExecutor();
    bool RegisterWithDiscovery(int port, const std::string& ifex_schema);

    // Check if running
    bool is_running() const { return running_; }

private:
    // Job storage
    std::unordered_map<std::string, std::unique_ptr<Job>> jobs_;
    std::mutex jobs_mutex_;

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

    // Get date range for calendar view
    std::pair<std::chrono::system_clock::time_point, std::chrono::system_clock::time_point>
    GetCalendarViewRange(swdv::ifex_scheduler::view_type_t view_type, const std::string& date);
};

} // namespace ifex::reference
