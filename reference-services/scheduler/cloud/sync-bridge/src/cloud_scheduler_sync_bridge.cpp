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

// Map sync v3 status to common job status
static scheduler_types::job_status_t SyncV3ToJobStatus(sync_v3::JobStatus status) {
    switch (status) {
        case sync_v3::JOB_STATUS_PENDING: return scheduler_types::JOB_STATUS_PENDING;
        case sync_v3::JOB_STATUS_RUNNING: return scheduler_types::JOB_STATUS_RUNNING;
        case sync_v3::JOB_STATUS_COMPLETED: return scheduler_types::JOB_STATUS_COMPLETED;
        case sync_v3::JOB_STATUS_FAILED: return scheduler_types::JOB_STATUS_FAILED;
        case sync_v3::JOB_STATUS_CANCELLED: return scheduler_types::JOB_STATUS_CANCELLED;
        default: return scheduler_types::JOB_STATUS_PENDING;
    }
}

// Map common job status to sync v3 status
static sync_v3::JobStatus JobStatusToSyncV3(scheduler_types::job_status_t status) {
    switch (status) {
        case scheduler_types::JOB_STATUS_PENDING: return sync_v3::JOB_STATUS_PENDING;
        case scheduler_types::JOB_STATUS_RUNNING: return sync_v3::JOB_STATUS_RUNNING;
        case scheduler_types::JOB_STATUS_COMPLETED: return sync_v3::JOB_STATUS_COMPLETED;
        case scheduler_types::JOB_STATUS_FAILED: return sync_v3::JOB_STATUS_FAILED;
        case scheduler_types::JOB_STATUS_CANCELLED: return sync_v3::JOB_STATUS_CANCELLED;
        default: return sync_v3::JOB_STATUS_PENDING;
    }
}

// Map sync v3 authority to scheduler authority
static scheduler_types::job_authority_t SyncV3ToAuthority(sync_v3::JobAuthority auth) {
    return (auth == sync_v3::AUTHORITY_CLOUD)
        ? scheduler_types::AUTHORITY_CLOUD
        : scheduler_types::AUTHORITY_VEHICLE;
}

// Map scheduler authority to sync v3 authority
static sync_v3::JobAuthority AuthorityToSyncV3(scheduler_types::job_authority_t auth) {
    return (auth == scheduler_types::AUTHORITY_CLOUD)
        ? sync_v3::AUTHORITY_CLOUD
        : sync_v3::AUTHORITY_VEHICLE;
}

// Map sync v3 wake policy to scheduler
static scheduler_types::wake_policy_t SyncV3ToWakePolicy(sync_v3::WakePolicy policy) {
    return (policy == sync_v3::WAKE_REQUIRED)
        ? scheduler_types::WAKE_REQUIRED
        : scheduler_types::WAKE_NO_WAKE;
}

// Map scheduler wake policy to sync v3
static sync_v3::WakePolicy WakePolicyToSyncV3(scheduler_types::wake_policy_t policy) {
    return (policy == scheduler_types::WAKE_REQUIRED)
        ? sync_v3::WAKE_REQUIRED
        : sync_v3::WAKE_NO_WAKE;
}

// Map sync v3 sleep policy to scheduler
static scheduler_types::sleep_policy_t SyncV3ToSleepPolicy(sync_v3::SleepPolicy policy) {
    return (policy == sync_v3::SLEEP_INHIBIT)
        ? scheduler_types::SLEEP_INHIBIT
        : scheduler_types::SLEEP_NORMAL;
}

// Map scheduler sleep policy to sync v3
static sync_v3::SleepPolicy SleepPolicyToSyncV3(scheduler_types::sleep_policy_t policy) {
    return (policy == scheduler_types::SLEEP_INHIBIT)
        ? sync_v3::SLEEP_INHIBIT
        : sync_v3::SLEEP_NORMAL;
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

    LOG(INFO) << "Starting CloudSchedulerSyncBridge (v3.2 protocol)"
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
    set_remote_version_stub_.reset();
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
    set_remote_version_stub_ = sched_pb::set_job_remote_version_service::NewStub(scheduler_channel_);

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
            request.set_content_id(config_.content_id);

            auto reader = subscribe_stub_->subscribe(subscription_context_.get(), request);

            transport_pb::on_vehicle_message response;
            while (reader->Read(&response) && subscription_running_) {
                const auto& msg = response.message();
                std::vector<uint8_t> payload(msg.payload().begin(), msg.payload().end());
                HandleV2CEnvelope(msg.vehicle_id(), payload);
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
        request.set_limit(100);

        sched_pb::get_pending_syncs_response response;
        auto status = get_pending_syncs_stub_->get_pending_syncs(&context, request, &response);

        if (!status.ok()) {
            if (running_) {
                LOG(WARNING) << "Failed to get pending syncs: " << status.error_message();
            }
            continue;
        }

        // Send SyncMessage to each vehicle with pending changes (v3.2 dirty-first)
        for (const auto& vehicle_state : response.pending_vehicles()) {
            const std::string& vehicle_id = vehicle_state.vehicle_id();

            VLOG(1) << "Vehicle " << vehicle_id << " needs sync:"
                    << " cloud_checksum=" << std::hex << vehicle_state.cloud_checksum()
                    << " last_seen=" << vehicle_state.last_seen_v2c_checksum() << std::dec;

            // Get dirty jobs and send as SyncMessage (v3.2 dirty-first)
            auto dirty_jobs = GetDirtyJobs(vehicle_id);
            std::vector<sync_v3::JobRecord> jobs_to_send;
            for (const auto& job : dirty_jobs) {
                jobs_to_send.push_back(JobInfoToC2VRecord(job));
            }

            SendSyncMessage(vehicle_id, jobs_to_send, {});
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
// V2C Message Handling (v3.2 Protocol)
// =============================================================================

void CloudSchedulerSyncBridge::HandleV2CEnvelope(
    const std::string& vehicle_id,
    const std::vector<uint8_t>& payload) {

    v2c_messages_received_++;

    sync_v3::V2C_Envelope envelope;
    if (!envelope.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
        LOG(WARNING) << "Failed to parse V2C_Envelope from " << vehicle_id;
        errors_++;
        return;
    }

    // Track vehicle
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        vehicles_seen_.insert(vehicle_id);
    }

    switch (envelope.message_case()) {
        // v3.2 messages (preferred)
        case sync_v3::V2C_Envelope::kSync:
            HandleSyncMessage(vehicle_id, envelope.sync());
            break;
        case sync_v3::V2C_Envelope::kGapDetect:
            HandleGapDetect(vehicle_id, envelope.gap_detect());
            break;

        // Other messages
        case sync_v3::V2C_Envelope::kExecutions:
            HandleExecutions(vehicle_id, envelope.executions());
            break;
        case sync_v3::V2C_Envelope::kTriggerResponse:
            HandleTriggerResponse(vehicle_id, envelope.trigger_response());
            break;
        case sync_v3::V2C_Envelope::MESSAGE_NOT_SET:
            LOG(WARNING) << "Received empty V2C_Envelope from " << vehicle_id;
            break;
    }

    v2c_messages_processed_++;
}

void CloudSchedulerSyncBridge::HandleExecutions(
    const std::string& vehicle_id,
    const sync_v3::V2C_Executions& executions) {

    LOG(INFO) << "Received V2C_Executions from " << vehicle_id
              << ": " << executions.executions_size() << " executions";

    std::vector<std::string> acked_ids;

    for (const auto& exec : executions.executions()) {
        if (RecordExecution(vehicle_id, exec.job_id(), exec)) {
            executions_recorded_++;
        }
        acked_ids.push_back(exec.execution_id());
    }

    // Send acknowledgment
    SendExecutionAck(vehicle_id, acked_ids);
}

void CloudSchedulerSyncBridge::HandleTriggerResponse(
    const std::string& vehicle_id,
    const sync_v3::V2C_TriggerResponse& response) {

    LOG(INFO) << "Received V2C_TriggerResponse from " << vehicle_id
              << ": job=" << response.job_id()
              << ", accepted=" << (response.accepted() ? "true" : "false");

    // Could store trigger result for API query, but for now just log
    if (!response.accepted()) {
        LOG(WARNING) << "Trigger rejected for job " << response.job_id()
                     << ": " << response.error_message();
    }
}

void CloudSchedulerSyncBridge::HandleSyncMessage(
    const std::string& vehicle_id,
    const sync_v3::SyncMessage& msg) {

    LOG(INFO) << "Received V2C SyncMessage from " << vehicle_id
              << ": jobs=" << msg.jobs_size()
              << ", acked_jobs=" << msg.acked_jobs_size()
              << ", checksum=" << std::hex << msg.state_checksum() << std::dec;

    // Update last seen checksum
    UpdateVehicleSyncState(vehicle_id, msg.state_checksum());

    // Update vehicle info
    {
        std::lock_guard<std::mutex> lock(vehicle_info_mutex_);
        auto& info = vehicle_sync_info_[vehicle_id];
        info.set_vehicle_id(vehicle_id);
        info.set_last_v2c_timestamp_ms(NowMs());
        info.set_last_seen_v2c_checksum(msg.state_checksum());
    }

    // 1. Process ACKs from vehicle - update remote_version for acked jobs
    for (const auto& ack : msg.acked_jobs()) {
        LOG(INFO) << "Processing ACK from vehicle: job=" << ack.job_id()
                  << " acked_version={" << ack.cloud_seq() << "," << ack.vehicle_seq() << "}";
        SetJobRemoteVersion(vehicle_id, ack.job_id(), ack.cloud_seq(), ack.vehicle_seq());
    }

    // 2. Apply received jobs, collect ACKs to send back
    std::vector<sync_v3::JobVersionAck> acks_to_send;
    for (const auto& job_record : msg.jobs()) {
        auto job = V2CRecordToJobInfo(vehicle_id, job_record);
        if (UpsertJob(job)) {
            jobs_upserted_++;
        }
        // Vehicle sent this job - that confirms vehicle has this version
        SetJobRemoteVersion(vehicle_id, job_record.job_id(),
                            job_record.version().cloud_seq(),
                            job_record.version().vehicle_seq());
        // ACK the job we just received
        sync_v3::JobVersionAck ack;
        ack.set_job_id(job_record.job_id());
        ack.set_cloud_seq(job_record.version().cloud_seq());
        ack.set_vehicle_seq(job_record.version().vehicle_seq());
        acks_to_send.push_back(ack);
    }

    // 3. Check quiescence
    auto dirty_jobs = GetDirtyJobs(vehicle_id);
    auto sync_state = GetVehicleSyncState(vehicle_id);
    uint64_t cloud_checksum = sync_state.cloud_checksum();

    if (cloud_checksum == msg.state_checksum() && dirty_jobs.empty()) {
        // QUIESCENT - checksums match, send ACKs only if any
        LOG(INFO) << "Quiescent (v3.2) for " << vehicle_id << " (checksum="
                  << std::hex << msg.state_checksum() << std::dec << ")";
        quiescent_skipped_++;

        if (!acks_to_send.empty()) {
            SendSyncMessage(vehicle_id, {}, acks_to_send);
        }
        return;
    }

    // 4. Not quiescent - collect dirty jobs to send
    std::vector<sync_v3::JobRecord> jobs_to_send;
    for (const auto& job : dirty_jobs) {
        jobs_to_send.push_back(JobInfoToC2VRecord(job));
    }

    // 5. Continue sync - send dirty jobs + ACKs
    if (!jobs_to_send.empty() || !acks_to_send.empty()) {
        SendSyncMessage(vehicle_id, jobs_to_send, acks_to_send);
    } else {
        // No dirty but mismatch - trigger gap detection
        LOG(INFO) << "Checksum mismatch but no dirty jobs for " << vehicle_id
                  << ", triggering gap detection";
        SendGapDetect(vehicle_id, GetAllJobIds(vehicle_id), {});
    }
}

void CloudSchedulerSyncBridge::HandleGapDetect(
    const std::string& vehicle_id,
    const sync_v3::GapDetect& msg) {

    LOG(INFO) << "Received V2C GapDetect from " << vehicle_id
              << ": job_ids=" << msg.job_ids_size()
              << ", request_job_ids=" << msg.request_job_ids_size();

    auto our_ids = GetAllJobIds(vehicle_id);
    std::set<std::string> our_set(our_ids.begin(), our_ids.end());
    std::set<std::string> vehicle_set(msg.job_ids().begin(), msg.job_ids().end());

    // Jobs we need from vehicle
    std::vector<std::string> request_from_vehicle;
    for (const auto& id : vehicle_set) {
        if (our_set.find(id) == our_set.end()) {
            request_from_vehicle.push_back(id);
        }
    }

    // Jobs vehicle needs from us (missing IDs)
    std::vector<sync_v3::JobRecord> jobs_to_send;
    for (const auto& id : our_set) {
        if (vehicle_set.find(id) == vehicle_set.end()) {
            auto jobs = GetCloudJobs(vehicle_id);
            for (const auto& job : jobs) {
                if (job.job_id() == id) {
                    jobs_to_send.push_back(JobInfoToC2VRecord(job));
                    break;
                }
            }
        }
    }

    // Fulfill specific requests from vehicle
    for (const auto& id : msg.request_job_ids()) {
        auto jobs = GetCloudJobs(vehicle_id);
        for (const auto& job : jobs) {
            if (job.job_id() == id) {
                // Check if already in jobs_to_send
                bool already_added = false;
                for (const auto& j : jobs_to_send) {
                    if (j.job_id() == id) {
                        already_added = true;
                        break;
                    }
                }
                if (!already_added) {
                    jobs_to_send.push_back(JobInfoToC2VRecord(job));
                }
                break;
            }
        }
    }

    LOG(INFO) << "Gap detection for " << vehicle_id
              << ": request_from_vehicle=" << request_from_vehicle.size()
              << ", jobs_to_send=" << jobs_to_send.size();

    // If job_ids match but checksums differ, the issue is content mismatch.
    // Fall back to sending ALL our jobs to force sync convergence.
    if (request_from_vehicle.empty() && jobs_to_send.empty()) {
        LOG(INFO) << "Gap detection: job_ids match but checksum differs - "
                  << "sending all " << our_ids.size() << " jobs to force sync";
        auto all_jobs = GetCloudJobs(vehicle_id);
        for (const auto& job : all_jobs) {
            jobs_to_send.push_back(JobInfoToC2VRecord(job));
        }
    }

    // Send responses
    if (!request_from_vehicle.empty()) {
        SendGapDetect(vehicle_id, our_ids, request_from_vehicle);
    }
    if (!jobs_to_send.empty()) {
        SendSyncMessage(vehicle_id, jobs_to_send, {});
    }
}

// =============================================================================
// C2V Message Sending (v3.2 Protocol)
// =============================================================================

void CloudSchedulerSyncBridge::SendC2VEnvelope(
    const std::string& vehicle_id,
    const sync_v3::C2V_Envelope& envelope) {

    std::string serialized;
    if (!envelope.SerializeToString(&serialized)) {
        LOG(ERROR) << "Failed to serialize C2V_Envelope";
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
}

void CloudSchedulerSyncBridge::SendSyncMessage(
    const std::string& vehicle_id,
    const std::vector<sync_v3::JobRecord>& jobs,
    const std::vector<sync_v3::JobVersionAck>& acked_jobs) {

    auto sync_state = GetVehicleSyncState(vehicle_id);

    sync_v3::C2V_Envelope envelope;
    auto* sync_msg = envelope.mutable_sync();
    sync_msg->set_vehicle_id(vehicle_id);

    // Add jobs to send
    for (const auto& job : jobs) {
        *sync_msg->add_jobs() = job;
    }

    // Add acknowledgments for jobs we received
    for (const auto& ack : acked_jobs) {
        *sync_msg->add_acked_jobs() = ack;
    }

    sync_msg->set_state_checksum(sync_state.cloud_checksum());

    SendC2VEnvelope(vehicle_id, envelope);

    // Update vehicle info
    {
        std::lock_guard<std::mutex> lock(vehicle_info_mutex_);
        auto& info = vehicle_sync_info_[vehicle_id];
        info.set_last_c2v_timestamp_ms(NowMs());
        info.set_cloud_checksum(sync_state.cloud_checksum());
        info.set_job_count(jobs.size());
    }

    LOG(INFO) << "Sent C2V SyncMessage to " << vehicle_id
              << ": " << jobs.size() << " jobs"
              << ", " << acked_jobs.size() << " acks"
              << ", checksum=" << std::hex << sync_msg->state_checksum() << std::dec;
}

void CloudSchedulerSyncBridge::SendGapDetect(
    const std::string& vehicle_id,
    const std::vector<std::string>& job_ids,
    const std::vector<std::string>& request_job_ids) {

    sync_v3::C2V_Envelope envelope;
    auto* gap_detect = envelope.mutable_gap_detect();
    gap_detect->set_vehicle_id(vehicle_id);

    // Add all our job IDs
    for (const auto& id : job_ids) {
        gap_detect->add_job_ids(id);
    }

    // Add jobs we need from vehicle
    for (const auto& id : request_job_ids) {
        gap_detect->add_request_job_ids(id);
    }

    SendC2VEnvelope(vehicle_id, envelope);

    LOG(INFO) << "Sent C2V GapDetect to " << vehicle_id
              << ": " << gap_detect->job_ids_size() << " job_ids"
              << ", " << gap_detect->request_job_ids_size() << " requests";
}

void CloudSchedulerSyncBridge::SendExecutionAck(
    const std::string& vehicle_id,
    const std::vector<std::string>& execution_ids) {

    sync_v3::C2V_Envelope envelope;
    auto* ack = envelope.mutable_execution_ack();
    ack->set_vehicle_id(vehicle_id);

    for (const auto& exec_id : execution_ids) {
        ack->add_execution_ids(exec_id);
    }

    SendC2VEnvelope(vehicle_id, envelope);

    LOG(INFO) << "Sent C2V_ExecutionAck to " << vehicle_id
              << ": " << execution_ids.size() << " executions";
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
    request.set_include_deleted(true);

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
    const sync_v3::ExecutionRecord& execution) {

    grpc::ClientContext context;
    sched_pb::record_execution_request request;
    sched_pb::record_execution_response response;

    request.set_vehicle_id(vehicle_id);
    request.set_job_id(job_id);

    auto* exec = request.mutable_execution();
    exec->set_execution_id(execution.execution_id());
    exec->set_executed_at_ms(execution.executed_at_ms());
    exec->set_duration_ms(execution.duration_ms());
    exec->set_status(SyncV3ToJobStatus(execution.status()));
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
// Type Conversions
// =============================================================================

scheduler_types::job_t CloudSchedulerSyncBridge::V2CRecordToJobInfo(
    const std::string& vehicle_id,
    const sync_v3::JobRecord& record) {

    scheduler_types::job_t job;
    job.set_vehicle_id(vehicle_id);
    job.set_job_id(record.job_id());
    job.set_authority(SyncV3ToAuthority(record.authority()));
    job.mutable_local_version()->set_cloud_seq(record.version().cloud_seq());
    job.mutable_local_version()->set_vehicle_seq(record.version().vehicle_seq());
    job.set_deleted(record.deleted());
    job.set_title(record.title());
    job.set_service(record.service());
    job.set_method(record.method());
    job.set_parameters_json(record.parameters_json());
    job.set_scheduled_time_ms(record.scheduled_time_ms());
    job.set_recurrence_rule(record.recurrence_rule());
    job.set_end_time_ms(record.end_time_ms());
    job.set_paused(record.paused());
    job.set_wake_policy(SyncV3ToWakePolicy(record.wake_policy()));
    job.set_sleep_policy(SyncV3ToSleepPolicy(record.sleep_policy()));
    job.set_wake_lead_time_s(record.wake_lead_time_s());
    job.set_status(SyncV3ToJobStatus(record.status()));
    job.set_next_run_time_ms(record.next_run_time_ms());
    job.set_last_executed_ms(record.last_executed_ms());
    job.set_created_at_ms(record.created_at_ms());
    job.set_updated_at_ms(record.updated_at_ms());
    job.set_created_by(record.created_by());

    return job;
}

sync_v3::JobRecord CloudSchedulerSyncBridge::JobInfoToC2VRecord(
    const scheduler_types::job_t& job) {

    sync_v3::JobRecord record;
    record.set_job_id(job.job_id());
    record.set_authority(AuthorityToSyncV3(job.authority()));
    record.mutable_version()->set_cloud_seq(job.local_version().cloud_seq());
    record.mutable_version()->set_vehicle_seq(job.local_version().vehicle_seq());
    record.set_deleted(job.deleted());
    record.set_title(job.title());
    record.set_service(job.service());
    record.set_method(job.method());
    record.set_parameters_json(job.parameters_json());
    record.set_scheduled_time_ms(job.scheduled_time_ms());
    record.set_recurrence_rule(job.recurrence_rule());
    record.set_end_time_ms(job.end_time_ms());
    record.set_paused(job.paused());
    record.set_wake_policy(WakePolicyToSyncV3(job.wake_policy()));
    record.set_sleep_policy(SleepPolicyToSyncV3(job.sleep_policy()));
    record.set_wake_lead_time_s(job.wake_lead_time_s());
    record.set_status(JobStatusToSyncV3(job.status()));
    record.set_next_run_time_ms(job.next_run_time_ms());
    record.set_last_executed_ms(job.last_executed_ms());
    record.set_created_at_ms(job.created_at_ms());
    record.set_updated_at_ms(job.updated_at_ms());
    record.set_created_by(job.created_by());

    return record;
}

uint64_t CloudSchedulerSyncBridge::ComputeStateChecksum(
    const std::vector<scheduler_types::job_t>& jobs) {

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
        lib_job.local_version.cloud_seq = job.local_version().cloud_seq();
        lib_job.local_version.vehicle_seq = job.local_version().vehicle_seq();
        lib_job.deleted = job.deleted();
        lib_jobs.push_back(lib_job);
    }

    return sched_lib::compute_state_checksum(lib_jobs);
}

bool CloudSchedulerSyncBridge::IsQuiescent(
    const sched_pb::vehicle_sync_state_t& state,
    uint64_t v2c_checksum) {

    return state.cloud_checksum() == v2c_checksum &&
           state.last_seen_v2c_checksum() == v2c_checksum;
}

// =============================================================================
// v3.2 Dirty Tracking and Gap Detection
// =============================================================================

std::vector<scheduler_types::job_t> CloudSchedulerSyncBridge::GetDirtyJobs(
    const std::string& vehicle_id) {
    // A job is "dirty" if local_version != remote_version.
    // Get all jobs and filter for those that need syncing.
    auto all_jobs = GetCloudJobs(vehicle_id);
    std::vector<scheduler_types::job_t> dirty;
    dirty.reserve(all_jobs.size());

    for (const auto& job : all_jobs) {
        const auto& local = job.local_version();
        const auto& remote = job.remote_version();

        // Dirty if versions don't match
        bool is_dirty = (local.cloud_seq() != remote.cloud_seq() ||
                         local.vehicle_seq() != remote.vehicle_seq());

        VLOG(2) << "  Job " << job.job_id()
                << " local={" << local.cloud_seq() << "," << local.vehicle_seq() << "}"
                << " remote={" << remote.cloud_seq() << "," << remote.vehicle_seq() << "}"
                << " dirty=" << (is_dirty ? "YES" : "no");

        if (is_dirty) {
            dirty.push_back(job);
        }
    }

    VLOG(1) << "GetDirtyJobs for " << vehicle_id << ": " << dirty.size()
            << " dirty out of " << all_jobs.size() << " total";
    return dirty;
}

std::vector<std::string> CloudSchedulerSyncBridge::GetAllJobIds(
    const std::string& vehicle_id) {
    auto jobs = GetCloudJobs(vehicle_id);
    std::vector<std::string> ids;
    ids.reserve(jobs.size());
    for (const auto& job : jobs) {
        ids.push_back(job.job_id());
    }
    return ids;
}

void CloudSchedulerSyncBridge::SetJobRemoteVersion(
    const std::string& vehicle_id,
    const std::string& job_id,
    uint64_t cloud_seq,
    uint64_t vehicle_seq) {

    grpc::ClientContext context;
    sched_pb::set_job_remote_version_request request;
    sched_pb::set_job_remote_version_response response;

    request.set_vehicle_id(vehicle_id);
    request.set_job_id(job_id);
    request.set_cloud_seq(cloud_seq);
    request.set_vehicle_seq(vehicle_seq);

    auto status = set_remote_version_stub_->set_job_remote_version(&context, request, &response);
    if (!status.ok()) {
        LOG(WARNING) << "Failed to set remote_version for " << vehicle_id << "/" << job_id
                     << ": " << status.error_message();
        return;
    }

    if (!response.success()) {
        LOG(WARNING) << "set_job_remote_version failed for " << vehicle_id << "/" << job_id;
    }
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
        // Send full SyncMessage with all jobs (v3.2)
        auto cloud_jobs = GetCloudJobs(vehicle_id);
        std::vector<sync_v3::JobRecord> jobs_to_send;
        for (const auto& job : cloud_jobs) {
            jobs_to_send.push_back(JobInfoToC2VRecord(job));
        }
        SendSyncMessage(vehicle_id, jobs_to_send, {});
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

    // Build C2V_TriggerJob
    sync_v3::C2V_Envelope envelope;
    auto* trigger = envelope.mutable_trigger_job();
    trigger->set_vehicle_id(vehicle_id);
    trigger->set_job_id(job_id);

    // Generate request ID for correlation
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dist;
    std::stringstream ss;
    ss << "req-" << std::hex << dist(gen);
    trigger->set_request_id(ss.str());

    trigger->set_requester_id("cloud-dashboard");
    trigger->set_timestamp_ms(NowMs());
    trigger->set_expires_at_ms(NowMs() + 30000);  // 30 second expiry

    SendC2VEnvelope(vehicle_id, envelope);

    LOG(INFO) << "Sent C2V_TriggerJob to " << vehicle_id << " for job " << job_id;
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
