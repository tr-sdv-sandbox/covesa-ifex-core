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
namespace sched = ::ifex::cloud::scheduler;

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

grpc::Status CloudSchedulerService::CreateJob(
    grpc::ServerContext* context,
    const sched::CreateJobRequest* request,
    sched::CreateJobResponse* response) {

    if (request->vehicle_id().empty()) {
        response->set_success(false);
        response->set_error_message("vehicle_id is required");
        return grpc::Status::OK;
    }

    if (request->service().empty() || request->method().empty()) {
        response->set_success(false);
        response->set_error_message("service and method are required");
        return grpc::Status::OK;
    }

    std::string job_id = GenerateJobId();
    uint64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // Store job locally with pending sync state
    {
        std::lock_guard<std::mutex> lock(jobs_mutex_);
        sched::JobInfo job;
        job.set_vehicle_id(request->vehicle_id());
        job.set_job_id(job_id);
        job.set_title(request->title());
        job.set_service(request->service());
        job.set_method(request->method());
        job.set_parameters_json(request->parameters_json());
        job.set_scheduled_time_ms(request->scheduled_time_ms());
        job.set_recurrence_rule(request->recurrence_rule());
        job.set_end_time_ms(request->end_time_ms());
        job.set_status(sync_v2::JOB_STATUS_PENDING);
        job.set_created_at_ms(now_ms);
        job.set_updated_at_ms(now_ms);
        job.set_created_by(request->created_by());
        job.set_paused(request->paused());
        job.set_authority(sync_v2::AUTHORITY_CLOUD);  // Cloud-created jobs
        job.set_wake_policy(request->wake_policy());
        job.set_sleep_policy(request->sleep_policy());
        job.set_wake_lead_time_s(request->wake_lead_time_s());
        jobs_[request->vehicle_id()][job_id] = job;

        // Initialize v2 sync state for cloud-created job
        job_versions_[request->vehicle_id()][job_id] = sync::VersionVector{1, 0};
        job_sync_states_[request->vehicle_id()][job_id] = sched::CLOUD_SYNC_PENDING;
    }

    // Send C2V_SyncMessage with the new job to the vehicle
    SendPendingJobsToVehicle(request->vehicle_id());

    response->set_success(true);
    response->set_job_id(job_id);
    LOG(INFO) << "Created job " << job_id << " on " << request->vehicle_id();
    return grpc::Status::OK;
}

grpc::Status CloudSchedulerService::UpdateJob(
    grpc::ServerContext* context,
    const sched::UpdateJobRequest* request,
    sched::UpdateJobResponse* response) {

    if (request->vehicle_id().empty() || request->job_id().empty()) {
        response->set_success(false);
        response->set_error_message("vehicle_id and job_id are required");
        return grpc::Status::OK;
    }

    uint64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // Update job locally and mark as pending sync
    {
        std::lock_guard<std::mutex> lock(jobs_mutex_);
        auto& vehicle_jobs = jobs_[request->vehicle_id()];
        auto it = vehicle_jobs.find(request->job_id());
        if (it == vehicle_jobs.end()) {
            response->set_success(false);
            response->set_error_message("Job not found");
            return grpc::Status::OK;
        }

        // Update job fields
        auto& job = it->second;
        if (!request->title().empty()) job.set_title(request->title());
        if (request->scheduled_time_ms() > 0) job.set_scheduled_time_ms(request->scheduled_time_ms());
        if (!request->recurrence_rule().empty()) job.set_recurrence_rule(request->recurrence_rule());
        if (!request->parameters_json().empty()) job.set_parameters_json(request->parameters_json());
        if (request->end_time_ms() > 0) job.set_end_time_ms(request->end_time_ms());
        job.set_updated_at_ms(now_ms);

        // Increment version for cloud change
        auto& version = job_versions_[request->vehicle_id()][request->job_id()];
        version.increment_cloud();
        job_sync_states_[request->vehicle_id()][request->job_id()] = sched::CLOUD_SYNC_PENDING;
    }

    // Send updated job state to vehicle
    SendPendingJobsToVehicle(request->vehicle_id());

    response->set_success(true);
    return grpc::Status::OK;
}

grpc::Status CloudSchedulerService::DeleteJob(
    grpc::ServerContext* context,
    const sched::DeleteJobRequest* request,
    sched::DeleteJobResponse* response) {

    if (request->vehicle_id().empty() || request->job_id().empty()) {
        response->set_success(false);
        response->set_error_message("vehicle_id and job_id are required");
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
            job_sync_states_[request->vehicle_id()][request->job_id()] = sched::CLOUD_SYNC_PENDING;
        }
    }

    // Send sync message with tombstone
    SendPendingJobsToVehicle(request->vehicle_id());

    LOG(INFO) << "Marked job " << request->job_id() << " as deleted for "
              << request->vehicle_id();

    response->set_success(true);
    return grpc::Status::OK;
}

grpc::Status CloudSchedulerService::PauseJob(
    grpc::ServerContext* context,
    const sched::PauseJobRequest* request,
    sched::PauseJobResponse* response) {

    if (request->vehicle_id().empty() || request->job_id().empty()) {
        response->set_success(false);
        response->set_error_message("vehicle_id and job_id are required");
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
            response->set_success(false);
            response->set_error_message("Job not found");
            return grpc::Status::OK;
        }

        it->second.set_paused(true);
        it->second.set_updated_at_ms(now_ms);

        // Increment version for cloud change
        auto& version = job_versions_[request->vehicle_id()][request->job_id()];
        version.increment_cloud();
        job_sync_states_[request->vehicle_id()][request->job_id()] = sched::CLOUD_SYNC_PENDING;
    }

    // Send updated state to vehicle
    SendPendingJobsToVehicle(request->vehicle_id());

    response->set_success(true);
    return grpc::Status::OK;
}

grpc::Status CloudSchedulerService::ResumeJob(
    grpc::ServerContext* context,
    const sched::ResumeJobRequest* request,
    sched::ResumeJobResponse* response) {

    if (request->vehicle_id().empty() || request->job_id().empty()) {
        response->set_success(false);
        response->set_error_message("vehicle_id and job_id are required");
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
            response->set_success(false);
            response->set_error_message("Job not found");
            return grpc::Status::OK;
        }

        it->second.set_paused(false);
        it->second.set_updated_at_ms(now_ms);

        // Increment version for cloud change
        auto& version = job_versions_[request->vehicle_id()][request->job_id()];
        version.increment_cloud();
        job_sync_states_[request->vehicle_id()][request->job_id()] = sched::CLOUD_SYNC_PENDING;
    }

    // Send updated state to vehicle
    SendPendingJobsToVehicle(request->vehicle_id());

    response->set_success(true);
    return grpc::Status::OK;
}

grpc::Status CloudSchedulerService::TriggerJob(
    grpc::ServerContext* context,
    const sched::TriggerJobRequest* request,
    sched::TriggerJobResponse* response) {

    if (request->vehicle_id().empty() || request->job_id().empty()) {
        response->set_success(false);
        response->set_error_message("vehicle_id and job_id are required");
        return grpc::Status::OK;
    }

    // TriggerJob is the only imperative command - send TriggerJobRequest
    if (!SendTriggerJobRequest(request->vehicle_id(), request->job_id(), "cloud-scheduler")) {
        response->set_success(false);
        response->set_error_message("Failed to send trigger request to vehicle");
        return grpc::Status::OK;
    }

    response->set_success(true);
    return grpc::Status::OK;
}

grpc::Status CloudSchedulerService::GetJob(
    grpc::ServerContext* context,
    const sched::GetJobRequest* request,
    sched::GetJobResponse* response) {

    std::lock_guard<std::mutex> lock(jobs_mutex_);

    auto vehicle_it = jobs_.find(request->vehicle_id());
    if (vehicle_it == jobs_.end()) {
        response->set_found(false);
        return grpc::Status::OK;
    }

    auto job_it = vehicle_it->second.find(request->job_id());
    if (job_it == vehicle_it->second.end()) {
        response->set_found(false);
        return grpc::Status::OK;
    }

    // Tombstones (deleted jobs) should not be found via GetJob
    if (job_it->second.deleted()) {
        response->set_found(false);
        return grpc::Status::OK;
    }

    response->set_found(true);
    *response->mutable_job() = job_it->second;
    return grpc::Status::OK;
}

grpc::Status CloudSchedulerService::ListJobs(
    grpc::ServerContext* context,
    const sched::ListJobsRequest* request,
    sched::ListJobsResponse* response) {

    std::lock_guard<std::mutex> lock(jobs_mutex_);

    int count = 0;
    for (const auto& [vehicle_id, vehicle_jobs] : jobs_) {
        // Apply vehicle filter
        if (!request->vehicle_id_filter().empty() &&
            vehicle_id != request->vehicle_id_filter()) {
            continue;
        }

        for (const auto& [job_id, job] : vehicle_jobs) {
            // Filter out deleted jobs unless include_deleted is true
            if (job.deleted() && !request->include_deleted()) {
                continue;
            }

            // Apply service filter
            if (!request->service_filter().empty() &&
                job.service() != request->service_filter()) {
                continue;
            }

            // Apply paused_only filter
            if (request->paused_only() && !job.paused()) {
                continue;
            }

            *response->add_jobs() = job;
            count++;
        }
    }

    response->set_total_count(count);
    return grpc::Status::OK;
}

grpc::Status CloudSchedulerService::GetJobExecutions(
    grpc::ServerContext* context,
    const sched::GetJobExecutionsRequest* request,
    sched::GetJobExecutionsResponse* response) {

    std::lock_guard<std::mutex> lock(executions_mutex_);

    response->set_vehicle_id(request->vehicle_id());
    response->set_job_id(request->job_id());

    auto vehicle_it = executions_.find(request->vehicle_id());
    if (vehicle_it == executions_.end()) {
        response->set_total_count(0);
        return grpc::Status::OK;
    }

    auto job_it = vehicle_it->second.find(request->job_id());
    if (job_it == vehicle_it->second.end()) {
        response->set_total_count(0);
        return grpc::Status::OK;
    }

    int limit = request->limit() > 0 ? request->limit() : 100;
    int count = 0;
    for (const auto& exec : job_it->second) {
        if (request->since_ms() > 0 && exec.executed_at_ms() < request->since_ms()) {
            continue;
        }
        *response->add_executions() = exec;
        if (++count >= limit) break;
    }

    response->set_total_count(static_cast<int>(job_it->second.size()));
    return grpc::Status::OK;
}

// Fleet operations - simplified stubs for testing
grpc::Status CloudSchedulerService::CreateFleetJob(
    grpc::ServerContext* context,
    const sched::CreateFleetJobRequest* request,
    sched::CreateFleetJobResponse* response) {

    // For testing, just create jobs on each specified vehicle
    int successful = 0;
    int failed = 0;

    for (const auto& vehicle_id : request->vehicle_ids()) {
        sched::CreateJobRequest single_request;
        single_request.set_vehicle_id(vehicle_id);
        single_request.set_title(request->title());
        single_request.set_service(request->service());
        single_request.set_method(request->method());
        single_request.set_parameters_json(request->parameters_json());
        single_request.set_scheduled_time_ms(request->scheduled_time_ms());
        single_request.set_recurrence_rule(request->recurrence_rule());
        single_request.set_end_time_ms(request->end_time_ms());
        single_request.set_created_by(request->created_by());

        sched::CreateJobResponse single_response;
        CreateJob(context, &single_request, &single_response);

        auto* result = response->add_results();
        result->set_vehicle_id(vehicle_id);
        result->set_success(single_response.success());
        result->set_job_id(single_response.job_id());
        result->set_error_message(single_response.error_message());

        if (single_response.success()) {
            successful++;
        } else {
            failed++;
        }
    }

    response->set_total_vehicles(request->vehicle_ids_size());
    response->set_successful(successful);
    response->set_failed(failed);
    response->set_fleet_job_id(GenerateJobId());

    return grpc::Status::OK;
}

grpc::Status CloudSchedulerService::DeleteFleetJob(
    grpc::ServerContext* context,
    const sched::DeleteFleetJobRequest* request,
    sched::DeleteFleetJobResponse* response) {

    int successful = 0;
    int failed = 0;

    for (const auto& vehicle_id : request->vehicle_ids()) {
        for (const auto& job_id : request->job_ids()) {
            sched::DeleteJobRequest single_request;
            single_request.set_vehicle_id(vehicle_id);
            single_request.set_job_id(job_id);

            sched::DeleteJobResponse single_response;
            DeleteJob(context, &single_request, &single_response);

            if (single_response.success()) {
                successful++;
            } else {
                failed++;
            }
        }
    }

    response->set_total_deletions(request->vehicle_ids_size() * request->job_ids_size());
    response->set_successful(successful);
    response->set_failed(failed);

    return grpc::Status::OK;
}

grpc::Status CloudSchedulerService::GetFleetJobStats(
    grpc::ServerContext* context,
    const sched::GetFleetJobStatsRequest* request,
    sched::GetFleetJobStatsResponse* response) {

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

    response->set_total_jobs(total_jobs);
    response->set_total_vehicles_with_jobs(static_cast<int>(vehicles_with_jobs.size()));

    for (const auto& [status, count] : by_status) {
        (*response->mutable_by_status())[std::to_string(status)] = count;
    }

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

uint64_t CloudSchedulerService::ComputeJobHash(const sched::JobInfo& job) {
    // Hash only content fields - exclude metadata like updated_at_ms, created_at_ms
    // which can change without actual job content changing
    std::hash<std::string> str_hash;
    std::hash<uint64_t> uint64_hash;
    std::hash<bool> bool_hash;

    // Use golden ratio constant for hash mixing (0x9e3779b9)
    uint64_t h = str_hash(job.job_id());
    h ^= str_hash(job.title()) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= str_hash(job.service()) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= str_hash(job.method()) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= str_hash(job.parameters_json()) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= uint64_hash(job.scheduled_time_ms()) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= str_hash(job.recurrence_rule()) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= uint64_hash(job.end_time_ms()) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= bool_hash(job.paused()) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= bool_hash(job.deleted()) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= uint64_hash(static_cast<uint64_t>(job.wake_policy())) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= uint64_hash(static_cast<uint64_t>(job.sleep_policy())) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= uint64_hash(job.wake_lead_time_s()) + 0x9e3779b9 + (h << 6) + (h >> 2);
    // Note: status, updated_at_ms, created_at_ms excluded - they're metadata, not content

    return h;
}

uint64_t CloudSchedulerService::ComputeStateChecksum(const std::string& vehicle_id) const {
    std::lock_guard<std::mutex> lock(jobs_mutex_);

    auto vehicle_it = jobs_.find(vehicle_id);
    if (vehicle_it == jobs_.end() || vehicle_it->second.empty()) {
        return 0;  // No jobs = checksum 0
    }

    // CRC32-style checksum combining all job hashes
    // Sort by job_id for deterministic ordering
    std::vector<std::string> sorted_ids;
    for (const auto& [id, _] : vehicle_it->second) {
        sorted_ids.push_back(id);
    }
    std::sort(sorted_ids.begin(), sorted_ids.end());

    uint32_t crc = 0xFFFFFFFF;
    for (const auto& id : sorted_ids) {
        const auto& job = vehicle_it->second.at(id);
        uint64_t hash = ComputeJobHash(job);

        // Mix hash into CRC using standard CRC32 polynomial
        for (int i = 0; i < 8; ++i) {
            uint8_t byte = (hash >> (i * 8)) & 0xFF;
            crc ^= byte;
            for (int j = 0; j < 8; ++j) {
                crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
            }
        }
    }

    return crc ^ 0xFFFFFFFF;
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
                sync_state_it->second == sched::CLOUD_SYNC_PENDING) {
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
    sync::VersionVector remote_version{
        record.version().cloud_seq(),
        record.version().vehicle_seq()
    };

    // Get local version if exists
    std::optional<sync::VersionVector> local_version;
    auto& vehicle_versions = job_versions_[vehicle_id];
    auto version_it = vehicle_versions.find(record.job_id());
    if (version_it != vehicle_versions.end()) {
        local_version = version_it->second;
    }

    // Determine authority
    sync::JobAuthority authority = (record.authority() == sync_v2::AUTHORITY_CLOUD)
        ? sync::JobAuthority::CLOUD
        : sync::JobAuthority::VEHICLE;

    // Use sync engine to determine action (cloud side)
    auto result = sync::SyncEngine::process_remote(
        remote_version, local_version, authority, true /* is_cloud_side */);

    switch (result.action) {
        case sync::SyncResult::ACCEPT_REMOTE: {
            // Accept vehicle's version
            sched::JobInfo job;
            RecordToJobInfo(record, &job);
            job.set_vehicle_id(vehicle_id);
            jobs_[vehicle_id][record.job_id()] = job;
            vehicle_versions[record.job_id()] = result.resolved_version;
            job_sync_states_[vehicle_id][record.job_id()] = sched::CLOUD_SYNC_SYNCED;
            LOG(INFO) << "Accepted job " << record.job_id() << " from " << vehicle_id
                      << " version={" << result.resolved_version.cloud_seq << ","
                      << result.resolved_version.vehicle_seq << "}";
            break;
        }

        case sync::SyncResult::REJECT_REMOTE: {
            // Keep our version, will push to vehicle in SendV2SyncMessage
            job_sync_states_[vehicle_id][record.job_id()] = sched::CLOUD_SYNC_PENDING;
            LOG(INFO) << "Rejected job " << record.job_id() << " from " << vehicle_id
                      << " (keeping cloud version)";
            break;
        }

        case sync::SyncResult::CONFLICT_RESOLVED: {
            // Conflict was already resolved by process_remote
            // result.winner tells us who won
            bool we_won = (result.winner == "cloud");
            if (!we_won) {
                // Vehicle wins, accept their version
                sched::JobInfo job;
                RecordToJobInfo(record, &job);
                job.set_vehicle_id(vehicle_id);
                jobs_[vehicle_id][record.job_id()] = job;
                vehicle_versions[record.job_id()] = result.resolved_version;
                job_sync_states_[vehicle_id][record.job_id()] = sched::CLOUD_SYNC_SYNCED;
                LOG(INFO) << "Conflict resolved: accepted vehicle job " << record.job_id();
            } else {
                // Cloud wins, keep our version and push
                vehicle_versions[record.job_id()] = result.resolved_version;
                job_sync_states_[vehicle_id][record.job_id()] = sched::CLOUD_SYNC_PENDING;
                LOG(INFO) << "Conflict resolved: keeping cloud job " << record.job_id();
            }
            break;
        }

        case sync::SyncResult::NO_ACTION:
        default:
            // Already in sync
            break;
    }

    // Handle deleted jobs (tombstones)
    if (record.deleted()) {
        // Keep the tombstone record but mark as synced
        sched::JobInfo job;
        RecordToJobInfo(record, &job);
        job.set_vehicle_id(vehicle_id);
        job.set_deleted(true);
        jobs_[vehicle_id][record.job_id()] = job;
        vehicle_versions[record.job_id()] = result.resolved_version;
        job_sync_states_[vehicle_id][record.job_id()] = sched::CLOUD_SYNC_SYNCED;
        LOG(INFO) << "Processed tombstone for job " << record.job_id() << " from " << vehicle_id;
    }
}

void CloudSchedulerService::ProcessVehicleExecutions(
    const std::string& vehicle_id,
    const google::protobuf::RepeatedPtrField<sync_v2::ExecutionRecord>& executions) {

    std::lock_guard<std::mutex> exec_lock(executions_mutex_);

    for (const auto& exec_record : executions) {
        sched::ExecutionInfo exec;
        exec.set_execution_id(exec_record.execution_id());
        exec.set_executed_at_ms(exec_record.executed_at_ms());
        exec.set_duration_ms(exec_record.duration_ms());
        exec.set_result_json(exec_record.result_json());
        exec.set_error_message(exec_record.error_message());

        // Copy status directly - ExecutionInfo.status uses sync_v2::JobStatus
        exec.set_status(exec_record.status());

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
            sync_state_it->second == sched::CLOUD_SYNC_PENDING) {

            auto* record = msg.add_jobs();
            auto version_it = job_versions_[vehicle_id].find(job_id);
            sync::VersionVector version = (version_it != job_versions_[vehicle_id].end())
                ? version_it->second
                : sync::VersionVector{1, 0};  // Default: cloud created

            JobInfoToRecord(job, version, record);
            pending_count++;
        }
    }

    if (pending_count == 0) {
        LOG(INFO) << "No pending jobs to sync to " << vehicle_id;
        return;
    }

    // Compute state checksum for quiescence detection
    // Note: We need to unlock mutex before calling ComputeStateChecksum since it takes its own lock
    // For now, compute inline here with the jobs we have
    uint32_t crc = 0xFFFFFFFF;
    std::vector<std::string> sorted_ids;
    for (const auto& [id, _] : jobs_[vehicle_id]) {
        sorted_ids.push_back(id);
    }
    std::sort(sorted_ids.begin(), sorted_ids.end());
    for (const auto& id : sorted_ids) {
        const auto& job = jobs_[vehicle_id].at(id);
        uint64_t hash = ComputeJobHash(job);
        for (int i = 0; i < 8; ++i) {
            uint8_t byte = (hash >> (i * 8)) & 0xFF;
            crc ^= byte;
            for (int j = 0; j < 8; ++j) {
                crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
            }
        }
    }
    msg.set_state_checksum(crc ^ 0xFFFFFFFF);

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
    const sched::JobInfo& job,
    const sync::VersionVector& version,
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

    // Copy status directly - JobInfo.status uses sync_v2::JobStatus
    record->set_status(job.status());

    // Paused is a separate boolean field (user intent, synced)
    record->set_paused(job.paused());

    // Deleted for tombstones
    record->set_deleted(job.deleted());

    // Wake/sleep policies
    record->set_wake_policy(job.wake_policy());
    record->set_sleep_policy(job.sleep_policy());
    record->set_wake_lead_time_s(job.wake_lead_time_s());

    // Authority - cloud-created jobs have AUTHORITY_CLOUD
    record->set_authority(job.authority());
}

void CloudSchedulerService::RecordToJobInfo(
    const sync_v2::JobRecord& record,
    sched::JobInfo* job) {

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

    // Copy status directly - JobInfo.status uses sync_v2::JobStatus
    job->set_status(record.status());

    // Paused is a separate boolean field
    job->set_paused(record.paused());

    // Deleted for tombstones
    job->set_deleted(record.deleted());

    // Wake/sleep policies
    job->set_wake_policy(record.wake_policy());
    job->set_sleep_policy(record.sleep_policy());
    job->set_wake_lead_time_s(record.wake_lead_time_s());

    // Authority
    job->set_authority(record.authority());
}

}  // namespace ifex::cloud
