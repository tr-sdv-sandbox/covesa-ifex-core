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

static sched_lib::Job JobInfoToLibraryJob(const scheduler_types::job_t& job) {
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
    lib_job.version.cloud_seq = job.cloud_seq();
    lib_job.version.vehicle_seq = job.vehicle_seq();
    lib_job.deleted = job.deleted();
    // Include deleted_at_ms for consistent hash computation with vehicle scheduler
    if (job.deleted()) {
        lib_job.deleted_at_ms = job.updated_at_ms();  // Use updated_at as deleted_at
    }
    return lib_job;
}

// Map common job status to sync v2 job status
static sync_v2::JobStatus JobStatusToSyncV2(scheduler_types_pb::job_status_t status) {
    switch (status) {
        case scheduler_types_pb::JOB_STATUS_PENDING: return sync_v2::JOB_STATUS_PENDING;
        case scheduler_types_pb::JOB_STATUS_RUNNING: return sync_v2::JOB_STATUS_RUNNING;
        case scheduler_types_pb::JOB_STATUS_COMPLETED: return sync_v2::JOB_STATUS_COMPLETED;
        case scheduler_types_pb::JOB_STATUS_FAILED: return sync_v2::JOB_STATUS_FAILED;
        case scheduler_types_pb::JOB_STATUS_CANCELLED: return sync_v2::JOB_STATUS_CANCELLED;
        default: return sync_v2::JOB_STATUS_PENDING;
    }
}

// Map sync v2 job status to common job status
static scheduler_types_pb::job_status_t SyncV2ToJobStatus(sync_v2::JobStatus status) {
    switch (status) {
        case sync_v2::JOB_STATUS_PENDING: return scheduler_types_pb::JOB_STATUS_PENDING;
        case sync_v2::JOB_STATUS_RUNNING: return scheduler_types_pb::JOB_STATUS_RUNNING;
        case sync_v2::JOB_STATUS_COMPLETED: return scheduler_types_pb::JOB_STATUS_COMPLETED;
        case sync_v2::JOB_STATUS_FAILED: return scheduler_types_pb::JOB_STATUS_FAILED;
        case sync_v2::JOB_STATUS_CANCELLED: return scheduler_types_pb::JOB_STATUS_CANCELLED;
        default: return scheduler_types_pb::JOB_STATUS_PENDING;
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

CloudSchedulerService::~CloudSchedulerService() = default;

void CloudSchedulerService::RegisterServices(grpc::ServerBuilder& builder) {
    // Dashboard API
    builder.RegisterService(static_cast<sched::create_job_service::Service*>(this));
    builder.RegisterService(static_cast<sched::update_job_service::Service*>(this));
    builder.RegisterService(static_cast<sched::delete_job_service::Service*>(this));
    builder.RegisterService(static_cast<sched::pause_job_service::Service*>(this));
    builder.RegisterService(static_cast<sched::resume_job_service::Service*>(this));
    builder.RegisterService(static_cast<sched::trigger_job_service::Service*>(this));
    builder.RegisterService(static_cast<sched::get_job_service::Service*>(this));
    builder.RegisterService(static_cast<sched::list_jobs_service::Service*>(this));
    builder.RegisterService(static_cast<sched::list_jobs_hash_service::Service*>(this));
    builder.RegisterService(static_cast<sched::list_executions_service::Service*>(this));
    builder.RegisterService(static_cast<sched::list_executions_hash_service::Service*>(this));
    builder.RegisterService(static_cast<sched::healthy_service::Service*>(this));
    // Internal API for sync bridge
    builder.RegisterService(static_cast<sched::get_jobs_for_vehicle_service::Service*>(this));
    builder.RegisterService(static_cast<sched::upsert_job_service::Service*>(this));
    builder.RegisterService(static_cast<sched::record_execution_service::Service*>(this));
    builder.RegisterService(static_cast<sched::get_vehicle_sync_state_service::Service*>(this));
    builder.RegisterService(static_cast<sched::update_vehicle_sync_state_service::Service*>(this));
    builder.RegisterService(static_cast<sched::get_pending_syncs_service::Service*>(this));
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
        scheduler_types::job_t job;
        job.set_vehicle_id(req.vehicle_id());
        job.set_job_id(job_id);
        job.set_title(req.title());
        job.set_service(req.service());
        job.set_method(req.method());
        job.set_parameters_json(req.parameters_json());
        job.set_scheduled_time_ms(req.scheduled_time_ms());
        job.set_recurrence_rule(req.recurrence_rule());
        job.set_end_time_ms(req.end_time_ms());
        job.set_status(scheduler_types_pb::JOB_STATUS_PENDING);
        job.set_created_at_ms(now_ms);
        job.set_updated_at_ms(now_ms);
        job.set_created_by(req.created_by());
        job.set_paused(req.paused());
        job.set_authority(scheduler_types_pb::AUTHORITY_CLOUD);  // Cloud-created jobs
        job.set_cloud_seq(1);  // Cloud-created job starts with cloud_seq=1
        job.set_vehicle_seq(0);
        job.set_wake_policy(req.wake_policy());
        job.set_sleep_policy(req.sleep_policy());
        job.set_wake_lead_time_s(req.wake_lead_time_s());
        jobs_[req.vehicle_id()][job_id] = job;

        // Initialize v2 sync state for cloud-created job
        job_sync_states_[req.vehicle_id()][job_id] = scheduler_types_pb::SYNC_PENDING;
    }

    // Update cloud checksum after job change
    UpdateCloudChecksum(req.vehicle_id());

    // Note: Sync bridge will detect checksum change and push to vehicle

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
        job.set_cloud_seq(job.cloud_seq() + 1);
        job_sync_states_[req.vehicle_id()][req.job_id()] = scheduler_types_pb::SYNC_PENDING;
    }

    // Update cloud checksum after job change
    UpdateCloudChecksum(req.vehicle_id());

    // Note: Sync bridge will detect checksum change and push to vehicle

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
            it->second.set_cloud_seq(it->second.cloud_seq() + 1);
            job_sync_states_[request->vehicle_id()][request->job_id()] = scheduler_types_pb::SYNC_PENDING;
        }
    }

    // Update cloud checksum after job change
    UpdateCloudChecksum(request->vehicle_id());

    // Note: Sync bridge will detect checksum change and push tombstone to vehicle

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
        it->second.set_cloud_seq(it->second.cloud_seq() + 1);
        job_sync_states_[request->vehicle_id()][request->job_id()] = scheduler_types_pb::SYNC_PENDING;
    }

    // Note: Sync bridge will detect checksum change and push to vehicle

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
        it->second.set_cloud_seq(it->second.cloud_seq() + 1);
        job_sync_states_[request->vehicle_id()][request->job_id()] = scheduler_types_pb::SYNC_PENDING;
    }

    // Note: Sync bridge will detect checksum change and push to vehicle

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

    // Note: Trigger job requests should be sent via CloudSchedulerSyncBridge
    // This service is just storage - it doesn't have direct transport access
    LOG(WARNING) << "trigger_job called but transport is handled by sync bridge. "
                 << "vehicle=" << request->vehicle_id() << " job=" << request->job_id();
    result->set_success(false);
    result->set_error_message("trigger_job not supported - use sync bridge for imperative commands");
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

    // Populate sync_state from tracking
    auto vehicle_states_it = job_sync_states_.find(request->vehicle_id());
    if (vehicle_states_it != job_sync_states_.end()) {
        auto job_state_it = vehicle_states_it->second.find(request->job_id());
        if (job_state_it != vehicle_states_it->second.end()) {
            result->mutable_job()->set_sync_state(job_state_it->second);
        }
    }

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

            // Copy job and populate sync_state from tracking
            auto* out_job = result->add_jobs();
            *out_job = job;

            // Look up sync state for this job
            auto vehicle_states_it = job_sync_states_.find(vehicle_id);
            if (vehicle_states_it != job_sync_states_.end()) {
                auto job_state_it = vehicle_states_it->second.find(job_id);
                if (job_state_it != vehicle_states_it->second.end()) {
                    out_job->set_sync_state(job_state_it->second);
                }
            }

            count++;
        }
    }

    result->set_total_count(count);
    return grpc::Status::OK;
}

grpc::Status CloudSchedulerService::list_jobs_hash(
    grpc::ServerContext* context,
    const sched::list_jobs_hash_request* request,
    sched::list_jobs_hash_response* response) {

    auto* result = response->mutable_result();
    const auto& filter = request->filter();
    std::lock_guard<std::mutex> lock(jobs_mutex_);

    // Collect matching jobs and convert to library format
    std::vector<sched_lib::Job> matching_jobs;

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

            // Apply time range filters
            if (filter.start_time_ms() > 0 && job.scheduled_time_ms() < filter.start_time_ms()) {
                continue;
            }
            if (filter.end_time_ms() > 0 && job.scheduled_time_ms() > filter.end_time_ms()) {
                continue;
            }

            // Apply paused_only filter
            if (filter.paused_only() && !job.paused()) {
                continue;
            }

            auto lib_job = JobInfoToLibraryJob(job);

            // Look up sync_state for this job (for UI change detection)
            auto vehicle_states_it = job_sync_states_.find(vehicle_id);
            if (vehicle_states_it != job_sync_states_.end()) {
                auto job_state_it = vehicle_states_it->second.find(job_id);
                if (job_state_it != vehicle_states_it->second.end()) {
                    lib_job.sync_state = static_cast<sched_lib::SyncState>(job_state_it->second);
                }
            }

            matching_jobs.push_back(lib_job);
        }
    }

    // Sort by job_id for deterministic hash (required by compute_state_checksum)
    std::sort(matching_jobs.begin(), matching_jobs.end(),
              [](const sched_lib::Job& a, const sched_lib::Job& b) {
                  return a.job_id < b.job_id;
              });

    // Compute checksum using library function (doesn't include sync_state)
    uint64_t state_hash = sched_lib::compute_state_checksum(matching_jobs);

    // Mix in sync_state for UI change detection
    // This is separate from the sync protocol checksum (ComputeStateChecksum)
    // which is used for cloud<->vehicle quiescence and must NOT include sync_state
    for (const auto& job : matching_jobs) {
        state_hash ^= static_cast<uint64_t>(job.sync_state) + 0x9e3779b9 + (state_hash << 6) + (state_hash >> 2);
    }

    result->set_state_hash(state_hash);
    result->set_job_count(static_cast<int32_t>(matching_jobs.size()));

    VLOG(1) << "list_jobs_hash: hash=" << state_hash << " count=" << matching_jobs.size();
    return grpc::Status::OK;
}

grpc::Status CloudSchedulerService::list_executions(
    grpc::ServerContext* context,
    const sched::list_executions_request* request,
    sched::list_executions_response* response) {

    auto* result = response->mutable_result();
    const auto& filter = request->filter();
    std::lock_guard<std::mutex> lock(executions_mutex_);

    // Collect all matching executions
    std::vector<const scheduler_types::execution_record_t*> matching;

    for (const auto& [vehicle_id, job_map] : executions_) {
        // Filter by vehicle_id if specified
        if (!filter.vehicle_id().empty() && vehicle_id != filter.vehicle_id()) {
            continue;
        }

        for (const auto& [job_id, exec_list] : job_map) {
            // Filter by job_id if specified
            if (!filter.job_id().empty() && job_id != filter.job_id()) {
                continue;
            }

            for (const auto& exec : exec_list) {
                // Filter by time range
                if (filter.start_time_ms() > 0 && exec.executed_at_ms() < filter.start_time_ms()) {
                    continue;
                }
                if (filter.end_time_ms() > 0 && exec.executed_at_ms() > filter.end_time_ms()) {
                    continue;
                }
                // Filter by status
                if (filter.status_filter() != 0 && exec.status() != filter.status_filter()) {
                    continue;
                }
                matching.push_back(&exec);
            }
        }
    }

    // Sort by execution time (newest first)
    std::sort(matching.begin(), matching.end(),
        [](const auto* a, const auto* b) {
            return a->executed_at_ms() > b->executed_at_ms();
        });

    // Apply pagination
    int limit = filter.limit() > 0 ? filter.limit() : 100;
    int offset = filter.offset() > 0 ? filter.offset() : 0;
    int count = 0;

    for (size_t i = offset; i < matching.size() && count < limit; ++i, ++count) {
        *result->add_executions() = *matching[i];
    }

    result->set_total_count(static_cast<int>(matching.size()));
    return grpc::Status::OK;
}

grpc::Status CloudSchedulerService::list_executions_hash(
    grpc::ServerContext* context,
    const sched::list_executions_hash_request* request,
    sched::list_executions_hash_response* response) {

    auto* result = response->mutable_result();
    const auto& filter = request->filter();
    std::lock_guard<std::mutex> lock(executions_mutex_);

    // Collect matching executions for deterministic ordering
    std::vector<const scheduler_types::execution_record_t*> matching;

    for (const auto& [vehicle_id, job_map] : executions_) {
        if (!filter.vehicle_id().empty() && vehicle_id != filter.vehicle_id()) {
            continue;
        }

        for (const auto& [job_id, exec_list] : job_map) {
            if (!filter.job_id().empty() && job_id != filter.job_id()) {
                continue;
            }

            for (const auto& exec : exec_list) {
                if (filter.start_time_ms() > 0 && exec.executed_at_ms() < filter.start_time_ms()) {
                    continue;
                }
                if (filter.end_time_ms() > 0 && exec.executed_at_ms() > filter.end_time_ms()) {
                    continue;
                }
                if (filter.status_filter() != 0 && exec.status() != filter.status_filter()) {
                    continue;
                }
                matching.push_back(&exec);
            }
        }
    }

    // Sort by execution_id for deterministic hash
    std::sort(matching.begin(), matching.end(),
        [](const auto* a, const auto* b) {
            return a->execution_id() < b->execution_id();
        });

    // Compute hash using library's hash functions
    uint64_t hash = 0;
    for (const auto* exec : matching) {
        hash = sched_lib::hash_mix_string(hash, exec->execution_id());
        hash = sched_lib::hash_mix(hash, exec->executed_at_ms());
        hash = sched_lib::hash_mix(hash, static_cast<uint64_t>(exec->status()));
    }

    result->set_state_hash(hash);
    result->set_execution_count(static_cast<int>(matching.size()));

    return grpc::Status::OK;
}

grpc::Status CloudSchedulerService::healthy(
    grpc::ServerContext* context,
    const sched::healthy_request* request,
    sched::healthy_response* response) {

    // Service is healthy as long as it's responding to requests
    // No lifecycle management - purely storage service
    response->set_is_healthy(true);
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
    job_sync_states_.clear();
    std::lock_guard<std::mutex> exec_lock(executions_mutex_);
    executions_.clear();
}

// =========================================================================
// Sync Protocol v2 Methods
// =========================================================================

uint64_t CloudSchedulerService::ComputeJobHash(const scheduler_types::job_t& job) {
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

// Note: HandleV2SyncMessage, ProcessVehicleJob, ProcessVehicleExecutions,
// SendV2SyncMessage have been removed. The CloudSchedulerSyncBridge now handles
// all sync protocol and transport operations. This service is purely storage/CRUD.

void CloudSchedulerService::JobInfoToRecord(
    const scheduler_types::job_t& job,
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
    scheduler_types::job_t* job) {

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

    // Authority and version
    job->set_authority(SyncV2ToAuthority(record.authority()));
    job->set_cloud_seq(record.version().cloud_seq());
    job->set_vehicle_seq(record.version().vehicle_seq());
}

// =============================================================================
// Internal API for Sync Bridge
// =============================================================================

grpc::Status CloudSchedulerService::get_jobs_for_vehicle(
    grpc::ServerContext* context,
    const sched::get_jobs_for_vehicle_request* request,
    sched::get_jobs_for_vehicle_response* response) {

    const auto& vehicle_id = request->vehicle_id();
    bool include_deleted = request->include_deleted();

    std::lock_guard<std::mutex> lock(jobs_mutex_);

    auto it = jobs_.find(vehicle_id);
    if (it == jobs_.end()) {
        // No jobs for this vehicle - return empty list
        return grpc::Status::OK;
    }

    for (const auto& [job_id, job] : it->second) {
        // Skip deleted jobs unless explicitly requested
        if (job.deleted() && !include_deleted) {
            continue;
        }

        auto* job_info = response->add_jobs();
        *job_info = job;
        // Version is already in job_info_t (cloud_seq, vehicle_seq fields)
    }

    return grpc::Status::OK;
}

grpc::Status CloudSchedulerService::upsert_job(
    grpc::ServerContext* context,
    const sched::upsert_job_request* request,
    sched::upsert_job_response* response) {

    const auto& job = request->job();
    const auto& vehicle_id = job.vehicle_id();
    const auto& job_id = job.job_id();

    if (vehicle_id.empty() || job_id.empty()) {
        response->set_success(false);
        return grpc::Status::OK;
    }

    uint64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    {
        std::lock_guard<std::mutex> lock(jobs_mutex_);

        // Store/update the job (version is in job_info_t cloud_seq/vehicle_seq)
        auto& stored_job = jobs_[vehicle_id][job_id];
        stored_job = job;

        // Ensure updated_at is set
        if (stored_job.updated_at_ms() == 0) {
            stored_job.set_updated_at_ms(now_ms);
        }

        // Vehicle sent this job in V2C - that confirms vehicle has it
        // Mark as SYNCED (vehicle confirmed receipt)
        job_sync_states_[vehicle_id][job_id] = scheduler_types_pb::SYNC_SYNCED;

        // Copy to response
        auto* updated = response->mutable_updated_job();
        *updated = stored_job;
    }

    // Recompute cloud checksum after job change
    UpdateCloudChecksum(vehicle_id);

    response->set_success(true);
    LOG(INFO) << "Upserted job " << job_id << " for vehicle " << vehicle_id;
    return grpc::Status::OK;
}

grpc::Status CloudSchedulerService::record_execution(
    grpc::ServerContext* context,
    const sched::record_execution_request* request,
    sched::record_execution_response* response) {

    const auto& vehicle_id = request->vehicle_id();
    const auto& job_id = request->job_id();
    const auto& execution = request->execution();

    if (vehicle_id.empty() || job_id.empty() || execution.execution_id().empty()) {
        response->set_success(false);
        return grpc::Status::OK;
    }

    {
        std::lock_guard<std::mutex> lock(executions_mutex_);

        auto& job_executions = executions_[vehicle_id][job_id];

        // Check for duplicate execution_id (idempotent)
        for (const auto& existing : job_executions) {
            if (existing.execution_id() == execution.execution_id()) {
                response->set_success(true);
                response->set_is_duplicate(true);
                return grpc::Status::OK;
            }
        }

        // Add the execution record
        job_executions.push_back(execution);
    }

    // Update job's last_executed_ms
    {
        std::lock_guard<std::mutex> lock(jobs_mutex_);
        auto vehicle_it = jobs_.find(vehicle_id);
        if (vehicle_it != jobs_.end()) {
            auto job_it = vehicle_it->second.find(job_id);
            if (job_it != vehicle_it->second.end()) {
                job_it->second.set_last_executed_ms(execution.executed_at_ms());
            }
        }
    }

    response->set_success(true);
    response->set_is_duplicate(false);
    LOG(INFO) << "Recorded execution " << execution.execution_id()
              << " for job " << job_id << " on vehicle " << vehicle_id;
    return grpc::Status::OK;
}

grpc::Status CloudSchedulerService::get_vehicle_sync_state(
    grpc::ServerContext* context,
    const sched::get_vehicle_sync_state_request* request,
    sched::get_vehicle_sync_state_response* response) {

    const auto& vehicle_id = request->vehicle_id();

    std::lock_guard<std::mutex> lock(sync_state_mutex_);

    auto it = vehicle_sync_states_.find(vehicle_id);
    if (it == vehicle_sync_states_.end()) {
        // Initialize sync state for new vehicle
        sched::vehicle_sync_state_t state;
        state.set_vehicle_id(vehicle_id);
        state.set_cloud_checksum(ComputeStateChecksum(vehicle_id));
        state.set_last_seen_v2c_checksum(0);
        state.set_last_sync_timestamp_ms(0);

        vehicle_sync_states_[vehicle_id] = state;
        *response->mutable_state() = state;
        response->set_found(false);
    } else {
        *response->mutable_state() = it->second;
        response->set_found(true);
    }

    return grpc::Status::OK;
}

grpc::Status CloudSchedulerService::update_vehicle_sync_state(
    grpc::ServerContext* context,
    const sched::update_vehicle_sync_state_request* request,
    sched::update_vehicle_sync_state_response* response) {

    const auto& vehicle_id = request->vehicle_id();
    uint64_t last_seen_v2c_checksum = request->last_seen_v2c_checksum();

    uint64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    {
        std::lock_guard<std::mutex> lock(sync_state_mutex_);

        auto& state = vehicle_sync_states_[vehicle_id];
        state.set_vehicle_id(vehicle_id);
        state.set_last_seen_v2c_checksum(last_seen_v2c_checksum);
        state.set_last_sync_timestamp_ms(now_ms);
        // cloud_checksum is maintained internally, don't overwrite
        if (state.cloud_checksum() == 0) {
            state.set_cloud_checksum(ComputeStateChecksum(vehicle_id));
        }
    }

    response->set_success(true);
    return grpc::Status::OK;
}

grpc::Status CloudSchedulerService::get_pending_syncs(
    grpc::ServerContext* context,
    const sched::get_pending_syncs_request* request,
    sched::get_pending_syncs_response* response) {

    int32_t limit = request->limit();
    int32_t count = 0;

    std::lock_guard<std::mutex> lock(sync_state_mutex_);

    for (const auto& [vehicle_id, state] : vehicle_sync_states_) {
        // Vehicle needs sync if checksums don't match
        if (state.cloud_checksum() != state.last_seen_v2c_checksum()) {
            auto* pending = response->add_pending_vehicles();
            pending->CopyFrom(state);

            count++;
            if (limit > 0 && count >= limit) {
                break;
            }
        }
    }

    VLOG(1) << "get_pending_syncs: found " << count << " vehicles needing sync";
    return grpc::Status::OK;
}

void CloudSchedulerService::UpdateCloudChecksum(const std::string& vehicle_id) {
    uint64_t checksum = ComputeStateChecksum(vehicle_id);

    std::lock_guard<std::mutex> lock(sync_state_mutex_);
    auto& state = vehicle_sync_states_[vehicle_id];
    state.set_vehicle_id(vehicle_id);
    state.set_cloud_checksum(checksum);
}

}  // namespace ifex::cloud
