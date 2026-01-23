#include "scheduler_server.hpp"
#include <ifex/network.hpp>
#include <iomanip>
#include <sstream>
#include <ctime>
#include <regex>
#include <cstdlib>
#include <fstream>
#include <filesystem>

namespace ifex::reference {

// Helper to convert time_point to ISO 8601 string
std::string TimePointToISO8601(const std::chrono::system_clock::time_point& tp) {
    auto time_t = std::chrono::system_clock::to_time_t(tp);
    std::stringstream ss;
    ss << std::put_time(std::gmtime(&time_t), "%Y-%m-%dT%H:%M:%SZ");
    return ss.str();
}

// Helper to parse ISO 8601 string to time_point
std::chrono::system_clock::time_point ISO8601ToTimePoint(const std::string& iso_str) {
    std::tm tm = {};
    std::istringstream ss(iso_str);

    // Try parsing with 'Z' suffix
    if (iso_str.back() == 'Z') {
        ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    } else {
        ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    }

    if (ss.fail()) {
        throw std::runtime_error("Failed to parse ISO 8601 datetime: " + iso_str);
    }

    return std::chrono::system_clock::from_time_t(timegm(&tm));
}

// Helper to convert time_point to milliseconds since epoch
static uint64_t TimePointToMs(const std::chrono::system_clock::time_point& tp) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        tp.time_since_epoch()).count();
}

// Helper to convert milliseconds since epoch to time_point
static std::chrono::system_clock::time_point MsToTimePoint(uint64_t ms) {
    return std::chrono::system_clock::time_point(
        std::chrono::milliseconds(ms));
}

void Job::ToProto(swdv::ifex_scheduler::job_t* proto) const {
    proto->set_id(id);
    proto->set_title(title);
    proto->set_service(service_name);
    proto->set_method(method_name);
    proto->set_parameters(parameters.dump());
    proto->set_scheduled_time_ms(TimePointToMs(scheduled_time));

    if (!recurrence_rule.empty()) {
        proto->set_recurrence_rule(recurrence_rule);
    }

    if (end_time.has_value()) {
        proto->set_end_time_ms(TimePointToMs(end_time.value()));
    }

    proto->set_status(status);
    proto->set_paused(paused);
    proto->set_created_at_ms(TimePointToMs(created_at));
    proto->set_updated_at_ms(TimePointToMs(updated_at));

    if (executed_at.has_value()) {
        proto->set_executed_at_ms(TimePointToMs(executed_at.value()));
    }

    if (next_run_time.has_value()) {
        proto->set_next_run_time_ms(TimePointToMs(next_run_time.value()));
    }

    if (error_message.has_value()) {
        proto->set_error_message(error_message.value());
    }

    if (result.has_value()) {
        proto->set_result(result.value());
    }

    proto->set_wake_policy(wake_policy);
    proto->set_sleep_policy(sleep_policy);
    proto->set_wake_lead_time_s(wake_lead_time_s);

    VLOG(1) << "Job::ToProto: job=" << id
            << " scheduled_time_ms=" << TimePointToMs(scheduled_time)
            << " wake_policy=" << static_cast<int>(wake_policy);
}

std::unique_ptr<Job> Job::FromProto(const swdv::ifex_scheduler::job_create_t& proto) {
    auto job = std::make_unique<Job>();

    job->title = proto.title();
    job->service_name = proto.service();
    job->method_name = proto.method();

    // Parse parameters JSON
    if (!proto.parameters().empty()) {
        try {
            job->parameters = json::parse(proto.parameters());
        } catch (const json::exception& e) {
            LOG(ERROR) << "Failed to parse job parameters: " << e.what();
            job->parameters = json::object();
        }
    }

    // Convert epoch milliseconds to time_point
    if (proto.scheduled_time_ms() == 0) {
        throw std::runtime_error("scheduled_time_ms is required and must be non-zero");
    }
    job->scheduled_time = MsToTimePoint(proto.scheduled_time_ms());

    if (!proto.recurrence_rule().empty()) {
        job->recurrence_rule = proto.recurrence_rule();
    }

    if (proto.end_time_ms() > 0) {
        job->end_time = MsToTimePoint(proto.end_time_ms());
    }

    job->wake_policy = proto.wake_policy();
    job->sleep_policy = proto.sleep_policy();
    job->wake_lead_time_s = proto.wake_lead_time_s();
    job->paused = proto.paused();

    auto now = std::chrono::system_clock::now();
    job->created_at = now;
    job->updated_at = now;

    return job;
}

json Job::ToJson() const {
    json j;
    j["id"] = id;
    j["title"] = title;
    j["service_name"] = service_name;
    j["method_name"] = method_name;
    j["parameters"] = parameters;
    j["scheduled_time_ms"] = TimePointToMs(scheduled_time);
    j["recurrence_rule"] = recurrence_rule;
    j["status"] = static_cast<int>(status);
    j["created_at_ms"] = TimePointToMs(created_at);
    j["updated_at_ms"] = TimePointToMs(updated_at);

    if (end_time.has_value()) {
        j["end_time_ms"] = TimePointToMs(end_time.value());
    }
    if (next_run_time.has_value()) {
        j["next_run_time_ms"] = TimePointToMs(next_run_time.value());
    }
    if (executed_at.has_value()) {
        j["executed_at_ms"] = TimePointToMs(executed_at.value());
    }
    if (error_message.has_value()) {
        j["error_message"] = error_message.value();
    }
    if (result.has_value()) {
        j["result"] = result.value();
    }

    j["wake_policy"] = static_cast<int>(wake_policy);
    j["sleep_policy"] = static_cast<int>(sleep_policy);
    j["wake_lead_time_s"] = wake_lead_time_s;

    j["paused"] = paused;

    // Sync v2 fields
    j["version_cloud_seq"] = version.cloud_seq;
    j["version_vehicle_seq"] = version.vehicle_seq;
    j["authority"] = static_cast<int>(authority);
    j["needs_sync"] = needs_sync;
    j["deleted"] = deleted;
    if (deleted_at.has_value()) {
        j["deleted_at_ms"] = TimePointToMs(deleted_at.value());
    }

    return j;
}

std::unique_ptr<Job> Job::FromJson(const json& j) {
    auto job = std::make_unique<Job>();

    job->id = j.at("id").get<std::string>();
    job->title = j.at("title").get<std::string>();
    job->service_name = j.at("service_name").get<std::string>();
    job->method_name = j.at("method_name").get<std::string>();
    job->parameters = j.value("parameters", json::object());

    // Support both old (string) and new (uint64) formats for backward compatibility
    if (j.contains("scheduled_time_ms")) {
        job->scheduled_time = MsToTimePoint(j.at("scheduled_time_ms").get<uint64_t>());
    } else if (j.contains("scheduled_time")) {
        job->scheduled_time = ISO8601ToTimePoint(j.at("scheduled_time").get<std::string>());
    }

    job->recurrence_rule = j.value("recurrence_rule", "");
    job->status = static_cast<swdv::scheduler_types::job_status_t>(j.at("status").get<int>());

    if (j.contains("created_at_ms")) {
        job->created_at = MsToTimePoint(j.at("created_at_ms").get<uint64_t>());
    } else if (j.contains("created_at")) {
        job->created_at = ISO8601ToTimePoint(j.at("created_at").get<std::string>());
    }

    if (j.contains("updated_at_ms")) {
        job->updated_at = MsToTimePoint(j.at("updated_at_ms").get<uint64_t>());
    } else if (j.contains("updated_at")) {
        job->updated_at = ISO8601ToTimePoint(j.at("updated_at").get<std::string>());
    }

    if (j.contains("end_time_ms")) {
        job->end_time = MsToTimePoint(j.at("end_time_ms").get<uint64_t>());
    } else if (j.contains("end_time")) {
        job->end_time = ISO8601ToTimePoint(j.at("end_time").get<std::string>());
    }

    if (j.contains("next_run_time_ms")) {
        job->next_run_time = MsToTimePoint(j.at("next_run_time_ms").get<uint64_t>());
    } else if (j.contains("next_run_time")) {
        job->next_run_time = ISO8601ToTimePoint(j.at("next_run_time").get<std::string>());
    }

    if (j.contains("executed_at_ms")) {
        job->executed_at = MsToTimePoint(j.at("executed_at_ms").get<uint64_t>());
    } else if (j.contains("executed_at")) {
        job->executed_at = ISO8601ToTimePoint(j.at("executed_at").get<std::string>());
    }

    if (j.contains("error_message")) {
        job->error_message = j.at("error_message").get<std::string>();
    }
    if (j.contains("result")) {
        job->result = j.at("result").get<std::string>();
    }

    // Wake/Sleep policies (with defaults for backward compatibility)
    job->wake_policy = static_cast<swdv::scheduler_types::wake_policy_t>(
        j.value("wake_policy", 0));
    job->sleep_policy = static_cast<swdv::scheduler_types::sleep_policy_t>(
        j.value("sleep_policy", 0));
    job->wake_lead_time_s = j.value("wake_lead_time_s", 0u);

    job->paused = j.value("paused", false);

    // Sync v2 fields (with defaults for backward compatibility)
    job->version.cloud_seq = j.value("version_cloud_seq", 0ULL);
    job->version.vehicle_seq = j.value("version_vehicle_seq", 0ULL);
    job->authority = static_cast<swdv::scheduler_sync_v2::JobAuthority>(
        j.value("authority", static_cast<int>(swdv::scheduler_sync_v2::AUTHORITY_VEHICLE)));
    job->needs_sync = j.value("needs_sync", false);
    job->deleted = j.value("deleted", false);
    if (j.contains("deleted_at_ms")) {
        job->deleted_at = MsToTimePoint(j.at("deleted_at_ms").get<uint64_t>());
    } else if (j.contains("deleted_at")) {
        job->deleted_at = ISO8601ToTimePoint(j.at("deleted_at").get<std::string>());
    }

    return job;
}

void Job::ToSyncProto(swdv::scheduler_sync_v2::JobRecord* proto) const {
    proto->set_job_id(id);
    proto->set_authority(authority);

    // Version vector
    auto* ver = proto->mutable_version();
    ver->set_cloud_seq(version.cloud_seq);
    ver->set_vehicle_seq(version.vehicle_seq);

    proto->set_deleted(deleted);
    if (deleted_at.has_value()) {
        proto->set_deleted_at_ms(TimePointToMs(deleted_at.value()));
    }

    // Content
    proto->set_title(title);
    proto->set_service(service_name);
    proto->set_method(method_name);
    proto->set_parameters_json(parameters.dump());
    proto->set_scheduled_time_ms(TimePointToMs(scheduled_time));
    proto->set_recurrence_rule(recurrence_rule);
    if (end_time.has_value()) {
        proto->set_end_time_ms(TimePointToMs(end_time.value()));
    }
    proto->set_paused(paused);

    // Execution state (map scheduler status to sync status)
    switch (status) {
        case swdv::scheduler_types::JOB_STATUS_PENDING:
            proto->set_status(swdv::scheduler_sync_v2::JOB_STATUS_PENDING);
            break;
        case swdv::scheduler_types::JOB_STATUS_RUNNING:
            proto->set_status(swdv::scheduler_sync_v2::JOB_STATUS_RUNNING);
            break;
        case swdv::scheduler_types::JOB_STATUS_COMPLETED:
            proto->set_status(swdv::scheduler_sync_v2::JOB_STATUS_COMPLETED);
            break;
        case swdv::scheduler_types::JOB_STATUS_FAILED:
            proto->set_status(swdv::scheduler_sync_v2::JOB_STATUS_FAILED);
            break;
        case swdv::scheduler_types::JOB_STATUS_CANCELLED:
            proto->set_status(swdv::scheduler_sync_v2::JOB_STATUS_CANCELLED);
            break;
    }

    if (next_run_time.has_value()) {
        proto->set_next_run_time_ms(TimePointToMs(next_run_time.value()));
    }
    if (executed_at.has_value()) {
        proto->set_last_executed_ms(TimePointToMs(executed_at.value()));
    }

    // Power management
    proto->set_wake_policy(wake_policy == swdv::scheduler_types::WAKE_REQUIRED
        ? swdv::scheduler_sync_v2::WAKE_REQUIRED
        : swdv::scheduler_sync_v2::WAKE_NO_WAKE);
    proto->set_sleep_policy(sleep_policy == swdv::scheduler_types::SLEEP_INHIBIT
        ? swdv::scheduler_sync_v2::SLEEP_INHIBIT
        : swdv::scheduler_sync_v2::SLEEP_NORMAL);
    proto->set_wake_lead_time_s(wake_lead_time_s);

    // Metadata
    proto->set_created_at_ms(TimePointToMs(created_at));
    proto->set_updated_at_ms(TimePointToMs(updated_at));
}

std::unique_ptr<Job> Job::FromSyncProto(const swdv::scheduler_sync_v2::JobRecord& proto) {
    auto job = std::make_unique<Job>();

    job->id = proto.job_id();
    job->authority = proto.authority();

    // Version vector
    job->version.cloud_seq = proto.version().cloud_seq();
    job->version.vehicle_seq = proto.version().vehicle_seq();

    job->deleted = proto.deleted();
    if (proto.deleted_at_ms() > 0) {
        job->deleted_at = MsToTimePoint(proto.deleted_at_ms());
    }

    // Content
    job->title = proto.title();
    job->service_name = proto.service();
    job->method_name = proto.method();
    if (!proto.parameters_json().empty()) {
        try {
            job->parameters = json::parse(proto.parameters_json());
        } catch (const json::exception& e) {
            LOG(ERROR) << "Failed to parse job parameters from sync: " << e.what();
            job->parameters = json::object();
        }
    }
    job->scheduled_time = MsToTimePoint(proto.scheduled_time_ms());
    job->recurrence_rule = proto.recurrence_rule();
    if (proto.end_time_ms() > 0) {
        job->end_time = MsToTimePoint(proto.end_time_ms());
    }
    job->paused = proto.paused();

    // Execution state (map sync status to scheduler status)
    switch (proto.status()) {
        case swdv::scheduler_sync_v2::JOB_STATUS_PENDING:
            job->status = swdv::scheduler_types::JOB_STATUS_PENDING;
            break;
        case swdv::scheduler_sync_v2::JOB_STATUS_RUNNING:
            job->status = swdv::scheduler_types::JOB_STATUS_RUNNING;
            break;
        case swdv::scheduler_sync_v2::JOB_STATUS_COMPLETED:
            job->status = swdv::scheduler_types::JOB_STATUS_COMPLETED;
            break;
        case swdv::scheduler_sync_v2::JOB_STATUS_FAILED:
            job->status = swdv::scheduler_types::JOB_STATUS_FAILED;
            break;
        case swdv::scheduler_sync_v2::JOB_STATUS_CANCELLED:
            job->status = swdv::scheduler_types::JOB_STATUS_CANCELLED;
            break;
    }

    if (proto.next_run_time_ms() > 0) {
        job->next_run_time = MsToTimePoint(proto.next_run_time_ms());
    }
    if (proto.last_executed_ms() > 0) {
        job->executed_at = MsToTimePoint(proto.last_executed_ms());
    }

    // Power management
    job->wake_policy = (proto.wake_policy() == swdv::scheduler_sync_v2::WAKE_REQUIRED)
        ? swdv::scheduler_types::WAKE_REQUIRED
        : swdv::scheduler_types::WAKE_NO_WAKE;
    job->sleep_policy = (proto.sleep_policy() == swdv::scheduler_sync_v2::SLEEP_INHIBIT)
        ? swdv::scheduler_types::SLEEP_INHIBIT
        : swdv::scheduler_types::SLEEP_NORMAL;
    job->wake_lead_time_s = proto.wake_lead_time_s();

    // Metadata
    job->created_at = MsToTimePoint(proto.created_at_ms());
    job->updated_at = MsToTimePoint(proto.updated_at_ms());

    return job;
}

SchedulerServer::SchedulerServer(const std::string& service_discovery_endpoint)
    : SchedulerServer(Config{service_discovery_endpoint, ""}) {
}

SchedulerServer::SchedulerServer(const Config& config)
    : discovery_endpoint_(config.discovery_endpoint),
      persistence_dir_(config.persistence_dir) {
    if (config.discovery_endpoint.empty()) {
        LOG(FATAL) << "Service discovery endpoint cannot be empty";
    }

    LOG(INFO) << "IFEX Scheduler Service initialized";
    LOG(INFO) << "  - Calendar-style job scheduling";
    LOG(INFO) << "  - CRUD operations for jobs";
    LOG(INFO) << "  - Cron expression support";
    LOG(INFO) << "  - Dynamic service invocation via dispatcher";
    if (!persistence_dir_.empty()) {
        LOG(INFO) << "  - Persistence enabled: " << persistence_dir_;
    }

    // Initialize service discovery client using ifex-core API
    LOG(INFO) << "Connecting to service discovery at: " << config.discovery_endpoint;
    discovery_client_ = ifex::DiscoveryClient::create(config.discovery_endpoint);

    // Load persisted jobs if persistence is enabled
    if (!persistence_dir_.empty()) {
        LoadJobs();
    }
}

SchedulerServer::~SchedulerServer() {
    // Save jobs before shutdown if persistence is enabled
    if (!persistence_dir_.empty()) {
        SaveJobs();
    }
    StopExecutor();
}

std::string SchedulerServer::GetPersistenceFilePath() const {
    return persistence_dir_ + "/scheduler_jobs.json";
}

void SchedulerServer::SaveJobs() {
    if (persistence_dir_.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(jobs_mutex_);

    try {
        // Create directory if it doesn't exist
        std::filesystem::create_directories(persistence_dir_);

        json jobs_array = json::array();
        for (const auto& [id, job] : jobs_) {
            jobs_array.push_back(job->ToJson());
        }

        json root;
        root["version"] = 1;
        root["job_counter"] = job_counter_.load();
        root["jobs"] = jobs_array;

        std::string filepath = GetPersistenceFilePath();
        std::ofstream file(filepath);
        if (!file.is_open()) {
            LOG(ERROR) << "Failed to open persistence file for writing: " << filepath;
            return;
        }

        file << root.dump(2);
        file.close();

        LOG(INFO) << "Saved " << jobs_.size() << " jobs to " << filepath;

    } catch (const std::exception& e) {
        LOG(ERROR) << "Failed to save jobs: " << e.what();
    }
}

void SchedulerServer::LoadJobs() {
    if (persistence_dir_.empty()) {
        return;
    }

    std::string filepath = GetPersistenceFilePath();

    if (!std::filesystem::exists(filepath)) {
        LOG(INFO) << "No persistence file found at " << filepath;
        return;
    }

    try {
        std::ifstream file(filepath);
        if (!file.is_open()) {
            LOG(ERROR) << "Failed to open persistence file for reading: " << filepath;
            return;
        }

        json root = json::parse(file);
        file.close();

        int version = root.value("version", 1);
        if (version != 1) {
            LOG(WARNING) << "Unknown persistence file version: " << version;
        }

        job_counter_ = root.value("job_counter", 0);

        std::lock_guard<std::mutex> lock(jobs_mutex_);

        const auto& jobs_array = root.at("jobs");
        for (const auto& job_json : jobs_array) {
            auto job = Job::FromJson(job_json);
            std::string job_id = job->id;
            jobs_[job_id] = std::move(job);
        }

        LOG(INFO) << "Loaded " << jobs_.size() << " jobs from " << filepath;

    } catch (const std::exception& e) {
        LOG(ERROR) << "Failed to load jobs: " << e.what();
    }
}

grpc::Status SchedulerServer::create_job(grpc::ServerContext* context,
                                         const swdv::ifex_scheduler::create_job_request* request,
                                         swdv::ifex_scheduler::create_job_response* response) {
    LOG(INFO) << "CREATE JOB REQUEST:";
    LOG(INFO) << "  Title: " << request->job().title();
    LOG(INFO) << "  Service: " << request->job().service() << "." << request->job().method();
    LOG(INFO) << "  Scheduled (ms): " << request->job().scheduled_time_ms();
    LOG(INFO) << "  Recurrence: " << request->job().recurrence_rule();

    try {
        // Create job from request
        auto job = Job::FromProto(request->job());
        // Use provided job_id if present, otherwise generate one
        if (!request->job().job_id().empty()) {
            job->id = request->job().job_id();
        } else {
            job->id = GenerateJobId();
        }

        // Validate that the service exists via discovery
        // Note: For POC, we skip x-scheduling validation - any discoverable service can be scheduled
        try {
            auto service_info = discovery_client_->get_service(job->service_name);
            if (!service_info.has_value()) {
                throw std::runtime_error("Service not found: " + job->service_name);
            }

            // Check if method exists
            bool method_found = false;
            for (const auto& method : service_info->methods) {
                if (method.method_name == job->method_name) {
                    method_found = true;
                    break;
                }
            }

            if (!method_found) {
                throw std::runtime_error("Method not found: " + job->method_name);
            }

            LOG(INFO) << "  Service validated: " << job->service_name << " at " << service_info->endpoint.address;

        } catch (const std::exception& e) {
            LOG(ERROR) << "Could not validate service: " << e.what();
            throw std::runtime_error("Service validation failed: " + std::string(e.what()));
        }

        // Calculate next run time if recurring
        if (!job->recurrence_rule.empty()) {
            job->next_run_time = CalculateNextRunTime(*job, std::chrono::system_clock::now());
        }

        // Store job ID before moving the job
        std::string job_id = job->id;

        // Store job
        {
            std::lock_guard<std::mutex> lock(jobs_mutex_);
            jobs_[job_id] = std::move(job);
        }

        LOG(INFO) << "Created job " << job_id;

        // Persist immediately for durability
        if (!persistence_dir_.empty()) {
            SaveJobs();
        }

        response->set_success(true);
        response->set_job_id(job_id);
        response->set_message("Job created successfully");

        return grpc::Status::OK;

    } catch (const std::exception& e) {
        LOG(ERROR) << "Failed to create job: " << e.what();
        response->set_success(false);
        response->set_job_id("");
        response->set_message(std::string("Failed to create job: ") + e.what());
        return grpc::Status::OK;
    }
}

grpc::Status SchedulerServer::get_jobs(grpc::ServerContext* context,
                                       const swdv::ifex_scheduler::get_jobs_request* request,
                                       swdv::ifex_scheduler::get_jobs_response* response) {
    LOG(INFO) << "GET JOBS REQUEST";

    try {
        std::lock_guard<std::mutex> lock(jobs_mutex_);

        for (const auto& [job_id, job] : jobs_) {
            if (request->has_filter() && !MatchesFilter(*job, request->filter())) {
                continue;
            }

            auto* proto_job = response->add_jobs();
            job->ToProto(proto_job);
        }

        LOG(INFO) << "  Returning " << response->jobs_size() << " jobs";
        response->set_success(true);
        return grpc::Status::OK;

    } catch (const std::exception& e) {
        LOG(ERROR) << "Failed to get jobs: " << e.what();
        response->set_success(false);
        return grpc::Status::OK;
    }
}

grpc::Status SchedulerServer::get_job(grpc::ServerContext* context,
                                      const swdv::ifex_scheduler::get_job_request* request,
                                      swdv::ifex_scheduler::get_job_response* response) {
    LOG(INFO) << "GET JOB REQUEST: " << request->job_id();

    try {
        std::lock_guard<std::mutex> lock(jobs_mutex_);

        auto it = jobs_.find(request->job_id());
        if (it != jobs_.end()) {
            auto* proto_job = response->mutable_job();
            it->second->ToProto(proto_job);
            response->set_success(true);
        } else {
            response->set_success(false);
            response->set_message("Job not found");
        }

        return grpc::Status::OK;

    } catch (const std::exception& e) {
        LOG(ERROR) << "Failed to get job: " << e.what();
        response->set_success(false);
        response->set_message(std::string("Failed to get job: ") + e.what());
        return grpc::Status::OK;
    }
}

grpc::Status SchedulerServer::update_job(grpc::ServerContext* context,
                                         const swdv::ifex_scheduler::update_job_request* request,
                                         swdv::ifex_scheduler::update_job_response* response) {
    LOG(INFO) << "UPDATE JOB REQUEST: " << request->job_id();

    try {
        bool updated = false;
        {
            std::lock_guard<std::mutex> lock(jobs_mutex_);

            auto it = jobs_.find(request->job_id());
            if (it == jobs_.end()) {
                response->set_success(false);
                response->set_message("Job not found");
                return grpc::Status::OK;
            }

            auto& job = it->second;
            const auto& updates = request->updates();

            // Apply updates
            if (!updates.title().empty()) {
                job->title = updates.title();
            }

            if (updates.scheduled_time_ms() > 0) {
                job->scheduled_time = MsToTimePoint(updates.scheduled_time_ms());
            }

            if (!updates.recurrence_rule().empty()) {
                job->recurrence_rule = updates.recurrence_rule();
                // Recalculate next run time
                job->next_run_time = CalculateNextRunTime(*job, std::chrono::system_clock::now());
            }

            if (updates.end_time_ms() > 0) {
                job->end_time = MsToTimePoint(updates.end_time_ms());
            }

            if (!updates.parameters().empty()) {
                job->parameters = json::parse(updates.parameters());
            }

            // Always apply paused state from updates
            // (sync bridge always sends the current paused value)
            job->paused = updates.paused();

            job->updated_at = std::chrono::system_clock::now();
            updated = true;

            LOG(INFO) << "Updated job " << request->job_id()
                      << " paused=" << (job->paused ? "true" : "false");
        }  // Release jobs_mutex_ here

        // Persist immediately for durability
        if (updated && !persistence_dir_.empty()) {
            SaveJobs();
        }

        response->set_success(true);
        response->set_message("Job updated successfully");

        return grpc::Status::OK;

    } catch (const std::exception& e) {
        LOG(ERROR) << "Failed to update job: " << e.what();
        response->set_success(false);
        response->set_message(std::string("Failed to update job: ") + e.what());
        return grpc::Status::OK;
    }
}

grpc::Status SchedulerServer::delete_job(grpc::ServerContext* context,
                                         const swdv::ifex_scheduler::delete_job_request* request,
                                         swdv::ifex_scheduler::delete_job_response* response) {
    LOG(INFO) << "DELETE JOB REQUEST: " << request->job_id();

    try {
        bool deleted = false;
        {
            std::lock_guard<std::mutex> lock(jobs_mutex_);

            auto it = jobs_.find(request->job_id());
            if (it != jobs_.end()) {
                jobs_.erase(it);
                deleted = true;
                LOG(INFO) << "Deleted job " << request->job_id();
                response->set_success(true);
                response->set_message("Job deleted successfully");
            } else {
                response->set_success(false);
                response->set_message("Job not found");
            }
        }  // Release jobs_mutex_ here

        // Persist immediately for durability
        if (deleted && !persistence_dir_.empty()) {
            SaveJobs();
        }

        return grpc::Status::OK;

    } catch (const std::exception& e) {
        LOG(ERROR) << "Failed to delete job: " << e.what();
        response->set_success(false);
        response->set_message(std::string("Failed to delete job: ") + e.what());
        return grpc::Status::OK;
    }
}

grpc::Status SchedulerServer::pause_job(grpc::ServerContext* context,
                                        const swdv::ifex_scheduler::pause_job_request* request,
                                        swdv::ifex_scheduler::pause_job_response* response) {
    LOG(INFO) << "PAUSE JOB REQUEST: " << request->job_id();

    try {
        bool did_pause = false;
        {
            std::lock_guard<std::mutex> lock(jobs_mutex_);

            auto it = jobs_.find(request->job_id());
            if (it == jobs_.end()) {
                response->set_success(false);
                response->set_message("Job not found");
                return grpc::Status::OK;
            }

            auto& job = it->second;

            // Can only pause PENDING jobs
            if (job->status != swdv::scheduler_types::JOB_STATUS_PENDING) {
                response->set_success(false);
                response->set_message("Can only pause PENDING jobs, current status: " +
                                     std::to_string(static_cast<int>(job->status)));
                return grpc::Status::OK;
            }

            if (job->paused) {
                response->set_success(false);
                response->set_message("Job is already paused");
                return grpc::Status::OK;
            }

            job->paused = true;
            job->updated_at = std::chrono::system_clock::now();
            did_pause = true;

            LOG(INFO) << "Paused job " << request->job_id();
        }

        // Persist immediately
        if (did_pause && !persistence_dir_.empty()) {
            SaveJobs();
        }

        response->set_success(true);
        response->set_message("Job paused successfully");
        return grpc::Status::OK;

    } catch (const std::exception& e) {
        LOG(ERROR) << "Failed to pause job: " << e.what();
        response->set_success(false);
        response->set_message(std::string("Failed to pause job: ") + e.what());
        return grpc::Status::OK;
    }
}

grpc::Status SchedulerServer::resume_job(grpc::ServerContext* context,
                                         const swdv::ifex_scheduler::resume_job_request* request,
                                         swdv::ifex_scheduler::resume_job_response* response) {
    LOG(INFO) << "RESUME JOB REQUEST: " << request->job_id();

    try {
        bool did_resume = false;
        {
            std::lock_guard<std::mutex> lock(jobs_mutex_);

            auto it = jobs_.find(request->job_id());
            if (it == jobs_.end()) {
                response->set_success(false);
                response->set_message("Job not found");
                return grpc::Status::OK;
            }

            auto& job = it->second;

            // Can only resume paused jobs
            if (!job->paused) {
                response->set_success(false);
                response->set_message("Job is not paused");
                return grpc::Status::OK;
            }

            job->paused = false;
            job->updated_at = std::chrono::system_clock::now();
            did_resume = true;

            LOG(INFO) << "Resumed job " << request->job_id();
        }

        // Persist immediately
        if (did_resume && !persistence_dir_.empty()) {
            SaveJobs();
        }

        response->set_success(true);
        response->set_message("Job resumed successfully");
        return grpc::Status::OK;

    } catch (const std::exception& e) {
        LOG(ERROR) << "Failed to resume job: " << e.what();
        response->set_success(false);
        response->set_message(std::string("Failed to resume job: ") + e.what());
        return grpc::Status::OK;
    }
}

grpc::Status SchedulerServer::trigger_job(grpc::ServerContext* context,
                                          const swdv::ifex_scheduler::trigger_job_request* request,
                                          swdv::ifex_scheduler::trigger_job_response* response) {
    LOG(INFO) << "TRIGGER JOB REQUEST: " << request->job_id();

    try {
        Job* job_to_execute = nullptr;
        {
            std::lock_guard<std::mutex> lock(jobs_mutex_);

            auto it = jobs_.find(request->job_id());
            if (it == jobs_.end()) {
                response->set_success(false);
                response->set_message("Job not found");
                return grpc::Status::OK;
            }

            auto& job = it->second;

            // Can trigger PENDING jobs (whether paused or not)
            if (job->status != swdv::scheduler_types::JOB_STATUS_PENDING) {
                response->set_success(false);
                response->set_message("Can only trigger PENDING jobs, current status: " +
                                     std::to_string(static_cast<int>(job->status)));
                return grpc::Status::OK;
            }

            job_to_execute = job.get();
        }

        // Execute the job immediately (outside the lock)
        if (job_to_execute) {
            ExecuteJob(job_to_execute);

            // Persist after execution
            if (!persistence_dir_.empty()) {
                SaveJobs();
            }

            // Return the updated job
            std::lock_guard<std::mutex> lock(jobs_mutex_);
            auto it = jobs_.find(request->job_id());
            if (it != jobs_.end()) {
                auto* proto_job = response->mutable_job();
                it->second->ToProto(proto_job);
            }

            response->set_success(true);
            response->set_message("Job triggered successfully");
        }

        return grpc::Status::OK;

    } catch (const std::exception& e) {
        LOG(ERROR) << "Failed to trigger job: " << e.what();
        response->set_success(false);
        response->set_message(std::string("Failed to trigger job: ") + e.what());
        return grpc::Status::OK;
    }
}

grpc::Status SchedulerServer::get_calendar_view(grpc::ServerContext* context,
                                                const swdv::ifex_scheduler::get_calendar_view_request* request,
                                                swdv::ifex_scheduler::get_calendar_view_response* response) {
    LOG(INFO) << "GET CALENDAR VIEW REQUEST:";
    LOG(INFO) << "  View type: " << request->view_type();
    LOG(INFO) << "  Reference time (ms): " << request->reference_time_ms();

    try {
        // Calculate date range based on view type
        auto [start_time, end_time] = GetCalendarViewRange(request->view_type(), request->reference_time_ms());

        response->set_start_time_ms(TimePointToMs(start_time));
        response->set_end_time_ms(TimePointToMs(end_time));

        std::lock_guard<std::mutex> lock(jobs_mutex_);

        // Find all jobs in the date range
        for (const auto& [job_id, job] : jobs_) {
            // Check if job falls within date range
            bool in_range = false;

            // Check scheduled time
            if (job->scheduled_time >= start_time && job->scheduled_time < end_time) {
                in_range = true;
            }

            // Check recurring jobs
            if (!in_range && !job->recurrence_rule.empty()) {
                // Calculate if any occurrence falls within range
                auto check_time = start_time;
                while (check_time < end_time) {
                    auto next_run = CalculateNextRunTime(*job, check_time);
                    if (next_run.has_value() && next_run.value() < end_time) {
                        in_range = true;
                        break;
                    }
                    check_time = next_run.value_or(end_time);
                }
            }

            if (in_range) {
                auto* proto_job = response->add_jobs();
                job->ToProto(proto_job);
            }
        }

        LOG(INFO) << "  Returning " << response->jobs_size() << " jobs for calendar view";
        response->set_success(true);

        return grpc::Status::OK;

    } catch (const std::exception& e) {
        LOG(ERROR) << "Failed to get calendar view: " << e.what();
        response->set_success(false);
        return grpc::Status::OK;
    }
}

void SchedulerServer::StartExecutor() {
    running_ = true;
    executor_thread_ = std::thread(&SchedulerServer::JobExecutor, this);
    LOG(INFO) << "Job executor started";
}

void SchedulerServer::StopExecutor() {
    running_ = false;
    if (executor_thread_.joinable()) {
        executor_thread_.join();
    }
    LOG(INFO) << "Job executor stopped";
}

void SchedulerServer::PersistJobs() {
    if (!persistence_dir_.empty()) {
        SaveJobs();
    }
}

bool SchedulerServer::RegisterWithDiscovery(int port, const std::string& ifex_schema) {
    try {
        // Get primary IP address
        std::string primary_ip = ifex::network::get_primary_ip_address();
        if (primary_ip.empty()) {
            LOG(WARNING) << "Could not determine primary IP address, falling back to localhost";
            primary_ip = "localhost";
        }

        std::string endpoint_address = primary_ip + ":" + std::to_string(port);
        LOG(INFO) << "Using endpoint: " << endpoint_address;

        // Create service endpoint
        ifex::ServiceEndpoint endpoint;
        endpoint.address = endpoint_address;
        endpoint.transport = ifex::ServiceEndpoint::Transport::GRPC;

        registration_id_ = discovery_client_->register_service(endpoint, ifex_schema);

        if (!registration_id_.empty()) {
            LOG(INFO) << "Registered with service discovery";
            LOG(INFO) << "   Endpoint: " << endpoint_address;
            LOG(INFO) << "   Registration ID: " << registration_id_;

            // Start heartbeat thread
            std::thread([this]() {
                while (running_) {
                    try {
                        discovery_client_->send_heartbeat(registration_id_, ifex::ServiceStatus::AVAILABLE);
                    } catch (const std::exception& e) {
                        LOG(ERROR) << "Heartbeat failed: " << e.what();
                    }
                    std::this_thread::sleep_for(std::chrono::seconds(30));
                }
            }).detach();

            LOG(INFO) << "Heartbeat started";
            return true;
        }

    } catch (const std::exception& e) {
        LOG(ERROR) << "Failed to register with service discovery: " << e.what();
    }

    return false;
}

std::string SchedulerServer::GenerateJobId() {
    auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();

    return "job_" + std::to_string(timestamp) + "_" + std::to_string(job_counter_++);
}

void SchedulerServer::JobExecutor() {
    LOG(INFO) << "Job executor thread started";

    while (running_) {
        auto now = std::chrono::system_clock::now();
        std::vector<Job*> jobs_to_execute;

        // Find jobs ready to execute (PENDING, not paused, scheduled time has passed)
        {
            std::lock_guard<std::mutex> lock(jobs_mutex_);
            for (auto& [job_id, job] : jobs_) {
                if (job->status == swdv::scheduler_types::JOB_STATUS_PENDING &&
                    !job->paused &&
                    job->scheduled_time <= now) {
                    jobs_to_execute.push_back(job.get());
                }
            }
        }

        // Execute ready jobs
        for (auto* job : jobs_to_execute) {
            ExecuteJob(job);
        }

        // Sleep for 1 second before checking again
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    LOG(INFO) << "Job executor thread stopped";
}

void SchedulerServer::ExecuteJob(Job* job) {
    LOG(INFO) << "Executing job " << job->id << ": " << job->title;

    {
        std::lock_guard<std::mutex> lock(jobs_mutex_);
        job->status = swdv::scheduler_types::JOB_STATUS_RUNNING;
        job->updated_at = std::chrono::system_clock::now();
    }

    try {
        // Call the service method
        bool success = CallServiceMethod(job);

        std::lock_guard<std::mutex> lock(jobs_mutex_);
        if (success) {
            job->status = swdv::scheduler_types::JOB_STATUS_COMPLETED;
            job->executed_at = std::chrono::system_clock::now();
            job->updated_at = job->executed_at.value();
            LOG(INFO) << "Job " << job->id << " completed successfully";

            // Handle recurring jobs
            if (!job->recurrence_rule.empty()) {
                // Calculate next run time
                job->next_run_time = CalculateNextRunTime(*job, job->executed_at.value());

                if (job->next_run_time.has_value()) {
                    // Create new job for next occurrence
                    auto new_job = std::make_unique<Job>(*job);
                    new_job->id = GenerateJobId();
                    new_job->scheduled_time = job->next_run_time.value();
                    new_job->status = swdv::scheduler_types::JOB_STATUS_PENDING;
                    new_job->created_at = std::chrono::system_clock::now();
                    new_job->updated_at = new_job->created_at;
                    new_job->executed_at = std::nullopt;
                    new_job->error_message = std::nullopt;

                    jobs_[new_job->id] = std::move(new_job);

                    LOG(INFO) << "Scheduled next occurrence";
                }
            }
        } else {
            job->status = swdv::scheduler_types::JOB_STATUS_FAILED;
            job->updated_at = std::chrono::system_clock::now();
            LOG(ERROR) << "Job " << job->id << " failed";
        }

    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(jobs_mutex_);
        job->status = swdv::scheduler_types::JOB_STATUS_FAILED;
        job->error_message = e.what();
        job->updated_at = std::chrono::system_clock::now();
        LOG(ERROR) << "Job " << job->id << " failed with exception: " << e.what();
    }
}

bool SchedulerServer::CallServiceMethod(Job* job) {
    try {
        LOG(INFO) << "  Calling " << job->service_name << "." << job->method_name;
        LOG(INFO) << "  Parameters: " << job->parameters.dump(2);

        // Get or create dispatcher stub
        if (!dispatcher_stub_) {
            auto dispatcher_info = discovery_client_->get_service("ifex-dispatcher");
            if (!dispatcher_info.has_value()) {
                LOG(ERROR) << "  Could not find ifex_dispatcher service";
                job->error_message = "Dispatcher service not available";
                return false;
            }

            auto channel = grpc::CreateChannel(dispatcher_info->endpoint.address, grpc::InsecureChannelCredentials());
            dispatcher_stub_ = swdv::ifex_dispatcher::call_method_service::NewStub(channel);
            LOG(INFO) << "  Connected to dispatcher at: " << dispatcher_info->endpoint.address;
        }

        // Create call_method request for dispatcher
        swdv::ifex_dispatcher::call_method_request request;
        auto* call = request.mutable_call();
        call->set_service_name(job->service_name);
        call->set_method_name(job->method_name);
        call->set_parameters(job->parameters.dump());

        // Make the call through dispatcher
        grpc::ClientContext context;
        swdv::ifex_dispatcher::call_method_response response;

        auto status = dispatcher_stub_->call_method(&context, request, &response);

        if (status.ok() && response.result().status() == swdv::ifex_dispatcher::SUCCESS) {
            LOG(INFO) << "  Service call completed successfully via dispatcher!";
            LOG(INFO) << "  Response: " << response.result().response();
            job->result = response.result().response();  // Store the result
            return true;
        } else {
            std::string error_msg;
            if (!status.ok()) {
                error_msg = status.error_message();
            } else {
                error_msg = response.result().error_message();
            }
            LOG(ERROR) << "  Service call failed: " << error_msg;
            job->error_message = error_msg;
            return false;
        }

    } catch (const std::exception& e) {
        LOG(ERROR) << "  Service call failed: " << e.what();
        job->error_message = e.what();
        return false;
    }
}

std::optional<std::chrono::system_clock::time_point>
SchedulerServer::CalculateNextRunTime(const Job& job,
                                      const std::chrono::system_clock::time_point& after_time) {
    // Simple implementation - just add time based on pattern
    // TODO: Implement proper cron expression parsing

    if (job.recurrence_rule == "daily") {
        return after_time + std::chrono::hours(24);
    } else if (job.recurrence_rule == "weekly") {
        return after_time + std::chrono::hours(24 * 7);
    } else if (job.recurrence_rule == "hourly") {
        return after_time + std::chrono::hours(1);
    } else if (job.recurrence_rule == "minutely") {
        return after_time + std::chrono::minutes(1);
    }

    // TODO: Parse cron expressions
    return std::nullopt;
}

bool SchedulerServer::MatchesFilter(const Job& job,
                                    const swdv::ifex_scheduler::job_filter_t& filter) {
    // Check date range using _ms fields
    if (filter.start_time_ms() > 0) {
        auto start_time = MsToTimePoint(filter.start_time_ms());
        if (job.scheduled_time < start_time) {
            return false;
        }
    }

    if (filter.end_time_ms() > 0) {
        auto end_time = MsToTimePoint(filter.end_time_ms());
        if (job.scheduled_time >= end_time) {
            return false;
        }
    }

    // Check service filter
    if (!filter.service().empty() && job.service_name != filter.service()) {
        return false;
    }

    // Check status filter
    if (filter.has_status_filter() && job.status != filter.status()) {
        return false;
    }

    // Check completed filter
    if (!filter.include_completed() && job.status == swdv::scheduler_types::JOB_STATUS_COMPLETED) {
        return false;
    }

    // Check paused_only filter
    if (filter.paused_only() && !job.paused) {
        return false;
    }

    return true;
}

std::pair<std::chrono::system_clock::time_point, std::chrono::system_clock::time_point>
SchedulerServer::GetCalendarViewRange(swdv::ifex_scheduler::view_type_t view_type,
                                      uint64_t reference_time_ms) {
    auto reference_time = MsToTimePoint(reference_time_ms);

    // Get start of day
    auto time_t = std::chrono::system_clock::to_time_t(reference_time);
    std::tm tm = *std::localtime(&time_t);
    tm.tm_hour = 0;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    auto start_of_day = std::chrono::system_clock::from_time_t(std::mktime(&tm));

    switch (view_type) {
        case swdv::ifex_scheduler::DAY:
            return {start_of_day, start_of_day + std::chrono::hours(24)};

        case swdv::ifex_scheduler::WEEK: {
            // Find start of week (Monday)
            int days_since_monday = (tm.tm_wday == 0) ? 6 : tm.tm_wday - 1;
            auto start_of_week = start_of_day - std::chrono::hours(24 * days_since_monday);
            return {start_of_week, start_of_week + std::chrono::hours(24 * 7)};
        }

        case swdv::ifex_scheduler::MONTH: {
            // Start of month
            tm.tm_mday = 1;
            auto start_of_month = std::chrono::system_clock::from_time_t(std::mktime(&tm));

            // Start of next month
            tm.tm_mon++;
            if (tm.tm_mon > 11) {
                tm.tm_mon = 0;
                tm.tm_year++;
            }
            auto start_of_next_month = std::chrono::system_clock::from_time_t(std::mktime(&tm));

            return {start_of_month, start_of_next_month};
        }

        default:
            // Default to day view
            return {start_of_day, start_of_day + std::chrono::hours(24)};
    }
}

} // namespace ifex::reference
