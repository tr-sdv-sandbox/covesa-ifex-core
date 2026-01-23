#include "cloud_scheduler_service.hpp"
#include "time_utils.hpp"

#include <algorithm>
#include <chrono>
#include <functional>
#include <iomanip>
#include <random>
#include <set>
#include <sstream>

namespace ifex::cloud {

namespace sync_v2 = swdv::scheduler_sync_v2;
namespace scheduler_types_pb = swdv::scheduler_types;
namespace sched_lib = ifex::scheduler;

// =============================================================================
// Helper: Convert job_info_t to library Job for hash computation
// =============================================================================

static sched_lib::Job JobInfoToLibraryJob(const sched::job_info_t& job) {
    sched_lib::Job lib_job;
    lib_job.job_id = job.job_id();
    lib_job.title = job.title();
    lib_job.service = job.service();
    lib_job.method = job.method();
    lib_job.parameters_json = job.parameters_json();
    lib_job.scheduled_time_ms = job.scheduled_time_ms();
    lib_job.recurrence_rule = job.recurrence_rule();
    lib_job.end_time_ms = job.end_time_ms();
    lib_job.paused = job.paused();
    lib_job.wake_policy = static_cast<sched_lib::WakePolicy>(job.wake_policy());
    lib_job.sleep_policy = static_cast<sched_lib::SleepPolicy>(job.sleep_policy());
    lib_job.wake_lead_time_s = job.wake_lead_time_s();
    lib_job.status = static_cast<sched_lib::JobStatus>(job.status());
    lib_job.authority = static_cast<sched_lib::JobAuthority>(job.authority());
    lib_job.deleted = job.deleted();
    return lib_job;
}

// Map IFEX job status to sync v2 job status
// Note: Cloud-specific statuses map to closest common status
static sync_v2::JobStatus JobStatusToSyncV2(sched::cloud_job_status_t status) {
    switch (status) {
        case sched::JOB_PENDING: return sync_v2::JOB_STATUS_PENDING;
        case sched::JOB_SCHEDULED: return sync_v2::JOB_STATUS_PENDING;  // Cloud-specific, map to pending
        case sched::JOB_RUNNING: return sync_v2::JOB_STATUS_RUNNING;
        case sched::JOB_COMPLETED: return sync_v2::JOB_STATUS_COMPLETED;
        case sched::JOB_FAILED: return sync_v2::JOB_STATUS_FAILED;
        case sched::JOB_CANCELLED: return sync_v2::JOB_STATUS_CANCELLED;
        case sched::JOB_PAUSED: return sync_v2::JOB_STATUS_PENDING;  // Cloud-specific, map to pending
        case sched::JOB_DELETING: return sync_v2::JOB_STATUS_CANCELLED;  // Cloud-specific, map to cancelled
        default: return sync_v2::JOB_STATUS_PENDING;
    }
}

// Map sync v2 job status to IFEX job status
static sched::cloud_job_status_t SyncV2ToJobStatus(sync_v2::JobStatus status) {
    switch (status) {
        case sync_v2::JOB_STATUS_PENDING: return sched::JOB_PENDING;
        case sync_v2::JOB_STATUS_RUNNING: return sched::JOB_RUNNING;
        case sync_v2::JOB_STATUS_COMPLETED: return sched::JOB_COMPLETED;
        case sync_v2::JOB_STATUS_FAILED: return sched::JOB_FAILED;
        case sync_v2::JOB_STATUS_CANCELLED: return sched::JOB_CANCELLED;
        default: return sched::JOB_PENDING;
    }
}

// Map IFEX authority to sync v2 authority
static sync_v2::JobAuthority AuthorityToSyncV2(scheduler_types_pb::job_authority_t auth) {
    return (auth == scheduler_types_pb::AUTHORITY_CLOUD) ? sync_v2::AUTHORITY_CLOUD : sync_v2::AUTHORITY_VEHICLE;
}

// Map sync v2 authority to IFEX authority
static scheduler_types_pb::job_authority_t SyncV2ToAuthority(sync_v2::JobAuthority auth) {
    return (auth == sync_v2::AUTHORITY_CLOUD) ? scheduler_types_pb::AUTHORITY_CLOUD : scheduler_types_pb::AUTHORITY_VEHICLE;
}

// Map IFEX wake policy to sync v2
static sync_v2::WakePolicy WakePolicyToSyncV2(scheduler_types_pb::wake_policy_t policy) {
    return (policy == scheduler_types_pb::WAKE_REQUIRED) ? sync_v2::WAKE_REQUIRED : sync_v2::WAKE_NO_WAKE;
}

// Map sync v2 wake policy to IFEX
static scheduler_types_pb::wake_policy_t SyncV2ToWakePolicy(sync_v2::WakePolicy policy) {
    return (policy == sync_v2::WAKE_REQUIRED) ? scheduler_types_pb::WAKE_REQUIRED : scheduler_types_pb::WAKE_NO_WAKE;
}

// Map IFEX sleep policy to sync v2
static sync_v2::SleepPolicy SleepPolicyToSyncV2(scheduler_types_pb::sleep_policy_t policy) {
    return (policy == scheduler_types_pb::SLEEP_INHIBIT) ? sync_v2::SLEEP_INHIBIT : sync_v2::SLEEP_NORMAL;
}

// Map sync v2 sleep policy to IFEX
static scheduler_types_pb::sleep_policy_t SyncV2ToSleepPolicy(sync_v2::SleepPolicy policy) {
    return (policy == sync_v2::SLEEP_INHIBIT) ? scheduler_types_pb::SLEEP_INHIBIT : scheduler_types_pb::SLEEP_NORMAL;
}

CloudSchedulerService::CloudSchedulerService(const CloudSchedulerServiceConfig& config)
    : config_(config) {}

CloudSchedulerService::~CloudSchedulerService() {
    Stop();
}

bool CloudSchedulerService::Start() {
    if (running_) {
        return true;
    }

    LOG(INFO) << "Starting CloudSchedulerService, backend=" << config_.backend_transport_address;

    // Create transport client
    transport_ = std::make_unique<CloudBackendTransportClient>(config_.backend_transport_address);

    // Subscribe to vehicle messages (scheduler sync from v2c)
    transport_->SubscribeToVehicleMessages(
        [this](const std::string& vehicle_id,
               const std::vector<uint8_t>& payload,
               uint64_t sequence,
               int64_t timestamp_ms) {
            HandleSyncMessage(vehicle_id, payload);
        });

    running_ = true;
    LOG(INFO) << "CloudSchedulerService started";
    return true;
}

void CloudSchedulerService::Stop() {
    if (!running_) {
        return;
    }

    LOG(INFO) << "Stopping CloudSchedulerService";
    running_ = false;

    if (transport_) {
        transport_->StopSubscriptions();
        transport_.reset();
    }

    LOG(INFO) << "CloudSchedulerService stopped";
}

void CloudSchedulerService::RegisterServices(grpc::ServerBuilder& builder) {
    builder.RegisterService(static_cast<sched::create_job_service::Service*>(this));
    builder.RegisterService(static_cast<sched::update_job_service::Service*>(this));
    builder.RegisterService(static_cast<sched::delete_job_service::Service*>(this));
    builder.RegisterService(static_cast<sched::pause_job_service::Service*>(this));
    builder.RegisterService(static_cast<sched::resume_job_service::Service*>(this));
    builder.RegisterService(static_cast<sched::trigger_job_service::Service*>(this));
    builder.RegisterService(static_cast<sched::get_job_service::Service*>(this));
    builder.RegisterService(static_cast<sched::list_jobs_service::Service*>(this));
    builder.RegisterService(static_cast<sched::get_job_executions_service::Service*>(this));
    builder.RegisterService(static_cast<sched::create_fleet_job_service::Service*>(this));
    builder.RegisterService(static_cast<sched::delete_fleet_job_service::Service*>(this));
    builder.RegisterService(static_cast<sched::get_fleet_job_stats_service::Service*>(this));
    builder.RegisterService(static_cast<sched::healthy_service::Service*>(this));
}

std::string CloudSchedulerService::GenerateJobId() {
    uint64_t counter = job_counter_++;
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dist;

    std::stringstream ss;
    ss << "job-" << std::hex << std::setfill('0') << std::setw(8) << counter
       << "-" << std::setw(8) << (dist(gen) & 0xFFFFFFFF);
    return ss.str();
}

uint64_t CloudSchedulerService::Iso8601ToEpochMs(const std::string& iso_str) {
    if (iso_str.empty()) return 0;

    std::tm tm = {};
    std::istringstream ss(iso_str);
    ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    if (ss.fail()) return 0;

    time_t epoch_sec = timegm(&tm);

    // Parse optional milliseconds
    uint64_t ms = 0;
    char c;
    if (ss >> c && c == '.') {
        int frac;
        if (ss >> frac) {
            // Normalize to 3 digits
            std::string frac_str = std::to_string(frac);
            while (frac_str.length() < 3) frac_str += "0";
            ms = std::stoull(frac_str.substr(0, 3));
        }
    }

    return static_cast<uint64_t>(epoch_sec) * 1000 + ms;
}

std::string CloudSchedulerService::EpochMsToIso8601(uint64_t epoch_ms) {
    if (epoch_ms == 0) return "";

    auto seconds = static_cast<time_t>(epoch_ms / 1000);
    auto ms_remainder = epoch_ms % 1000;

    std::tm tm_utc;
    gmtime_r(&seconds, &tm_utc);

    std::ostringstream oss;
    oss << std::put_time(&tm_utc, "%Y-%m-%dT%H:%M:%S");
    if (ms_remainder > 0) {
        oss << '.' << std::setfill('0') << std::setw(3) << ms_remainder;
    }
    oss << 'Z';
    return oss.str();
}

void CloudSchedulerService::SendPendingJobsToVehicle(const std::string& vehicle_id) {
    // Wrapper for SendV2SyncMessage with just the pending jobs
    SendV2SyncMessage(vehicle_id);
}

bool CloudSchedulerService::SendTriggerJobRequest(
    const std::string& vehicle_id,
    const std::string& job_id,
    const std::string& requester_id) {

    if (!transport_) {
        LOG(ERROR) << "Transport not initialized";
        return false;
    }

    sync_v2::TriggerJobRequest trigger;
    trigger.set_job_id(job_id);
    trigger.set_requester_id(requester_id);
    trigger.set_timestamp_ms(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    // No expiry by default

    std::string serialized;
    if (!trigger.SerializeToString(&serialized)) {
        LOG(ERROR) << "Failed to serialize TriggerJobRequest";
        return false;
    }

    std::vector<uint8_t> payload(serialized.begin(), serialized.end());
    auto result = transport_->SendToVehicle(
        vehicle_id, payload,
        swdv::cloud_backend_transport_service::persistence_t::VOLATILE);

    if (result.status() != swdv::cloud_backend_transport_service::publish_status_t::OK) {
        LOG(ERROR) << "Failed to send TriggerJobRequest to " << vehicle_id
                   << ": status=" << static_cast<int>(result.status());
        return false;
    }

    LOG(INFO) << "Sent TriggerJobRequest for job " << job_id << " to " << vehicle_id;
    return true;
}

void CloudSchedulerService::HandleSyncMessage(
    const std::string& vehicle_id,
    const std::vector<uint8_t>& payload) {

    sync_v2::V2C_SyncMessage msg;
    if (!msg.ParseFromArray(payload.data(), payload.size())) {
        LOG(WARNING) << "Failed to parse v2 sync message from " << vehicle_id;
        return;
    }

    HandleV2SyncMessage(vehicle_id, msg);
}

// =========================================================================
// gRPC Methods
// =========================================================================

grpc::Status CloudSchedulerService::create_job(
    grpc::ServerContext* context,
    const sched::create_job_request* request,
    sched::create_job_response* response) {

    auto* result = response->mutable_result();
    const auto& req = request->request();

    if (req.vehicle_id().empty()) {
        result->set_success(false);
        result->set_error_message("vehicle_id is required");
        return grpc::Status::OK;
    }

    if (req.service().empty() || req.method().empty()) {
        result->set_success(false);
        result->set_error_message("service and method are required");
        return grpc::Status::OK;
    }

    std::string job_id = GenerateJobId();
    uint64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // Store job locally with pending sync state
    {
        std::lock_guard<std::mutex> lock(jobs_mutex_);
        sched::job_info_t job;
        job.set_vehicle_id(req.vehicle_id());
        job.set_job_id(job_id);
        job.set_title(req.title());
        job.set_service(req.service());
        job.set_method(req.method());
        job.set_parameters_json(req.parameters_json());
        job.set_scheduled_time_ms(req.scheduled_time_ms());
        job.set_recurrence_rule(req.recurrence_rule());
        job.set_end_time_ms(req.end_time_ms());
        job.set_status(sched::JOB_PENDING);
        job.set_created_at_ms(now_ms);
        job.set_updated_at_ms(now_ms);
        job.set_created_by(req.created_by());
        job.set_paused(req.paused());
        job.set_authority(scheduler_types_pb::AUTHORITY_CLOUD);  // Cloud-created jobs
        job.set_wake_policy(req.wake_policy());
        job.set_sleep_policy(req.sleep_policy());
        job.set_wake_lead_time_s(req.wake_lead_time_s());
        jobs_[req.vehicle_id()][job_id] = job;

        // Initialize v2 sync state for cloud-created job
        job_versions_[req.vehicle_id()][job_id] = sched_lib::VersionVector{1, 0};
        job_sync_states_[req.vehicle_id()][job_id] = scheduler_types_pb::SYNC_PENDING;
    }

    // Send C2V_SyncMessage with the new job to the vehicle
    SendPendingJobsToVehicle(req.vehicle_id());

    result->set_success(true);
    result->set_job_id(job_id);
    LOG(INFO) << "Created job " << job_id << " on " << req.vehicle_id();
    return grpc::Status::OK;
}

grpc::Status CloudSchedulerService::update_job(
    grpc::ServerContext* context,
    const sched::update_job_request* request,
    sched::update_job_response* response) {

    auto* result = response->mutable_result();
    const auto& req = request->request();

    if (req.vehicle_id().empty() || req.job_id().empty()) {
        result->set_success(false);
        result->set_error_message("vehicle_id and job_id are required");
        return grpc::Status::OK;
    }

    uint64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // Update job locally and mark as pending sync
    {
        std::lock_guard<std::mutex> lock(jobs_mutex_);
        auto& vehicle_jobs = jobs_[req.vehicle_id()];
        auto it = vehicle_jobs.find(req.job_id());
        if (it == vehicle_jobs.end()) {
            result->set_success(false);
            result->set_error_message("Job not found");
            return grpc::Status::OK;
        }

        // Update job fields (use has_ flags for optional fields)
        auto& job = it->second;
        if (!req.title().empty()) job.set_title(req.title());
        if (req.scheduled_time_ms() > 0) job.set_scheduled_time_ms(req.scheduled_time_ms());
        if (!req.recurrence_rule().empty()) job.set_recurrence_rule(req.recurrence_rule());
        if (!req.parameters_json().empty()) job.set_parameters_json(req.parameters_json());
        if (req.end_time_ms() > 0) job.set_end_time_ms(req.end_time_ms());
        if (req.has_paused()) job.set_paused(req.paused());
        if (req.has_wake_policy()) job.set_wake_policy(req.wake_policy());
        if (req.has_sleep_policy()) job.set_sleep_policy(req.sleep_policy());
        if (req.has_wake_lead_time()) job.set_wake_lead_time_s(req.wake_lead_time_s());
        job.set_updated_at_ms(now_ms);

        // Increment version for cloud change
        auto& version = job_versions_[req.vehicle_id()][req.job_id()];
        version.increment_cloud();
        job_sync_states_[req.vehicle_id()][req.job_id()] = scheduler_types_pb::SYNC_PENDING;
    }

    // Send updated job state to vehicle
    SendPendingJobsToVehicle(req.vehicle_id());

    result->set_success(true);
    return grpc::Status::OK;
}

grpc::Status CloudSchedulerService::delete_job(
    grpc::ServerContext* context,
    const sched::delete_job_request* request,
    sched::delete_job_response* response) {

    auto* result = response->mutable_result();

    if (request->vehicle_id().empty() || request->job_id().empty()) {
        result->set_success(false);
        result->set_error_message("vehicle_id and job_id are required");
        return grpc::Status::OK;
    }

    uint64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // Mark job as deleted (tombstone) rather than removing it
    // The sync protocol sends deleted=true in the JobRecord
    {
        std::lock_guard<std::mutex> lock(jobs_mutex_);
        auto& vehicle_jobs = jobs_[request->vehicle_id()];
        auto it = vehicle_jobs.find(request->job_id());
        if (it != vehicle_jobs.end()) {
            it->second.set_deleted(true);
            it->second.set_updated_at_ms(now_ms);

            // Increment version for cloud change
            auto& version = job_versions_[request->vehicle_id()][request->job_id()];
            version.increment_cloud();
            job_sync_states_[request->vehicle_id()][request->job_id()] = scheduler_types_pb::SYNC_PENDING;
        }
    }

    // Send sync message with tombstone
    SendPendingJobsToVehicle(request->vehicle_id());

    LOG(INFO) << "Marked job " << request->job_id() << " as deleted for "
              << request->vehicle_id();

    result->set_success(true);
    return grpc::Status::OK;
}

grpc::Status CloudSchedulerService::pause_job(
    grpc::ServerContext* context,
    const sched::pause_job_request* request,
    sched::pause_job_response* response) {

    auto* result = response->mutable_result();

    if (request->vehicle_id().empty() || request->job_id().empty()) {
        result->set_success(false);
        result->set_error_message("vehicle_id and job_id are required");
        return grpc::Status::OK;
    }

    uint64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // Update job paused state locally and mark for sync
    {
        std::lock_guard<std::mutex> lock(jobs_mutex_);
        auto& vehicle_jobs = jobs_[request->vehicle_id()];
        auto it = vehicle_jobs.find(request->job_id());
        if (it == vehicle_jobs.end()) {
            result->set_success(false);
            result->set_error_message("Job not found");
            return grpc::Status::OK;
        }

        it->second.set_paused(true);
        it->second.set_updated_at_ms(now_ms);

        // Increment version for cloud change
        auto& version = job_versions_[request->vehicle_id()][request->job_id()];
        version.increment_cloud();
        job_sync_states_[request->vehicle_id()][request->job_id()] = scheduler_types_pb::SYNC_PENDING;
    }

    // Send updated state to vehicle
    SendPendingJobsToVehicle(request->vehicle_id());

    result->set_success(true);
    return grpc::Status::OK;
}

grpc::Status CloudSchedulerService::resume_job(
    grpc::ServerContext* context,
    const sched::resume_job_request* request,
    sched::resume_job_response* response) {

    auto* result = response->mutable_result();

    if (request->vehicle_id().empty() || request->job_id().empty()) {
        result->set_success(false);
        result->set_error_message("vehicle_id and job_id are required");
        return grpc::Status::OK;
    }

    uint64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // Update job paused state locally and mark for sync
    {
        std::lock_guard<std::mutex> lock(jobs_mutex_);
        auto& vehicle_jobs = jobs_[request->vehicle_id()];
        auto it = vehicle_jobs.find(request->job_id());
        if (it == vehicle_jobs.end()) {
            result->set_success(false);
            result->set_error_message("Job not found");
            return grpc::Status::OK;
        }

        it->second.set_paused(false);
        it->second.set_updated_at_ms(now_ms);

        // Increment version for cloud change
        auto& version = job_versions_[request->vehicle_id()][request->job_id()];
        version.increment_cloud();
        job_sync_states_[request->vehicle_id()][request->job_id()] = scheduler_types_pb::SYNC_PENDING;
    }

    // Send updated state to vehicle
    SendPendingJobsToVehicle(request->vehicle_id());

    result->set_success(true);
    return grpc::Status::OK;
}

grpc::Status CloudSchedulerService::trigger_job(
    grpc::ServerContext* context,
    const sched::trigger_job_request* request,
    sched::trigger_job_response* response) {

    auto* result = response->mutable_result();

    if (request->vehicle_id().empty() || request->job_id().empty()) {
        result->set_success(false);
        result->set_error_message("vehicle_id and job_id are required");
        return grpc::Status::OK;
    }

    // TriggerJob is the only imperative command - send TriggerJobRequest
    if (!SendTriggerJobRequest(request->vehicle_id(), request->job_id(), "cloud-scheduler")) {
        result->set_success(false);
        result->set_error_message("Failed to send trigger request to vehicle");
        return grpc::Status::OK;
    }

    result->set_success(true);
    return grpc::Status::OK;
}

grpc::Status CloudSchedulerService::get_job(
    grpc::ServerContext* context,
    const sched::get_job_request* request,
    sched::get_job_response* response) {

    auto* result = response->mutable_result();
    std::lock_guard<std::mutex> lock(jobs_mutex_);

    auto vehicle_it = jobs_.find(request->vehicle_id());
    if (vehicle_it == jobs_.end()) {
        result->set_found(false);
        return grpc::Status::OK;
    }

    auto job_it = vehicle_it->second.find(request->job_id());
    if (job_it == vehicle_it->second.end()) {
        result->set_found(false);
        return grpc::Status::OK;
    }

    // Tombstones (deleted jobs) should not be found via get_job
    if (job_it->second.deleted()) {
        result->set_found(false);
        return grpc::Status::OK;
    }

    result->set_found(true);
    *result->mutable_job() = job_it->second;
    return grpc::Status::OK;
}

grpc::Status CloudSchedulerService::list_jobs(
    grpc::ServerContext* context,
    const sched::list_jobs_request* request,
    sched::list_jobs_response* response) {

    auto* result = response->mutable_result();
    const auto& filter = request->filter();
    std::lock_guard<std::mutex> lock(jobs_mutex_);

    int count = 0;
    for (const auto& [vehicle_id, vehicle_jobs] : jobs_) {
        // Apply vehicle filter
        if (!filter.vehicle_id_filter().empty() &&
            vehicle_id != filter.vehicle_id_filter()) {
            continue;
        }

        for (const auto& [job_id, job] : vehicle_jobs) {
            // Filter out deleted jobs unless include_deleted is true
            if (job.deleted() && !filter.include_deleted()) {
                continue;
            }

            // Apply service filter
            if (!filter.service_filter().empty() &&
                job.service() != filter.service_filter()) {
                continue;
            }

            // Apply paused_only filter
            if (filter.paused_only() && !job.paused()) {
                continue;
            }

            *result->add_jobs() = job;
            count++;
        }
    }

    result->set_total_count(count);
    return grpc::Status::OK;
}

grpc::Status CloudSchedulerService::get_job_executions(
    grpc::ServerContext* context,
    const sched::get_job_executions_request* request,
    sched::get_job_executions_response* response) {

    auto* result = response->mutable_result();
    const auto& req = request->request();
    std::lock_guard<std::mutex> lock(executions_mutex_);

    result->set_vehicle_id(req.vehicle_id());
    result->set_job_id(req.job_id());

    auto vehicle_it = executions_.find(req.vehicle_id());
    if (vehicle_it == executions_.end()) {
        result->set_total_count(0);
        return grpc::Status::OK;
    }

    auto job_it = vehicle_it->second.find(req.job_id());
    if (job_it == vehicle_it->second.end()) {
        result->set_total_count(0);
        return grpc::Status::OK;
    }

    int limit = req.limit() > 0 ? req.limit() : 100;
    int count = 0;
    for (const auto& exec : job_it->second) {
        if (req.since_ms() > 0 && exec.executed_at_ms() < req.since_ms()) {
            continue;
        }
        *result->add_executions() = exec;
        if (++count >= limit) break;
    }

    result->set_total_count(static_cast<int>(job_it->second.size()));
    return grpc::Status::OK;
}

// Fleet operations - simplified stubs for testing
grpc::Status CloudSchedulerService::create_fleet_job(
    grpc::ServerContext* context,
    const sched::create_fleet_job_request* request,
    sched::create_fleet_job_response* response) {

    auto* result = response->mutable_result();
    const auto& req = request->request();

    // For testing, just create jobs on each specified vehicle
    int successful = 0;
    int failed = 0;

    for (const auto& vehicle_id : req.vehicle_ids()) {
        sched::create_job_request single_request;
        auto* single_req = single_request.mutable_request();
        single_req->set_vehicle_id(vehicle_id);
        single_req->set_title(req.title());
        single_req->set_service(req.service());
        single_req->set_method(req.method());
        single_req->set_parameters_json(req.parameters_json());
        single_req->set_scheduled_time_ms(req.scheduled_time_ms());
        single_req->set_recurrence_rule(req.recurrence_rule());
        single_req->set_end_time_ms(req.end_time_ms());
        single_req->set_created_by(req.created_by());

        sched::create_job_response single_response;
        create_job(context, &single_request, &single_response);

        auto* fleet_result = result->add_results();
        fleet_result->set_vehicle_id(vehicle_id);
        fleet_result->set_success(single_response.result().success());
        fleet_result->set_job_id(single_response.result().job_id());
        fleet_result->set_error_message(single_response.result().error_message());

        if (single_response.result().success()) {
            successful++;
        } else {
            failed++;
        }
    }

    result->set_total_vehicles(req.vehicle_ids_size());
    result->set_successful(successful);
    result->set_failed(failed);
    result->set_fleet_job_id(GenerateJobId());

    return grpc::Status::OK;
}

grpc::Status CloudSchedulerService::delete_fleet_job(
    grpc::ServerContext* context,
    const sched::delete_fleet_job_request* request,
    sched::delete_fleet_job_response* response) {

    auto* result = response->mutable_result();
    const auto& req = request->request();

    int successful = 0;
    int failed = 0;

    for (const auto& vehicle_id : req.vehicle_ids()) {
        for (const auto& job_id : req.job_ids()) {
            sched::delete_job_request single_request;
            single_request.set_vehicle_id(vehicle_id);
            single_request.set_job_id(job_id);

            sched::delete_job_response single_response;
            delete_job(context, &single_request, &single_response);

            if (single_response.result().success()) {
                successful++;
            } else {
                failed++;
            }
        }
    }

    result->set_total_deletions(req.vehicle_ids_size() * req.job_ids_size());
    result->set_successful(successful);
    result->set_failed(failed);

    return grpc::Status::OK;
}

grpc::Status CloudSchedulerService::get_fleet_job_stats(
    grpc::ServerContext* context,
    const sched::get_fleet_job_stats_request* request,
    sched::get_fleet_job_stats_response* response) {

    auto* result = response->mutable_result();
    std::lock_guard<std::mutex> lock(jobs_mutex_);

    int total_jobs = 0;
    std::set<std::string> vehicles_with_jobs;
    std::map<int, int> by_status;

    for (const auto& [vehicle_id, vehicle_jobs] : jobs_) {
        if (!vehicle_jobs.empty()) {
            vehicles_with_jobs.insert(vehicle_id);
        }
        for (const auto& [job_id, job] : vehicle_jobs) {
            total_jobs++;
            by_status[static_cast<int>(job.status())]++;
        }
    }

    result->set_total_jobs(total_jobs);
    result->set_total_vehicles_with_jobs(static_cast<int>(vehicles_with_jobs.size()));

    for (const auto& [status, count] : by_status) {
        auto* status_count = result->add_by_status();
        status_count->set_status(std::to_string(status));
        status_count->set_count(count);
    }

    return grpc::Status::OK;
}

grpc::Status CloudSchedulerService::healthy(
    grpc::ServerContext* context,
    const sched::healthy_request* request,
    sched::healthy_response* response) {

    response->set_is_healthy(running_);
    return grpc::Status::OK;
}

// =========================================================================
// Test Helpers
// =========================================================================

size_t CloudSchedulerService::GetJobCount(const std::string& vehicle_id) const {
    std::lock_guard<std::mutex> lock(jobs_mutex_);
    auto it = jobs_.find(vehicle_id);
    if (it == jobs_.end()) return 0;

    // Count only non-deleted jobs (exclude tombstones)
    size_t count = 0;
    for (const auto& [job_id, job] : it->second) {
        if (!job.deleted()) {
            count++;
        }
    }
    return count;
}

size_t CloudSchedulerService::GetTotalJobCount() const {
    std::lock_guard<std::mutex> lock(jobs_mutex_);
    size_t total = 0;
    for (const auto& [vehicle_id, jobs] : jobs_) {
        // Count only non-deleted jobs (exclude tombstones)
        for (const auto& [job_id, job] : jobs) {
            if (!job.deleted()) {
                total++;
            }
        }
    }
    return total;
}

void CloudSchedulerService::ClearAllJobs() {
    std::lock_guard<std::mutex> lock(jobs_mutex_);
    jobs_.clear();
    job_versions_.clear();
    job_sync_states_.clear();
    std::lock_guard<std::mutex> exec_lock(executions_mutex_);
    executions_.clear();
}

// =========================================================================
// Sync Protocol v2 Methods
// =========================================================================

uint64_t CloudSchedulerService::ComputeJobHash(const sched::job_info_t& job) {
    // Use centralized hash computation from ifex-scheduler library
    return sched_lib::compute_job_content_hash(JobInfoToLibraryJob(job));
}

uint64_t CloudSchedulerService::ComputeStateChecksum(const std::string& vehicle_id) const {
    std::lock_guard<std::mutex> lock(jobs_mutex_);

    auto vehicle_it = jobs_.find(vehicle_id);
    if (vehicle_it == jobs_.end()) {
        // Use library's empty state checksum (seed value)
        return sched_lib::compute_state_checksum({});
    }

    // Convert to library Job format and sort by job_id
    std::vector<sched_lib::Job> lib_jobs;
    lib_jobs.reserve(vehicle_it->second.size());

    for (const auto& [id, job] : vehicle_it->second) {
        lib_jobs.push_back(JobInfoToLibraryJob(job));
    }

    // Sort by job_id for deterministic ordering (required by library)
    std::sort(lib_jobs.begin(), lib_jobs.end(),
              [](const sched_lib::Job& a, const sched_lib::Job& b) {
                  return a.job_id < b.job_id;
              });

    return sched_lib::compute_state_checksum(lib_jobs);
}

void CloudSchedulerService::HandleV2SyncMessage(
    const std::string& vehicle_id,
    const sync_v2::V2C_SyncMessage& msg) {

    LOG(INFO) << "Received v2 sync message from " << vehicle_id
              << " jobs=" << msg.jobs_size()
              << " executions=" << msg.executions_size()
              << " state_checksum=" << msg.state_checksum();

    bool need_push = false;

    // Scope the mutex to avoid deadlock with SendV2SyncMessage
    {
        std::lock_guard<std::mutex> lock(jobs_mutex_);

        // Process each job record using sync engine
        // (deleted jobs have deleted=true in the JobRecord itself)
        for (const auto& job_record : msg.jobs()) {
            ProcessVehicleJob(vehicle_id, job_record);
        }

        // Process execution records (append-only)
        ProcessVehicleExecutions(vehicle_id, msg.executions());

        // Check if we need to send back any cloud changes
        // (jobs with higher cloud sequence that vehicle hasn't seen)
        for (const auto& [job_id, version] : job_versions_[vehicle_id]) {
            auto sync_state_it = job_sync_states_[vehicle_id].find(job_id);
            if (sync_state_it != job_sync_states_[vehicle_id].end() &&
                sync_state_it->second == scheduler_types_pb::SYNC_PENDING) {
                need_push = true;
                break;
            }
        }
    }  // Release mutex before calling SendV2SyncMessage

    if (need_push) {
        SendV2SyncMessage(vehicle_id);
    }
}

void CloudSchedulerService::ProcessVehicleJob(
    const std::string& vehicle_id,
    const sync_v2::JobRecord& record) {

    // Convert remote version
    sched_lib::VersionVector remote_version{
        record.version().cloud_seq(),
        record.version().vehicle_seq()
    };

    // Get local version if exists
    std::optional<sched_lib::VersionVector> local_version;
    auto& vehicle_versions = job_versions_[vehicle_id];
    auto version_it = vehicle_versions.find(record.job_id());
    if (version_it != vehicle_versions.end()) {
        local_version = version_it->second;
    }

    // Determine authority
    sched_lib::JobAuthority authority = (record.authority() == sync_v2::AUTHORITY_CLOUD)
        ? sched_lib::JobAuthority::CLOUD
        : sched_lib::JobAuthority::VEHICLE;

    // Use sync engine to determine action (cloud side)
    auto result = sched_lib::SyncEngine::process_remote(
        remote_version, local_version, authority, true /* is_cloud_side */);

    switch (result.action) {
        case sched_lib::SyncResult::ACCEPT_REMOTE: {
            // Accept vehicle's version
            sched::job_info_t job;
            RecordToJobInfo(record, &job);
            job.set_vehicle_id(vehicle_id);
            jobs_[vehicle_id][record.job_id()] = job;
            vehicle_versions[record.job_id()] = result.resolved_version;
            job_sync_states_[vehicle_id][record.job_id()] = scheduler_types_pb::SYNC_SYNCED;
            LOG(INFO) << "Accepted job " << record.job_id() << " from " << vehicle_id
                      << " version={" << result.resolved_version.cloud_seq << ","
                      << result.resolved_version.vehicle_seq << "}";
            break;
        }

        case sched_lib::SyncResult::REJECT_REMOTE: {
            // Keep our version, will push to vehicle in SendV2SyncMessage
            job_sync_states_[vehicle_id][record.job_id()] = scheduler_types_pb::SYNC_PENDING;
            LOG(INFO) << "Rejected job " << record.job_id() << " from " << vehicle_id
                      << " (keeping cloud version)";
            break;
        }

        case sched_lib::SyncResult::CONFLICT_RESOLVED: {
            // Conflict was already resolved by process_remote
            // result.winner tells us who won
            bool we_won = (result.winner == "cloud");
            if (!we_won) {
                // Vehicle wins, accept their version
                sched::job_info_t job;
                RecordToJobInfo(record, &job);
                job.set_vehicle_id(vehicle_id);
                jobs_[vehicle_id][record.job_id()] = job;
                vehicle_versions[record.job_id()] = result.resolved_version;
                job_sync_states_[vehicle_id][record.job_id()] = scheduler_types_pb::SYNC_SYNCED;
                LOG(INFO) << "Conflict resolved: accepted vehicle job " << record.job_id();
            } else {
                // Cloud wins, keep our version and push
                vehicle_versions[record.job_id()] = result.resolved_version;
                job_sync_states_[vehicle_id][record.job_id()] = scheduler_types_pb::SYNC_PENDING;
                LOG(INFO) << "Conflict resolved: keeping cloud job " << record.job_id();
            }
            break;
        }

        case sched_lib::SyncResult::NO_ACTION:
        default:
            // Already in sync
            break;
    }

    // Handle deleted jobs (tombstones)
    if (record.deleted()) {
        // Keep the tombstone record but mark as synced
        sched::job_info_t job;
        RecordToJobInfo(record, &job);
        job.set_vehicle_id(vehicle_id);
        job.set_deleted(true);
        jobs_[vehicle_id][record.job_id()] = job;
        vehicle_versions[record.job_id()] = result.resolved_version;
        job_sync_states_[vehicle_id][record.job_id()] = scheduler_types_pb::SYNC_SYNCED;
        LOG(INFO) << "Processed tombstone for job " << record.job_id() << " from " << vehicle_id;
    }
}

void CloudSchedulerService::ProcessVehicleExecutions(
    const std::string& vehicle_id,
    const google::protobuf::RepeatedPtrField<sync_v2::ExecutionRecord>& executions) {

    std::lock_guard<std::mutex> exec_lock(executions_mutex_);

    for (const auto& exec_record : executions) {
        sched::execution_info_t exec;
        exec.set_execution_id(exec_record.execution_id());
        exec.set_executed_at_ms(exec_record.executed_at_ms());
        exec.set_duration_ms(exec_record.duration_ms());
        exec.set_result_json(exec_record.result_json());
        exec.set_error_message(exec_record.error_message());

        // Map sync v2 status to IFEX status
        exec.set_status(SyncV2ToJobStatus(exec_record.status()));

        // Check for duplicate execution_id (append-only, no duplicates)
        auto& job_execs = executions_[vehicle_id][exec_record.job_id()];
        bool duplicate = false;
        for (const auto& existing : job_execs) {
            if (existing.execution_id() == exec_record.execution_id()) {
                duplicate = true;
                break;
            }
        }

        if (!duplicate) {
            job_execs.push_back(exec);
            LOG(INFO) << "Recorded execution " << exec_record.execution_id()
                      << " for job " << exec_record.job_id()
                      << " from " << vehicle_id;
        }
    }
}

void CloudSchedulerService::SendV2SyncMessage(const std::string& vehicle_id) {
    if (!transport_) {
        LOG(ERROR) << "Transport not initialized";
        return;
    }

    sync_v2::C2V_SyncMessage msg;
    msg.set_vehicle_id(vehicle_id);
    msg.set_sync_timestamp_ms(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    std::lock_guard<std::mutex> lock(jobs_mutex_);

    // Add jobs that need to be pushed to vehicle
    int pending_count = 0;
    for (const auto& [job_id, job] : jobs_[vehicle_id]) {
        auto sync_state_it = job_sync_states_[vehicle_id].find(job_id);
        if (sync_state_it != job_sync_states_[vehicle_id].end() &&
            sync_state_it->second == scheduler_types_pb::SYNC_PENDING) {

            auto* record = msg.add_jobs();
            auto version_it = job_versions_[vehicle_id].find(job_id);
            sched_lib::VersionVector version = (version_it != job_versions_[vehicle_id].end())
                ? version_it->second
                : sched_lib::VersionVector{1, 0};  // Default: cloud created

            JobInfoToRecord(job, version, record);
            pending_count++;
        }
    }

    if (pending_count == 0) {
        LOG(INFO) << "No pending jobs to sync to " << vehicle_id;
        return;
    }

    // Compute state checksum for quiescence detection using library function
    // (same algorithm as vehicle bridge for interoperability)
    std::vector<sched_lib::Job> lib_jobs;
    lib_jobs.reserve(jobs_[vehicle_id].size());
    for (const auto& [id, job] : jobs_[vehicle_id]) {
        lib_jobs.push_back(JobInfoToLibraryJob(job));
    }
    std::sort(lib_jobs.begin(), lib_jobs.end(),
              [](const sched_lib::Job& a, const sched_lib::Job& b) {
                  return a.job_id < b.job_id;
              });
    msg.set_state_checksum(sched_lib::compute_state_checksum(lib_jobs));

    std::string serialized;
    if (!msg.SerializeToString(&serialized)) {
        LOG(ERROR) << "Failed to serialize v2 sync message";
        return;
    }

    std::vector<uint8_t> payload(serialized.begin(), serialized.end());
    auto result = transport_->SendToVehicle(
        vehicle_id, payload,
        swdv::cloud_backend_transport_service::persistence_t::VOLATILE);

    if (result.status() == swdv::cloud_backend_transport_service::publish_status_t::OK) {
        LOG(INFO) << "Sent v2 sync message to " << vehicle_id
                  << " with " << pending_count << " jobs, checksum=" << msg.state_checksum();
    } else {
        LOG(ERROR) << "Failed to send v2 sync message to " << vehicle_id;
    }
}

void CloudSchedulerService::JobInfoToRecord(
    const sched::job_info_t& job,
    const sched_lib::VersionVector& version,
    sync_v2::JobRecord* record) {

    record->set_job_id(job.job_id());
    record->set_title(job.title());
    record->set_service(job.service());
    record->set_method(job.method());
    record->set_parameters_json(job.parameters_json());
    record->set_recurrence_rule(job.recurrence_rule());
    record->set_created_at_ms(job.created_at_ms());
    record->set_updated_at_ms(job.updated_at_ms());
    record->set_scheduled_time_ms(job.scheduled_time_ms());
    if (job.next_run_time_ms() > 0) {
        record->set_next_run_time_ms(job.next_run_time_ms());
    }
    if (job.end_time_ms() > 0) {
        record->set_end_time_ms(job.end_time_ms());
    }

    // Set version
    auto* v = record->mutable_version();
    v->set_cloud_seq(version.cloud_seq);
    v->set_vehicle_seq(version.vehicle_seq);

    // Map IFEX status to sync v2 status
    record->set_status(JobStatusToSyncV2(job.status()));

    // Paused is a separate boolean field (user intent, synced)
    record->set_paused(job.paused());

    // Deleted for tombstones
    record->set_deleted(job.deleted());

    // Wake/sleep policies
    record->set_wake_policy(WakePolicyToSyncV2(job.wake_policy()));
    record->set_sleep_policy(SleepPolicyToSyncV2(job.sleep_policy()));
    record->set_wake_lead_time_s(job.wake_lead_time_s());

    // Authority
    record->set_authority(AuthorityToSyncV2(job.authority()));
}

void CloudSchedulerService::RecordToJobInfo(
    const sync_v2::JobRecord& record,
    sched::job_info_t* job) {

    job->set_job_id(record.job_id());
    job->set_title(record.title());
    job->set_service(record.service());
    job->set_method(record.method());
    job->set_parameters_json(record.parameters_json());
    job->set_recurrence_rule(record.recurrence_rule());
    job->set_created_at_ms(record.created_at_ms());
    job->set_updated_at_ms(record.updated_at_ms());
    job->set_scheduled_time_ms(record.scheduled_time_ms());
    job->set_next_run_time_ms(record.next_run_time_ms());
    job->set_end_time_ms(record.end_time_ms());

    // Map sync v2 status to IFEX status
    job->set_status(SyncV2ToJobStatus(record.status()));

    // Paused is a separate boolean field
    job->set_paused(record.paused());

    // Deleted for tombstones
    job->set_deleted(record.deleted());

    // Wake/sleep policies
    job->set_wake_policy(SyncV2ToWakePolicy(record.wake_policy()));
    job->set_sleep_policy(SyncV2ToSleepPolicy(record.sleep_policy()));
    job->set_wake_lead_time_s(record.wake_lead_time_s());

    // Authority
    job->set_authority(SyncV2ToAuthority(record.authority()));
}

}  // namespace ifex::cloud
