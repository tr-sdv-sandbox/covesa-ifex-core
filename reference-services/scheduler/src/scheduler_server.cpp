#include "scheduler_server.hpp"
#include <ifex/network.hpp>
#include <iomanip>
#include <sstream>
#include <ctime>
#include <regex>
#include <cstdlib>

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

void Job::ToProto(swdv::ifex_scheduler::job_t* proto) const {
    proto->set_id(id);
    proto->set_title(title);
    proto->set_service(service_name);
    proto->set_method(method_name);
    proto->set_parameters(parameters.dump());
    proto->set_scheduled_time(TimePointToISO8601(scheduled_time));

    if (!recurrence_rule.empty()) {
        proto->set_recurrence_rule(recurrence_rule);
    }

    if (end_time.has_value()) {
        proto->set_end_time(TimePointToISO8601(end_time.value()));
    }

    proto->set_status(status);
    proto->set_created_at(TimePointToISO8601(created_at));
    proto->set_updated_at(TimePointToISO8601(updated_at));

    if (executed_at.has_value()) {
        proto->set_executed_at(TimePointToISO8601(executed_at.value()));
    }

    if (next_run_time.has_value()) {
        proto->set_next_run_time(TimePointToISO8601(next_run_time.value()));
    }

    if (error_message.has_value()) {
        proto->set_error_message(error_message.value());
    }

    if (result.has_value()) {
        proto->set_result(result.value());
    }

    if (!service_address.empty()) {
        proto->set_service_address(service_address);
    }
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

    job->scheduled_time = ISO8601ToTimePoint(proto.scheduled_time());

    if (!proto.recurrence_rule().empty()) {
        job->recurrence_rule = proto.recurrence_rule();
    }

    if (!proto.end_time().empty()) {
        job->end_time = ISO8601ToTimePoint(proto.end_time());
    }

    if (!proto.service_address().empty()) {
        job->service_address = proto.service_address();
    }

    auto now = std::chrono::system_clock::now();
    job->created_at = now;
    job->updated_at = now;

    return job;
}

SchedulerServer::SchedulerServer(const std::string& service_discovery_endpoint)
    : discovery_endpoint_(service_discovery_endpoint) {
    if (service_discovery_endpoint.empty()) {
        LOG(FATAL) << "Service discovery endpoint cannot be empty";
    }

    LOG(INFO) << "IFEX Scheduler Service initialized";
    LOG(INFO) << "  - Calendar-style job scheduling";
    LOG(INFO) << "  - CRUD operations for jobs";
    LOG(INFO) << "  - Cron expression support";
    LOG(INFO) << "  - Dynamic service invocation via dispatcher";

    // Initialize service discovery client using ifex-core API
    LOG(INFO) << "Connecting to service discovery at: " << service_discovery_endpoint;
    discovery_client_ = ifex::DiscoveryClient::create(service_discovery_endpoint);
}

SchedulerServer::~SchedulerServer() {
    StopExecutor();
}

grpc::Status SchedulerServer::create_job(grpc::ServerContext* context,
                                         const swdv::ifex_scheduler::create_job_request* request,
                                         swdv::ifex_scheduler::create_job_response* response) {
    LOG(INFO) << "CREATE JOB REQUEST:";
    LOG(INFO) << "  Title: " << request->job().title();
    LOG(INFO) << "  Service: " << request->job().service() << "." << request->job().method();
    LOG(INFO) << "  Scheduled: " << request->job().scheduled_time();
    LOG(INFO) << "  Recurrence: " << request->job().recurrence_rule();

    try {
        // Create job from request
        auto job = Job::FromProto(request->job());
        job->id = GenerateJobId();

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

        if (!updates.scheduled_time().empty()) {
            job->scheduled_time = ISO8601ToTimePoint(updates.scheduled_time());
        }

        if (!updates.recurrence_rule().empty()) {
            job->recurrence_rule = updates.recurrence_rule();
            // Recalculate next run time
            job->next_run_time = CalculateNextRunTime(*job, std::chrono::system_clock::now());
        }

        if (!updates.end_time().empty()) {
            job->end_time = ISO8601ToTimePoint(updates.end_time());
        }

        if (!updates.parameters().empty()) {
            job->parameters = json::parse(updates.parameters());
        }

        if (!updates.service_address().empty()) {
            job->service_address = updates.service_address();
        }

        job->updated_at = std::chrono::system_clock::now();

        LOG(INFO) << "Updated job " << request->job_id();
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
        std::lock_guard<std::mutex> lock(jobs_mutex_);

        auto it = jobs_.find(request->job_id());
        if (it != jobs_.end()) {
            jobs_.erase(it);
            LOG(INFO) << "Deleted job " << request->job_id();
            response->set_success(true);
            response->set_message("Job deleted successfully");
        } else {
            response->set_success(false);
            response->set_message("Job not found");
        }

        return grpc::Status::OK;

    } catch (const std::exception& e) {
        LOG(ERROR) << "Failed to delete job: " << e.what();
        response->set_success(false);
        response->set_message(std::string("Failed to delete job: ") + e.what());
        return grpc::Status::OK;
    }
}

grpc::Status SchedulerServer::get_calendar_view(grpc::ServerContext* context,
                                                const swdv::ifex_scheduler::get_calendar_view_request* request,
                                                swdv::ifex_scheduler::get_calendar_view_response* response) {
    LOG(INFO) << "GET CALENDAR VIEW REQUEST:";
    LOG(INFO) << "  View type: " << request->view_type();
    LOG(INFO) << "  Date: " << request->date();

    try {
        // Calculate date range based on view type
        auto [start_time, end_time] = GetCalendarViewRange(request->view_type(), request->date());

        response->set_start_date(TimePointToISO8601(start_time));
        response->set_end_date(TimePointToISO8601(end_time));

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

        // Find jobs ready to execute
        {
            std::lock_guard<std::mutex> lock(jobs_mutex_);
            for (auto& [job_id, job] : jobs_) {
                if (job->status == swdv::ifex_scheduler::PENDING &&
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
        job->status = swdv::ifex_scheduler::RUNNING;
        job->updated_at = std::chrono::system_clock::now();
    }

    try {
        // Call the service method
        bool success = CallServiceMethod(job);

        std::lock_guard<std::mutex> lock(jobs_mutex_);
        if (success) {
            job->status = swdv::ifex_scheduler::COMPLETED;
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
                    new_job->status = swdv::ifex_scheduler::PENDING;
                    new_job->created_at = std::chrono::system_clock::now();
                    new_job->updated_at = new_job->created_at;
                    new_job->executed_at = std::nullopt;
                    new_job->error_message = std::nullopt;

                    jobs_[new_job->id] = std::move(new_job);

                    LOG(INFO) << "Scheduled next occurrence";
                }
            }
        } else {
            job->status = swdv::ifex_scheduler::FAILED;
            job->updated_at = std::chrono::system_clock::now();
            LOG(ERROR) << "Job " << job->id << " failed";
        }

    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(jobs_mutex_);
        job->status = swdv::ifex_scheduler::FAILED;
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
    // Check date range
    if (!filter.start_date().empty()) {
        auto start_time = ISO8601ToTimePoint(filter.start_date());
        if (job.scheduled_time < start_time) {
            return false;
        }
    }

    if (!filter.end_date().empty()) {
        auto end_time = ISO8601ToTimePoint(filter.end_date());
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
    if (!filter.include_completed() && job.status == swdv::ifex_scheduler::COMPLETED) {
        return false;
    }

    return true;
}

std::pair<std::chrono::system_clock::time_point, std::chrono::system_clock::time_point>
SchedulerServer::GetCalendarViewRange(swdv::ifex_scheduler::view_type_t view_type,
                                      const std::string& date) {
    auto reference_time = ISO8601ToTimePoint(date);

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
