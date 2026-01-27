#include "cloud_scheduler_sync_bridge.hpp"

#include <algorithm>
#include <chrono>
#include <random>
#include <sstream>

namespace ifex::cloud {

namespace sched_lib = ifex::scheduler;

// =============================================================================
// Helper Functions
// =============================================================================

static std::string GenerateInstanceId() {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dist;
    std::stringstream ss;
    ss << "bridge-" << std::hex << dist(gen);
    return ss.str();
}

static uint64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// Map sync v2 status to common job status
static scheduler_types::job_status_t SyncV2ToJobStatus(sync_v2::JobStatus status) {
    switch (status) {
        case sync_v2::JOB_STATUS_PENDING: return scheduler_types::JOB_STATUS_PENDING;
        case sync_v2::JOB_STATUS_RUNNING: return scheduler_types::JOB_STATUS_RUNNING;
        case sync_v2::JOB_STATUS_COMPLETED: return scheduler_types::JOB_STATUS_COMPLETED;
        case sync_v2::JOB_STATUS_FAILED: return scheduler_types::JOB_STATUS_FAILED;
        case sync_v2::JOB_STATUS_CANCELLED: return scheduler_types::JOB_STATUS_CANCELLED;
        default: return scheduler_types::JOB_STATUS_PENDING;
    }
}

// Map common job status to sync v2 status
static sync_v2::JobStatus JobStatusToSyncV2(scheduler_types::job_status_t status) {
    switch (status) {
        case scheduler_types::JOB_STATUS_PENDING: return sync_v2::JOB_STATUS_PENDING;
        case scheduler_types::JOB_STATUS_RUNNING: return sync_v2::JOB_STATUS_RUNNING;
        case scheduler_types::JOB_STATUS_COMPLETED: return sync_v2::JOB_STATUS_COMPLETED;
        case scheduler_types::JOB_STATUS_FAILED: return sync_v2::JOB_STATUS_FAILED;
        case scheduler_types::JOB_STATUS_CANCELLED: return sync_v2::JOB_STATUS_CANCELLED;
        default: return sync_v2::JOB_STATUS_PENDING;
    }
}

// Map sync v2 authority to scheduler authority
static scheduler_types::job_authority_t SyncV2ToAuthority(sync_v2::JobAuthority auth) {
    return (auth == sync_v2::AUTHORITY_CLOUD)
        ? scheduler_types::AUTHORITY_CLOUD
        : scheduler_types::AUTHORITY_VEHICLE;
}

// Map scheduler authority to sync v2 authority
static sync_v2::JobAuthority AuthorityToSyncV2(scheduler_types::job_authority_t auth) {
    return (auth == scheduler_types::AUTHORITY_CLOUD)
        ? sync_v2::AUTHORITY_CLOUD
        : sync_v2::AUTHORITY_VEHICLE;
}

// Map sync v2 wake policy to scheduler
static scheduler_types::wake_policy_t SyncV2ToWakePolicy(sync_v2::WakePolicy policy) {
    return (policy == sync_v2::WAKE_REQUIRED)
        ? scheduler_types::WAKE_REQUIRED
        : scheduler_types::WAKE_NO_WAKE;
}

// Map scheduler wake policy to sync v2
static sync_v2::WakePolicy WakePolicyToSyncV2(scheduler_types::wake_policy_t policy) {
    return (policy == scheduler_types::WAKE_REQUIRED)
        ? sync_v2::WAKE_REQUIRED
        : sync_v2::WAKE_NO_WAKE;
}

// Map sync v2 sleep policy to scheduler
static scheduler_types::sleep_policy_t SyncV2ToSleepPolicy(sync_v2::SleepPolicy policy) {
    return (policy == sync_v2::SLEEP_INHIBIT)
        ? scheduler_types::SLEEP_INHIBIT
        : scheduler_types::SLEEP_NORMAL;
}

// Map scheduler sleep policy to sync v2
static sync_v2::SleepPolicy SleepPolicyToSyncV2(scheduler_types::sleep_policy_t policy) {
    return (policy == scheduler_types::SLEEP_INHIBIT)
        ? sync_v2::SLEEP_INHIBIT
        : sync_v2::SLEEP_NORMAL;
}

// =============================================================================
// Constructor / Destructor
// =============================================================================

CloudSchedulerSyncBridge::CloudSchedulerSyncBridge(const CloudSchedulerSyncBridgeConfig& config)
    : config_(config) {
    if (config_.bridge_instance_id.empty()) {
        config_.bridge_instance_id = GenerateInstanceId();
    }
}

CloudSchedulerSyncBridge::~CloudSchedulerSyncBridge() {
    Stop();
}

// =============================================================================
// Lifecycle
// =============================================================================

bool CloudSchedulerSyncBridge::Start() {
    if (running_) {
        return true;
    }

    LOG(INFO) << "Starting CloudSchedulerSyncBridge"
              << ", scheduler=" << config_.scheduler_address
              << ", transport=" << config_.transport_address
              << ", content_id=" << config_.content_id
              << ", instance_id=" << config_.bridge_instance_id;

    start_time_ = std::chrono::steady_clock::now();

    if (!ConnectToServices()) {
        LOG(ERROR) << "Failed to connect to services";
        return false;
    }

    running_ = true;
    StartMessageSubscription();
    StartPendingSyncsPoll();

    LOG(INFO) << "CloudSchedulerSyncBridge started";
    return true;
}

void CloudSchedulerSyncBridge::Stop() {
    if (!running_) {
        return;
    }

    LOG(INFO) << "Stopping CloudSchedulerSyncBridge";
    running_ = false;
    subscription_running_ = false;

    // Cancel the streaming context to unblock the subscription thread
    {
        std::lock_guard<std::mutex> lock(subscription_context_mutex_);
        if (subscription_context_) {
            subscription_context_->TryCancel();
        }
    }

    if (subscription_thread_.joinable()) {
        subscription_thread_.join();
    }

    // Wake up and stop poll thread
    {
        std::lock_guard<std::mutex> lock(poll_mutex_);
        poll_cv_.notify_all();
    }
    if (poll_thread_.joinable()) {
        poll_thread_.join();
    }

    // Clear stubs
    get_jobs_stub_.reset();
    upsert_job_stub_.reset();
    record_execution_stub_.reset();
    get_sync_state_stub_.reset();
    update_sync_state_stub_.reset();
    get_pending_syncs_stub_.reset();
    send_stub_.reset();
    subscribe_stub_.reset();

    scheduler_connected_ = false;
    transport_connected_ = false;

    LOG(INFO) << "CloudSchedulerSyncBridge stopped";
}

bool CloudSchedulerSyncBridge::ConnectToServices() {
    // Create channels
    scheduler_channel_ = grpc::CreateChannel(
        config_.scheduler_address,
        grpc::InsecureChannelCredentials());

    transport_channel_ = grpc::CreateChannel(
        config_.transport_address,
        grpc::InsecureChannelCredentials());

    // Create scheduler stubs (internal API)
    get_jobs_stub_ = sched_pb::get_jobs_for_vehicle_service::NewStub(scheduler_channel_);
    upsert_job_stub_ = sched_pb::upsert_job_service::NewStub(scheduler_channel_);
    record_execution_stub_ = sched_pb::record_execution_service::NewStub(scheduler_channel_);
    get_sync_state_stub_ = sched_pb::get_vehicle_sync_state_service::NewStub(scheduler_channel_);
    update_sync_state_stub_ = sched_pb::update_vehicle_sync_state_service::NewStub(scheduler_channel_);
    get_pending_syncs_stub_ = sched_pb::get_pending_syncs_service::NewStub(scheduler_channel_);

    // Create transport stubs
    send_stub_ = transport_pb::send_to_vehicle_service::NewStub(transport_channel_);
    subscribe_stub_ = transport_pb::on_vehicle_message_service::NewStub(transport_channel_);

    scheduler_connected_ = true;
    transport_connected_ = true;

    return true;
}

void CloudSchedulerSyncBridge::StartMessageSubscription() {
    subscription_running_ = true;

    subscription_thread_ = std::thread([this]() {
        LOG(INFO) << "Starting V2C message subscription for content_id=" << config_.content_id;

        while (subscription_running_ && running_) {
            // Create context that can be cancelled
            auto context = std::make_unique<grpc::ClientContext>();

            {
                std::lock_guard<std::mutex> lock(subscription_context_mutex_);
                subscription_context_ = std::move(context);
            }

            transport_pb::on_vehicle_message_subscribe_request request;
            request.set_content_id(config_.content_id);  // Scheduler sync content_id (e.g., 202)

            auto reader = subscribe_stub_->subscribe(subscription_context_.get(), request);

            transport_pb::on_vehicle_message response;
            while (reader->Read(&response) && subscription_running_) {
                const auto& msg = response.message();
                std::vector<uint8_t> payload(msg.payload().begin(), msg.payload().end());
                HandleV2CSyncMessage(msg.vehicle_id(), payload);
            }

            auto status = reader->Finish();

            // Clear the context
            {
                std::lock_guard<std::mutex> lock(subscription_context_mutex_);
                subscription_context_.reset();
            }

            if (!status.ok() && subscription_running_) {
                LOG(WARNING) << "Subscription stream ended: " << status.error_message();
                transport_connected_ = false;

                // Reconnect delay
                std::this_thread::sleep_for(std::chrono::seconds(1));
                transport_connected_ = true;
            }
        }

        LOG(INFO) << "V2C message subscription stopped";
    });
}

void CloudSchedulerSyncBridge::StartPendingSyncsPoll() {
    poll_thread_ = std::thread(&CloudSchedulerSyncBridge::PollLoop, this);
}

void CloudSchedulerSyncBridge::PollLoop() {
    LOG(INFO) << "Starting pending syncs poll thread, interval=" << config_.poll_interval_ms << "ms";

    while (running_) {
        {
            std::unique_lock<std::mutex> lock(poll_mutex_);
            poll_cv_.wait_for(lock, std::chrono::milliseconds(config_.poll_interval_ms),
                              [this]() { return !running_; });
        }

        if (!running_) break;

        // Query scheduler for vehicles with pending syncs
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));

        sched_pb::get_pending_syncs_request request;
        request.set_limit(100);  // Process up to 100 vehicles per poll cycle

        sched_pb::get_pending_syncs_response response;
        auto status = get_pending_syncs_stub_->get_pending_syncs(&context, request, &response);

        if (!status.ok()) {
            if (running_) {
                LOG(WARNING) << "Failed to get pending syncs: " << status.error_message();
            }
            continue;
        }

        // Send C2V to each vehicle with pending changes
        for (const auto& vehicle_state : response.pending_vehicles()) {
            const std::string& vehicle_id = vehicle_state.vehicle_id();

            VLOG(1) << "Vehicle " << vehicle_id << " needs sync:"
                    << " cloud_checksum=" << std::hex << vehicle_state.cloud_checksum()
                    << " last_seen=" << vehicle_state.last_seen_v2c_checksum() << std::dec;

            SendC2VMessage(vehicle_id);
        }

        if (response.pending_vehicles_size() > 0) {
            LOG(INFO) << "Sent C2V sync messages to " << response.pending_vehicles_size() << " vehicles";
        }
    }

    LOG(INFO) << "Pending syncs poll thread stopped";
}

void CloudSchedulerSyncBridge::RegisterServices(grpc::ServerBuilder& builder) {
    builder.RegisterService(static_cast<bridge_pb::get_stats_service::Service*>(this));
    builder.RegisterService(static_cast<bridge_pb::get_health_service::Service*>(this));
    builder.RegisterService(static_cast<bridge_pb::get_vehicle_sync_info_service::Service*>(this));
    builder.RegisterService(static_cast<bridge_pb::force_sync_service::Service*>(this));
    builder.RegisterService(static_cast<bridge_pb::trigger_job_service::Service*>(this));
    builder.RegisterService(static_cast<bridge_pb::healthy_service::Service*>(this));
}

// =============================================================================
// V2C Message Processing
// =============================================================================

void CloudSchedulerSyncBridge::HandleV2CSyncMessage(
    const std::string& vehicle_id,
    const std::vector<uint8_t>& payload) {

    v2c_messages_received_++;

    sync_v2::V2C_SyncMessage v2c_msg;
    if (!v2c_msg.ParseFromArray(payload.data(), payload.size())) {
        LOG(WARNING) << "Failed to parse V2C_SyncMessage from " << vehicle_id;
        errors_++;
        return;
    }

    ProcessV2CMessage(vehicle_id, v2c_msg);
}

void CloudSchedulerSyncBridge::ProcessV2CMessage(
    const std::string& vehicle_id,
    const sync_v2::V2C_SyncMessage& v2c_msg) {

    LOG(INFO) << "Processing V2C message from " << vehicle_id
              << ": jobs=" << v2c_msg.jobs_size()
              << ", executions=" << v2c_msg.executions_size()
              << ", checksum=" << v2c_msg.state_checksum();

    // Track vehicle
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        vehicles_seen_.insert(vehicle_id);
    }

    // Get current sync state
    auto sync_state = GetVehicleSyncState(vehicle_id);

    // Check quiescence: if cloud checksum matches last seen V2C checksum
    // AND V2C checksum matches cloud checksum, no sync needed
    if (IsQuiescent(sync_state, v2c_msg.state_checksum())) {
        LOG(INFO) << "Quiescent for " << vehicle_id
                  << " (cloud=" << sync_state.cloud_checksum()
                  << ", v2c=" << v2c_msg.state_checksum() << ")";
        quiescent_skipped_++;

        // Update last seen checksum
        UpdateVehicleSyncState(vehicle_id, v2c_msg.state_checksum());
        return;
    }

    // Get current cloud jobs
    auto cloud_jobs = GetCloudJobs(vehicle_id);

    // Build map of cloud jobs by job_id for comparison
    std::map<std::string, scheduler_types::job_t> cloud_job_map;
    for (const auto& job : cloud_jobs) {
        cloud_job_map[job.job_id()] = job;
    }

    // Process each job from V2C message
    for (const auto& v2c_record : v2c_msg.jobs()) {
        const auto& job_id = v2c_record.job_id();

        // Convert to scheduler job format
        auto v2c_job = V2CRecordToJobInfo(vehicle_id, v2c_record);

        // Build version vectors
        sched_lib::VersionVector v2c_version{
            v2c_record.version().cloud_seq(),
            v2c_record.version().vehicle_seq()
        };

        // Check if we have this job in cloud
        auto it = cloud_job_map.find(job_id);
        std::optional<sched_lib::VersionVector> local_version;
        if (it != cloud_job_map.end()) {
            local_version = sched_lib::VersionVector{
                it->second.cloud_seq(),
                it->second.vehicle_seq()
            };
        }

        // Determine authority
        sched_lib::JobAuthority authority = (v2c_record.authority() == sync_v2::AUTHORITY_CLOUD)
            ? sched_lib::JobAuthority::CLOUD
            : sched_lib::JobAuthority::VEHICLE;

        // Use sync engine to determine action (we are cloud side)
        auto result = sched_lib::SyncEngine::process_remote(
            v2c_version, local_version, authority, true /* is_cloud_side */);

        switch (result.action) {
            case sched_lib::SyncResult::NO_ACTION:
                // Versions equal - no data change needed, but confirm vehicle has it
                // This updates job_sync_states_ to SYNCED in scheduler
                v2c_job.set_cloud_seq(v2c_version.cloud_seq);
                v2c_job.set_vehicle_seq(v2c_version.vehicle_seq);
                UpsertJob(v2c_job);
                break;

            case sched_lib::SyncResult::ACCEPT_REMOTE:
                // Accept vehicle's version
                v2c_job.set_cloud_seq(v2c_version.cloud_seq);
                v2c_job.set_vehicle_seq(v2c_version.vehicle_seq);
                if (UpsertJob(v2c_job)) {
                    jobs_upserted_++;
                }
                break;

            case sched_lib::SyncResult::REJECT_REMOTE:
                // Cloud is ahead - C2V will send our version
                break;

            case sched_lib::SyncResult::CONFLICT_RESOLVED:
                conflicts_resolved_++;
                if (result.winner == "vehicle") {
                    // Vehicle wins - use merged version
                    v2c_job.set_cloud_seq(result.resolved_version.cloud_seq);
                    v2c_job.set_vehicle_seq(result.resolved_version.vehicle_seq);
                    if (UpsertJob(v2c_job)) {
                        jobs_upserted_++;
                    }
                }
                // If cloud wins, C2V will send our version
                break;
        }
    }

    // Process executions (append-only, no conflicts)
    for (const auto& execution : v2c_msg.executions()) {
        // Find the job_id for this execution
        if (RecordExecution(vehicle_id, execution.job_id(), execution)) {
            executions_recorded_++;
        }
    }

    // Update vehicle sync state
    UpdateVehicleSyncState(vehicle_id, v2c_msg.state_checksum());

    // Send C2V message with current cloud state
    SendC2VMessage(vehicle_id);

    v2c_messages_processed_++;

    // Update vehicle info
    {
        std::lock_guard<std::mutex> lock(vehicle_info_mutex_);
        auto& info = vehicle_sync_info_[vehicle_id];
        info.set_vehicle_id(vehicle_id);
        info.set_last_v2c_timestamp_ms(NowMs());
        info.set_last_seen_v2c_checksum(v2c_msg.state_checksum());
    }
}

// =============================================================================
// Scheduler API Calls
// =============================================================================

std::vector<scheduler_types::job_t> CloudSchedulerSyncBridge::GetCloudJobs(
    const std::string& vehicle_id) {

    grpc::ClientContext context;
    sched_pb::get_jobs_for_vehicle_request request;
    sched_pb::get_jobs_for_vehicle_response response;

    request.set_vehicle_id(vehicle_id);
    request.set_include_deleted(true);  // Need tombstones for sync

    auto status = get_jobs_stub_->get_jobs_for_vehicle(&context, request, &response);
    if (!status.ok()) {
        LOG(ERROR) << "Failed to get jobs for " << vehicle_id << ": " << status.error_message();
        scheduler_connected_ = false;
        return {};
    }

    scheduler_connected_ = true;
    return {response.jobs().begin(), response.jobs().end()};
}

bool CloudSchedulerSyncBridge::UpsertJob(const scheduler_types::job_t& job) {
    grpc::ClientContext context;
    sched_pb::upsert_job_request request;
    sched_pb::upsert_job_response response;

    *request.mutable_job() = job;

    auto status = upsert_job_stub_->upsert_job(&context, request, &response);
    if (!status.ok()) {
        LOG(ERROR) << "Failed to upsert job " << job.job_id() << ": " << status.error_message();
        errors_++;
        return false;
    }

    return response.success();
}

bool CloudSchedulerSyncBridge::RecordExecution(
    const std::string& vehicle_id,
    const std::string& job_id,
    const sync_v2::ExecutionRecord& execution) {

    grpc::ClientContext context;
    sched_pb::record_execution_request request;
    sched_pb::record_execution_response response;

    request.set_vehicle_id(vehicle_id);
    request.set_job_id(job_id);

    auto* exec = request.mutable_execution();
    exec->set_execution_id(execution.execution_id());
    exec->set_executed_at_ms(execution.executed_at_ms());
    exec->set_duration_ms(execution.duration_ms());
    exec->set_status(SyncV2ToJobStatus(execution.status()));
    exec->set_result_json(execution.result_json());
    exec->set_error_message(execution.error_message());

    auto status = record_execution_stub_->record_execution(&context, request, &response);
    if (!status.ok()) {
        LOG(ERROR) << "Failed to record execution: " << status.error_message();
        errors_++;
        return false;
    }

    return response.success() && !response.is_duplicate();
}

sched_pb::vehicle_sync_state_t CloudSchedulerSyncBridge::GetVehicleSyncState(
    const std::string& vehicle_id) {

    grpc::ClientContext context;
    sched_pb::get_vehicle_sync_state_request request;
    sched_pb::get_vehicle_sync_state_response response;

    request.set_vehicle_id(vehicle_id);

    auto status = get_sync_state_stub_->get_vehicle_sync_state(&context, request, &response);
    if (!status.ok()) {
        LOG(ERROR) << "Failed to get sync state for " << vehicle_id << ": " << status.error_message();
        return {};
    }

    return response.state();
}

void CloudSchedulerSyncBridge::UpdateVehicleSyncState(
    const std::string& vehicle_id,
    uint64_t v2c_checksum) {

    grpc::ClientContext context;
    sched_pb::update_vehicle_sync_state_request request;
    sched_pb::update_vehicle_sync_state_response response;

    request.set_vehicle_id(vehicle_id);
    request.set_last_seen_v2c_checksum(v2c_checksum);

    auto status = update_sync_state_stub_->update_vehicle_sync_state(&context, request, &response);
    if (!status.ok()) {
        LOG(ERROR) << "Failed to update sync state for " << vehicle_id << ": " << status.error_message();
    }
}

// =============================================================================
// C2V Message Building
// =============================================================================

void CloudSchedulerSyncBridge::SendC2VMessage(const std::string& vehicle_id) {
    // Get current cloud jobs
    auto cloud_jobs = GetCloudJobs(vehicle_id);
    auto sync_state = GetVehicleSyncState(vehicle_id);

    // Build C2V message
    sync_v2::C2V_SyncMessage c2v_msg;
    c2v_msg.set_vehicle_id(vehicle_id);
    c2v_msg.set_sync_timestamp_ms(NowMs());
    c2v_msg.set_state_checksum(sync_state.cloud_checksum());
    c2v_msg.set_last_seen_v2c_checksum(sync_state.last_seen_v2c_checksum());

    // Add all jobs (including tombstones)
    for (const auto& job : cloud_jobs) {
        *c2v_msg.add_jobs() = JobInfoToC2VRecord(job);
    }

    // Serialize and send
    std::string serialized;
    if (!c2v_msg.SerializeToString(&serialized)) {
        LOG(ERROR) << "Failed to serialize C2V message";
        errors_++;
        return;
    }

    // Send via transport
    grpc::ClientContext context;
    transport_pb::send_to_vehicle_request request;
    transport_pb::send_to_vehicle_response response;

    auto* send_req = request.mutable_request();
    send_req->set_vehicle_id(vehicle_id);
    send_req->set_content_id(config_.content_id);
    send_req->set_payload(serialized);
    send_req->set_persistence(transport_pb::persistence_t::VOLATILE);

    auto status = send_stub_->send_to_vehicle(&context, request, &response);
    if (!status.ok()) {
        LOG(ERROR) << "Failed to send C2V to " << vehicle_id << ": " << status.error_message();
        errors_++;
        transport_connected_ = false;
        return;
    }

    if (response.result().status() != transport_pb::publish_status_t::OK) {
        LOG(ERROR) << "Send failed for " << vehicle_id
                   << ": " << static_cast<int>(response.result().status());
        errors_++;
        return;
    }

    c2v_messages_sent_++;
    transport_connected_ = true;

    // Update vehicle info
    {
        std::lock_guard<std::mutex> lock(vehicle_info_mutex_);
        auto& info = vehicle_sync_info_[vehicle_id];
        info.set_last_c2v_timestamp_ms(NowMs());
        info.set_cloud_checksum(sync_state.cloud_checksum());
        info.set_job_count(cloud_jobs.size());
    }

    LOG(INFO) << "Sent C2V to " << vehicle_id << " with " << cloud_jobs.size() << " jobs";
}

// =============================================================================
// Type Conversions
// =============================================================================

scheduler_types::job_t CloudSchedulerSyncBridge::V2CRecordToJobInfo(
    const std::string& vehicle_id,
    const sync_v2::JobRecord& record) {

    scheduler_types::job_t job;
    job.set_vehicle_id(vehicle_id);
    job.set_job_id(record.job_id());
    job.set_authority(SyncV2ToAuthority(record.authority()));
    job.set_cloud_seq(record.version().cloud_seq());
    job.set_vehicle_seq(record.version().vehicle_seq());
    job.set_deleted(record.deleted());
    job.set_title(record.title());
    job.set_service(record.service());
    job.set_method(record.method());
    job.set_parameters_json(record.parameters_json());
    job.set_scheduled_time_ms(record.scheduled_time_ms());
    job.set_recurrence_rule(record.recurrence_rule());
    job.set_end_time_ms(record.end_time_ms());
    job.set_paused(record.paused());
    job.set_wake_policy(SyncV2ToWakePolicy(record.wake_policy()));
    job.set_sleep_policy(SyncV2ToSleepPolicy(record.sleep_policy()));
    job.set_wake_lead_time_s(record.wake_lead_time_s());
    job.set_status(SyncV2ToJobStatus(record.status()));
    job.set_next_run_time_ms(record.next_run_time_ms());
    job.set_last_executed_ms(record.last_executed_ms());
    job.set_created_at_ms(record.created_at_ms());
    job.set_updated_at_ms(record.updated_at_ms());
    job.set_created_by(record.created_by());

    return job;
}

sync_v2::JobRecord CloudSchedulerSyncBridge::JobInfoToC2VRecord(
    const scheduler_types::job_t& job) {

    sync_v2::JobRecord record;
    record.set_job_id(job.job_id());
    record.set_authority(AuthorityToSyncV2(job.authority()));
    record.mutable_version()->set_cloud_seq(job.cloud_seq());
    record.mutable_version()->set_vehicle_seq(job.vehicle_seq());
    record.set_deleted(job.deleted());
    record.set_title(job.title());
    record.set_service(job.service());
    record.set_method(job.method());
    record.set_parameters_json(job.parameters_json());
    record.set_scheduled_time_ms(job.scheduled_time_ms());
    record.set_recurrence_rule(job.recurrence_rule());
    record.set_end_time_ms(job.end_time_ms());
    record.set_paused(job.paused());
    record.set_wake_policy(WakePolicyToSyncV2(job.wake_policy()));
    record.set_sleep_policy(SleepPolicyToSyncV2(job.sleep_policy()));
    record.set_wake_lead_time_s(job.wake_lead_time_s());
    record.set_status(JobStatusToSyncV2(job.status()));
    record.set_next_run_time_ms(job.next_run_time_ms());
    record.set_last_executed_ms(job.last_executed_ms());
    record.set_created_at_ms(job.created_at_ms());
    record.set_updated_at_ms(job.updated_at_ms());
    record.set_created_by(job.created_by());

    return record;
}

uint64_t CloudSchedulerSyncBridge::ComputeStateChecksum(
    const std::vector<scheduler_types::job_t>& jobs) {

    // Use the scheduler library for consistent checksum computation
    // Must include ALL fields that are part of the hash (see job_hash.cpp)
    std::vector<sched_lib::Job> lib_jobs;
    for (const auto& job : jobs) {
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
        lib_jobs.push_back(lib_job);
    }

    return sched_lib::compute_state_checksum(lib_jobs);
}

bool CloudSchedulerSyncBridge::IsQuiescent(
    const sched_pb::vehicle_sync_state_t& state,
    uint64_t v2c_checksum) {

    // Quiescent if:
    // 1. Cloud checksum matches V2C checksum (both have same state)
    // 2. Last seen V2C checksum matches current V2C (no new changes from vehicle)
    return state.cloud_checksum() == v2c_checksum &&
           state.last_seen_v2c_checksum() == v2c_checksum;
}

// =============================================================================
// gRPC Service Methods (Bridge API)
// =============================================================================

grpc::Status CloudSchedulerSyncBridge::get_stats(
    grpc::ServerContext* context,
    const bridge_pb::get_stats_request* request,
    bridge_pb::get_stats_response* response) {

    auto* stats = response->mutable_stats();
    stats->set_v2c_messages_received(v2c_messages_received_);
    stats->set_v2c_messages_processed(v2c_messages_processed_);
    stats->set_c2v_messages_sent(c2v_messages_sent_);
    stats->set_jobs_upserted(jobs_upserted_);
    stats->set_executions_recorded(executions_recorded_);
    stats->set_quiescent_skipped(quiescent_skipped_);
    stats->set_conflicts_resolved(conflicts_resolved_);
    stats->set_errors(errors_);

    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats->set_vehicles_seen(vehicles_seen_.size());
    }

    auto uptime = std::chrono::steady_clock::now() - start_time_;
    stats->set_uptime_ms(std::chrono::duration_cast<std::chrono::milliseconds>(uptime).count());

    return grpc::Status::OK;
}

grpc::Status CloudSchedulerSyncBridge::get_health(
    grpc::ServerContext* context,
    const bridge_pb::get_health_request* request,
    bridge_pb::get_health_response* response) {

    auto* health = response->mutable_health();

    if (running_) {
        health->set_status(bridge_pb::STATUS_RUNNING);
    } else {
        health->set_status(bridge_pb::STATUS_STOPPED);
    }

    health->set_scheduler_connected(scheduler_connected_);
    health->set_transport_connected(transport_connected_);

    if (!last_error_.empty()) {
        health->set_last_error(last_error_);
        health->set_last_error_timestamp_ms(last_error_timestamp_ms_);
    }

    return grpc::Status::OK;
}

grpc::Status CloudSchedulerSyncBridge::get_vehicle_sync_info(
    grpc::ServerContext* context,
    const bridge_pb::get_vehicle_sync_info_request* request,
    bridge_pb::get_vehicle_sync_info_response* response) {

    const auto& vehicle_id = request->vehicle_id();

    std::lock_guard<std::mutex> lock(vehicle_info_mutex_);
    auto it = vehicle_sync_info_.find(vehicle_id);

    if (it == vehicle_sync_info_.end()) {
        response->set_found(false);
        return grpc::Status::OK;
    }

    *response->mutable_info() = it->second;

    // Check if quiescent
    auto sync_state = GetVehicleSyncState(vehicle_id);
    response->mutable_info()->set_is_quiescent(
        sync_state.cloud_checksum() == sync_state.last_seen_v2c_checksum());

    response->set_found(true);
    return grpc::Status::OK;
}

grpc::Status CloudSchedulerSyncBridge::force_sync(
    grpc::ServerContext* context,
    const bridge_pb::force_sync_request* request,
    bridge_pb::force_sync_response* response) {

    const auto& vehicle_id = request->vehicle_id();

    if (!running_) {
        response->set_success(false);
        response->set_error_message("Bridge not running");
        return grpc::Status::OK;
    }

    try {
        SendC2VMessage(vehicle_id);
        response->set_success(true);
    } catch (const std::exception& e) {
        response->set_success(false);
        response->set_error_message(e.what());
    }

    return grpc::Status::OK;
}

grpc::Status CloudSchedulerSyncBridge::trigger_job(
    grpc::ServerContext* context,
    const bridge_pb::trigger_job_request* request,
    bridge_pb::trigger_job_response* response) {

    const auto& vehicle_id = request->vehicle_id();
    const auto& job_id = request->job_id();

    if (!running_) {
        response->set_sent(false);
        response->set_error_message("Bridge not running");
        return grpc::Status::OK;
    }

    if (vehicle_id.empty() || job_id.empty()) {
        response->set_sent(false);
        response->set_error_message("vehicle_id and job_id are required");
        return grpc::Status::OK;
    }

    // Build TriggerJobRequest
    sync_v2::TriggerJobRequest trigger_req;
    trigger_req.set_job_id(job_id);
    trigger_req.set_requester_id("cloud-dashboard");
    trigger_req.set_timestamp_ms(NowMs());
    trigger_req.set_expires_at_ms(NowMs() + 30000);  // 30 second expiry

    // Serialize
    std::string payload;
    if (!trigger_req.SerializeToString(&payload)) {
        response->set_sent(false);
        response->set_error_message("Failed to serialize trigger request");
        return grpc::Status::OK;
    }

    // Send to vehicle via transport
    grpc::ClientContext ctx;
    transport_pb::send_to_vehicle_request send_req;
    transport_pb::send_to_vehicle_response send_resp;

    auto* msg = send_req.mutable_request();
    msg->set_vehicle_id(vehicle_id);
    msg->set_content_id(config_.content_id);
    msg->set_payload(payload);
    msg->set_persistence(transport_pb::BEST_EFFORT);

    auto status = send_stub_->send_to_vehicle(&ctx, send_req, &send_resp);
    if (!status.ok()) {
        response->set_sent(false);
        response->set_error_message("Transport error: " + status.error_message());
        return grpc::Status::OK;
    }

    if (send_resp.result().status() != transport_pb::OK) {
        response->set_sent(false);
        response->set_error_message("Send failed");
        return grpc::Status::OK;
    }

    LOG(INFO) << "Sent TriggerJobRequest to " << vehicle_id << " for job " << job_id;
    response->set_sent(true);
    return grpc::Status::OK;
}

grpc::Status CloudSchedulerSyncBridge::healthy(
    grpc::ServerContext* context,
    const bridge_pb::healthy_request* request,
    bridge_pb::healthy_response* response) {

    response->set_is_healthy(running_ && scheduler_connected_ && transport_connected_);
    return grpc::Status::OK;
}

}  // namespace ifex::cloud
