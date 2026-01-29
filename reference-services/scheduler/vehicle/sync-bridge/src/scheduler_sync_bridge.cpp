/**
 * @file scheduler_sync_bridge.cpp
 * @brief Implementation of SchedulerSyncBridge (v3.2 dirty-first protocol)
 */

#include "scheduler_sync_bridge.hpp"
#include "ifex_content_ids.hpp"

#include <glog/logging.h>
#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <functional>
#include <iomanip>
#include <random>
#include <set>
#include <sstream>

namespace ifex::reference {

using namespace std::chrono_literals;
namespace sync_v3 = swdv::scheduler_sync_v3;
namespace scheduler_pb = swdv::ifex_scheduler;
namespace scheduler_types_pb = swdv::scheduler_types;
namespace sched_lib = ifex::scheduler;

// =============================================================================
// Helper: Convert epoch milliseconds to ISO8601 string
// =============================================================================

static std::string EpochMsToIso8601(uint64_t epoch_ms) {
    if (epoch_ms == 0) {
        return "";  // 0 means "not set" or "no change"
    }

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

// =============================================================================
// Helper: Convert SyncedJobState to library Job for hash computation
// =============================================================================

static sched_lib::Job ToLibraryJob(const SyncedJobState& state) {
    sched_lib::Job job;
    job.job_id = state.job_id;
    job.title = state.title;
    job.service = state.service;
    job.method = state.method;
    job.parameters_json = state.parameters;
    job.scheduled_time_ms = state.scheduled_time_ms;
    job.recurrence_rule = state.recurrence_rule;
    job.end_time_ms = state.end_time_ms;
    job.paused = state.paused;
    job.wake_policy = static_cast<sched_lib::WakePolicy>(state.wake_policy);
    job.sleep_policy = static_cast<sched_lib::SleepPolicy>(state.sleep_policy);
    job.wake_lead_time_s = state.wake_lead_time_s;
    job.status = static_cast<sched_lib::JobStatus>(state.status);
    job.local_version = sched_lib::VersionVector(state.version.cloud_seq, state.version.vehicle_seq);
    job.authority = static_cast<sched_lib::JobAuthority>(state.authority);
    job.deleted = state.deleted;
    return job;
}

// =============================================================================
// SyncedJobState
// =============================================================================

uint64_t SyncedJobState::ComputeHash() const {
    // Use centralized hash computation from ifex-scheduler library
    return sched_lib::compute_job_content_hash(ToLibraryJob(*this));
}

void SyncedJobState::ToJobRecord(sync_v3::JobRecord* record) const {
    record->set_job_id(job_id);
    record->set_authority(authority);

    auto* ver = record->mutable_version();
    ver->set_cloud_seq(version.cloud_seq);
    ver->set_vehicle_seq(version.vehicle_seq);

    record->set_deleted(deleted);

    record->set_title(title);
    record->set_service(service);
    record->set_method(method);
    record->set_parameters_json(parameters);
    record->set_scheduled_time_ms(scheduled_time_ms);
    record->set_recurrence_rule(recurrence_rule);
    if (end_time_ms > 0) {
        record->set_end_time_ms(end_time_ms);
    }
    record->set_paused(paused);

    record->set_status(status);
    record->set_wake_policy(wake_policy);
    record->set_sleep_policy(sleep_policy);
    record->set_wake_lead_time_s(wake_lead_time_s);

    record->set_created_at_ms(created_at_ms);
    record->set_updated_at_ms(updated_at_ms);
}

SyncedJobState SyncedJobState::FromJobRecord(const sync_v3::JobRecord& record) {
    SyncedJobState state;
    state.job_id = record.job_id();
    state.authority = record.authority();
    state.version.cloud_seq = record.version().cloud_seq();
    state.version.vehicle_seq = record.version().vehicle_seq();
    state.deleted = record.deleted();

    state.title = record.title();
    state.service = record.service();
    state.method = record.method();
    state.parameters = record.parameters_json();
    state.scheduled_time_ms = record.scheduled_time_ms();
    state.scheduled_time = EpochMsToIso8601(record.scheduled_time_ms());
    state.recurrence_rule = record.recurrence_rule();
    state.end_time_ms = record.end_time_ms();
    state.next_run_time = EpochMsToIso8601(record.next_run_time_ms());
    state.paused = record.paused();

    state.status = record.status();
    state.wake_policy = record.wake_policy();
    state.sleep_policy = record.sleep_policy();
    state.wake_lead_time_s = record.wake_lead_time_s();

    state.created_at_ms = record.created_at_ms();
    state.updated_at_ms = record.updated_at_ms();

    return state;
}

// =============================================================================
// SchedulerSyncBridge
// =============================================================================

SchedulerSyncBridge::SchedulerSyncBridge(const SchedulerSyncBridgeConfig& config)
    : config_(config)
    , instance_id_(GenerateInstanceId()) {
    LOG(INFO) << "Creating SchedulerSyncBridge instance: " << instance_id_;
}

SchedulerSyncBridge::~SchedulerSyncBridge() {
    Stop();
}

bool SchedulerSyncBridge::Start() {
    if (running_.load()) {
        LOG(WARNING) << "SchedulerSyncBridge already running";
        return true;
    }

    LOG(INFO) << "Starting SchedulerSyncBridge (v3.2 protocol)...";
    LOG(INFO) << "  Scheduler endpoint: " << config_.scheduler_endpoint;
    LOG(INFO) << "  Backend Transport endpoint: " << config_.backend_transport_endpoint;
    LOG(INFO) << "  Sync content_id: " << config_.sync_content_id;
    LOG(INFO) << "  Initialization delay: " << config_.initialization_delay_ms << "ms";

    // Connect to Backend Transport
    auto bt_channel = grpc::CreateChannel(
        config_.backend_transport_endpoint,
        grpc::InsecureChannelCredentials());

    transport_client_ = std::make_unique<client::BackendTransportClient>(
        bt_channel, config_.sync_content_id);

    // Connect to Scheduler service
    scheduler_channel_ = grpc::CreateChannel(
        config_.scheduler_endpoint,
        grpc::InsecureChannelCredentials());

    list_jobs_stub_ = scheduler_pb::list_jobs_service::NewStub(scheduler_channel_);
    create_job_stub_ = scheduler_pb::create_job_service::NewStub(scheduler_channel_);
    update_job_stub_ = scheduler_pb::update_job_service::NewStub(scheduler_channel_);
    delete_job_stub_ = scheduler_pb::delete_job_service::NewStub(scheduler_channel_);
    pause_job_stub_ = scheduler_pb::pause_job_service::NewStub(scheduler_channel_);
    resume_job_stub_ = scheduler_pb::resume_job_service::NewStub(scheduler_channel_);
    trigger_job_stub_ = scheduler_pb::trigger_job_service::NewStub(scheduler_channel_);
    get_job_stub_ = scheduler_pb::get_job_service::NewStub(scheduler_channel_);
    set_remote_version_stub_ = scheduler_pb::set_job_remote_version_service::NewStub(scheduler_channel_);

    // Verify Scheduler connection
    auto deadline = std::chrono::system_clock::now() + 5s;
    if (!scheduler_channel_->WaitForConnected(deadline)) {
        LOG(ERROR) << "Failed to connect to Scheduler service";
        return false;
    }

    // Load persisted state if available
    LoadPersistedState();

    running_.store(true);
    stop_requested_.store(false);
    sync_state_.store(SyncState::DISCONNECTED);
    last_activity_time_ = std::chrono::steady_clock::now();

    // Start worker threads
    poll_thread_ = std::thread(&SchedulerSyncBridge::PollLoop, this);
    execution_retry_thread_ = std::thread(&SchedulerSyncBridge::ExecutionRetryLoop, this);

    // Initialize c2v sync handling
    if (config_.enable_cloud_sync) {
        LOG(INFO) << "Enabling cloud sync handling (c2v)";

        transport_client_->on_content(
            [this](const std::vector<uint8_t>& payload) {
                HandleCloudMessage(payload);
            });
    }

    LOG(INFO) << "SchedulerSyncBridge started";
    return true;
}

void SchedulerSyncBridge::Stop() {
    if (!running_.load()) {
        return;
    }

    LOG(INFO) << "Stopping SchedulerSyncBridge...";

    stop_requested_.store(true);
    running_.store(false);

    // Signal threads to wake up
    cv_.notify_all();

    // Wait for threads
    if (poll_thread_.joinable()) {
        poll_thread_.join();
    }
    if (execution_retry_thread_.joinable()) {
        execution_retry_thread_.join();
    }

    // Persist final state
    PersistState();

    // Cleanup
    transport_client_.reset();
    list_jobs_stub_.reset();
    create_job_stub_.reset();
    update_job_stub_.reset();
    delete_job_stub_.reset();

    LOG(INFO) << "SchedulerSyncBridge stopped";
}

bool SchedulerSyncBridge::IsConnected() const {
    if (!transport_client_) return false;
    return transport_client_->healthy();
}

SchedulerSyncStats SchedulerSyncBridge::GetStats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    SchedulerSyncStats stats = stats_;
    stats.current_sequence = sequence_number_.load();
    stats.is_initialized = initialized_.load();
    stats.is_connected = IsConnected();

    std::lock_guard<std::mutex> state_lock(state_mutex_);
    stats.active_jobs_tracked = synced_state_.size();

    return stats;
}

void SchedulerSyncBridge::ForceFullSync() {
    LOG(INFO) << "Forcing full sync (sending Hello)";
    sync_state_.store(SyncState::SEND_HELLO);
    cv_.notify_all();
}

uint64_t SchedulerSyncBridge::GetStateChecksum() const {
    return ComputeStateChecksumXxHash();
}

bool SchedulerSyncBridge::IsQuiescent() const {
    return sync_state_.load() == SyncState::QUIESCENT;
}

uint64_t SchedulerSyncBridge::GetLastSeenCloudChecksum() const {
    return last_seen_c2v_checksum_.load();
}

// =============================================================================
// Poll Loop (State Machine Driver)
// =============================================================================

void SchedulerSyncBridge::PollLoop() {
    LOG(INFO) << "Poll thread started, waiting " << config_.initialization_delay_ms
              << "ms for initialization";

    // Initialization delay
    {
        std::unique_lock<std::mutex> lock(cv_mutex_);
        cv_.wait_for(lock, std::chrono::milliseconds(config_.initialization_delay_ms),
                     [this]() { return stop_requested_.load(); });
    }

    if (stop_requested_.load()) {
        LOG(INFO) << "Poll thread stopping (during init)";
        return;
    }

    // Initial state capture
    LOG(INFO) << "Initialization complete, capturing initial state";
    auto jobs = QuerySchedulerJobs();

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        synced_state_.clear();
        for (const auto& job : jobs) {
            synced_state_[job.job_id] = job;
        }
    }

    initialized_.store(true);
    LOG(INFO) << "Initial state captured: " << jobs.size() << " jobs";

    // v3.2: Start in SYNCING state, send initial SyncMessage with our checksum
    sync_state_.store(SyncState::SYNCING);
    SendSyncMessage({}, {});

    // Main poll loop - v3.2 simplified two-state model
    //
    // Poll loop responsibilities:
    // 1. Detect local changes (new/modified jobs)
    // 2. If dirty jobs exist, send them → SYNCING
    // 3. Reception handler (HandleSyncMessage) handles convergence → QUIESCENT
    //
    while (!stop_requested_.load()) {
        {
            std::unique_lock<std::mutex> lock(cv_mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(config_.poll_interval_ms),
                         [this]() { return stop_requested_.load(); });
        }

        if (stop_requested_.load()) break;

        // Query current scheduler state
        auto current = QuerySchedulerJobs();

        // Detect changes and check for dirty jobs
        bool has_dirty_jobs = false;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            DetectChanges(current);

            for (const auto& [job_id, state] : synced_state_) {
                if (state.is_dirty()) {
                    has_dirty_jobs = true;
                    break;
                }
            }
        }

        // v3.2: Simple logic - if we have dirty jobs, send them
        if (has_dirty_jobs) {
            auto dirty_jobs = GetDirtyJobs();
            if (!dirty_jobs.empty()) {
                LOG(INFO) << "Poll: sending " << dirty_jobs.size() << " dirty jobs";
                std::vector<sync_v3::JobRecord> jobs_to_send;
                for (const auto& state : dirty_jobs) {
                    jobs_to_send.push_back(StateToJobRecord(state));
                }
                sync_state_.store(SyncState::SYNCING);
                SendSyncMessage(jobs_to_send, {});
            }
        }

        // Send heartbeat if needed
        MaybeSendHeartbeat();
    }

    LOG(INFO) << "Poll thread stopped";
}

// =============================================================================
// Execution Retry Loop
// =============================================================================

void SchedulerSyncBridge::ExecutionRetryLoop() {
    LOG(INFO) << "Execution retry thread started";

    const auto retry_interval = 30s;  // Retry unacked executions every 30s

    while (!stop_requested_.load()) {
        {
            std::unique_lock<std::mutex> lock(cv_mutex_);
            cv_.wait_for(lock, retry_interval,
                         [this]() { return stop_requested_.load(); });
        }

        if (stop_requested_.load()) break;

        // Check for pending executions and resend
        {
            std::lock_guard<std::mutex> lock(executions_mutex_);
            if (!pending_execution_acks_.empty()) {
                LOG(INFO) << "Retrying " << pending_execution_acks_.size()
                          << " unacknowledged executions";
                SendExecutions();
            }
        }
    }

    LOG(INFO) << "Execution retry thread stopped";
}

// =============================================================================
// Query Scheduler Jobs
// =============================================================================

std::vector<SyncedJobState> SchedulerSyncBridge::QuerySchedulerJobs() {
    std::vector<SyncedJobState> result;

    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + 5s);

    scheduler_pb::list_jobs_request request;
    request.mutable_filter()->set_include_deleted(true);

    scheduler_pb::list_jobs_response response;
    auto status = list_jobs_stub_->list_jobs(&context, request, &response);

    if (!status.ok()) {
        LOG(WARNING) << "Failed to query Scheduler: " << status.error_message();
        return result;
    }

    for (const auto& job : response.jobs()) {
        SyncedJobState state;
        state.job_id = job.job_id();
        state.title = job.title();
        state.service = job.service();
        state.method = job.method();
        state.parameters = job.parameters_json();
        state.scheduled_time_ms = job.scheduled_time_ms();
        state.scheduled_time = EpochMsToIso8601(job.scheduled_time_ms());
        state.recurrence_rule = job.recurrence_rule();
        state.end_time_ms = job.end_time_ms();
        state.next_run_time = EpochMsToIso8601(job.next_run_time_ms());
        state.status = MapStatus(job.status());
        state.wake_policy = MapWakePolicy(job.wake_policy());
        state.sleep_policy = MapSleepPolicy(job.sleep_policy());
        state.wake_lead_time_s = job.wake_lead_time_s();
        state.paused = job.paused();

        state.version = sched_lib::VersionVector(
            job.local_version().cloud_seq(), job.local_version().vehicle_seq());
        state.authority = static_cast<sync_v3::JobAuthority>(job.authority());
        state.deleted = job.deleted();

        state.created_at_ms = job.created_at_ms();
        state.updated_at_ms = job.updated_at_ms();

        result.push_back(std::move(state));
    }

    return result;
}

// =============================================================================
// Get Single Job from Scheduler
// =============================================================================

std::optional<SyncedJobState> SchedulerSyncBridge::GetJobFromScheduler(const std::string& job_id) {
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + 5s);

    scheduler_pb::get_job_request request;
    request.set_job_id(job_id);
    request.set_include_deleted(true);  // Sync needs to see tombstones

    scheduler_pb::get_job_response response;
    auto status = get_job_stub_->get_job(&context, request, &response);

    if (!status.ok()) {
        VLOG(1) << "GetJobFromScheduler failed for " << job_id << ": " << status.error_message();
        return std::nullopt;
    }

    if (!response.has_job()) {
        VLOG(1) << "GetJobFromScheduler: job " << job_id << " not found";
        return std::nullopt;
    }

    const auto& job = response.job();
    SyncedJobState state;
    state.job_id = job.job_id();
    state.title = job.title();
    state.service = job.service();
    state.method = job.method();
    state.parameters = job.parameters_json();
    state.scheduled_time_ms = job.scheduled_time_ms();
    state.scheduled_time = EpochMsToIso8601(job.scheduled_time_ms());
    state.recurrence_rule = job.recurrence_rule();
    state.end_time_ms = job.end_time_ms();
    state.next_run_time = EpochMsToIso8601(job.next_run_time_ms());
    state.status = MapStatus(job.status());
    state.wake_policy = MapWakePolicy(job.wake_policy());
    state.sleep_policy = MapSleepPolicy(job.sleep_policy());
    state.wake_lead_time_s = job.wake_lead_time_s();
    state.paused = job.paused();

    state.version = sched_lib::VersionVector(
        job.local_version().cloud_seq(), job.local_version().vehicle_seq());
    state.authority = static_cast<sync_v3::JobAuthority>(job.authority());
    state.deleted = job.deleted();

    state.created_at_ms = job.created_at_ms();
    state.updated_at_ms = job.updated_at_ms();

    return state;
}

// =============================================================================
// Detect Changes
// =============================================================================

void SchedulerSyncBridge::DetectChanges(const std::vector<SyncedJobState>& current) {
    // Note: state_mutex_ must be held by caller

    std::unordered_map<std::string, const SyncedJobState*> current_map;
    for (const auto& job : current) {
        current_map[job.job_id] = &job;
    }

    // Check for new or changed jobs
    for (const auto& job : current) {
        auto it = synced_state_.find(job.job_id);

        if (it == synced_state_.end()) {
            // New job - increment version if not already set
            SyncedJobState new_state = job;
            if (new_state.version.vehicle_seq == 0 && new_state.version.cloud_seq == 0) {
                new_state.version.vehicle_seq = 1;  // New local job starts at {0,1}
            }
            synced_state_[job.job_id] = new_state;

            if (job.IsTerminal()) {
                RecordExecution(job.job_id, job.updated_at_ms, 0, job.status, "", "");
            }
            LOG(INFO) << "Job " << (job.deleted ? "deleted" : "created") << ": "
                      << job.title << " (id=" << job.job_id
                      << ", v={" << new_state.version.cloud_seq << "," << new_state.version.vehicle_seq << "})";
        } else {
            auto& synced = it->second;

            // Check for terminal transition
            bool was_terminal = synced.IsTerminal();
            bool is_terminal = job.IsTerminal();

            if (!was_terminal && is_terminal) {
                RecordExecution(job.job_id, job.updated_at_ms, 0, job.status, "", "");
                LOG(INFO) << "Job completed: " << job.title << " (id=" << job.job_id << ")";
            }

            // Check for content/version/authority changes
            // Note: authority is included in state checksum, so we must detect its changes
            bool content_changed = job.ComputeHash() != synced.ComputeHash();
            bool version_changed = job.version != synced.version;
            bool deleted_changed = job.deleted != synced.deleted;
            bool authority_changed = job.authority != synced.authority;

            if (content_changed || version_changed || deleted_changed || authority_changed) {
                SyncedJobState updated_state = job;

                // If content changed but version didn't, increment vehicle_seq to mark dirty
                if ((content_changed || deleted_changed || authority_changed) && !version_changed) {
                    updated_state.version.vehicle_seq = synced.version.vehicle_seq + 1;
                    LOG(INFO) << "Job content changed, bumped version: " << job.title
                              << " v={" << updated_state.version.cloud_seq << ","
                              << updated_state.version.vehicle_seq << "}";
                } else {
                    VLOG(1) << "Job updated: " << job.title;
                }

                // Preserve synced_version from previous state
                updated_state.synced_version = synced.synced_version;
                it->second = updated_state;
            }
        }
    }

    // Remove jobs no longer in scheduler
    for (auto it = synced_state_.begin(); it != synced_state_.end(); ) {
        if (current_map.find(it->first) == current_map.end()) {
            LOG(INFO) << "Job removed from cache: " << it->first;
            it = synced_state_.erase(it);
        } else {
            ++it;
        }
    }
}

// =============================================================================
// Heartbeat
// =============================================================================

void SchedulerSyncBridge::MaybeSendHeartbeat() {
    if (config_.heartbeat_interval_ms == 0) return;
    if (sync_state_.load() != SyncState::QUIESCENT) return;

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - last_activity_time_).count();

    if (elapsed >= config_.heartbeat_interval_ms) {
        // v3.2: Heartbeat is just an empty SyncMessage
        SendSyncMessage({}, {});
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.heartbeats_sent++;
    }
}

// =============================================================================
// V2C Message Sending (v3.1 Protocol)
// =============================================================================

void SchedulerSyncBridge::SendV2CEnvelope(const sync_v3::V2C_Envelope& envelope) {
    std::string serialized;
    if (!envelope.SerializeToString(&serialized)) {
        LOG(ERROR) << "Failed to serialize V2C_Envelope";
        return;
    }

    std::vector<uint8_t> payload(serialized.begin(), serialized.end());
    auto result = transport_client_->publish(payload, client::Persistence::Volatile);

    if (result.ok()) {
        ++sequence_number_;
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.events_sent++;
        stats_.bytes_sent += serialized.size();
        stats_.last_sync_timestamp_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
    } else {
        LOG(WARNING) << "Failed to send V2C_Envelope: status="
                     << static_cast<int>(result.status);
    }

    last_activity_time_ = std::chrono::steady_clock::now();
}

void SchedulerSyncBridge::SendSyncMessage(
    const std::vector<sync_v3::JobRecord>& jobs,
    const std::vector<sync_v3::JobVersionAck>& acked_jobs) {

    sync_v3::V2C_Envelope envelope;
    auto* sync_msg = envelope.mutable_sync();
    sync_msg->set_vehicle_id(config_.vehicle_id);

    // Add jobs to send
    for (const auto& job : jobs) {
        *sync_msg->add_jobs() = job;
    }

    // Add acknowledgments for jobs we received
    for (const auto& ack : acked_jobs) {
        *sync_msg->add_acked_jobs() = ack;
    }

    sync_msg->set_state_checksum(ComputeStateChecksumXxHash());

    SendV2CEnvelope(envelope);
    sync_state_.store(SyncState::WAIT_RESPONSE);

    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.sync_messages_sent++;

    LOG(INFO) << "Sent V2C SyncMessage: " << sync_msg->jobs_size() << " jobs"
              << ", " << sync_msg->acked_jobs_size() << " acks"
              << ", checksum=" << std::hex << sync_msg->state_checksum() << std::dec;
}

void SchedulerSyncBridge::SendGapDetect(
    const std::vector<std::string>& job_ids,
    const std::vector<std::string>& request_job_ids) {

    sync_v3::V2C_Envelope envelope;
    auto* gap_detect = envelope.mutable_gap_detect();
    gap_detect->set_vehicle_id(config_.vehicle_id);

    // Add all our job IDs
    for (const auto& id : job_ids) {
        gap_detect->add_job_ids(id);
    }

    // Add jobs we need from cloud
    for (const auto& id : request_job_ids) {
        gap_detect->add_request_job_ids(id);
    }

    SendV2CEnvelope(envelope);
    // Don't change state - gap detection is a sub-protocol

    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.gap_detects_sent++;

    LOG(INFO) << "Sent V2C GapDetect: " << gap_detect->job_ids_size() << " job_ids"
              << ", " << gap_detect->request_job_ids_size() << " requests";
}

void SchedulerSyncBridge::SendExecutions() {
    std::lock_guard<std::mutex> lock(executions_mutex_);
    if (pending_execution_acks_.empty()) return;

    sync_v3::V2C_Envelope envelope;
    auto* executions = envelope.mutable_executions();
    executions->set_vehicle_id(config_.vehicle_id);

    for (const auto& [exec_id, exec] : pending_execution_acks_) {
        *executions->add_executions() = exec;
    }

    SendV2CEnvelope(envelope);

    std::lock_guard<std::mutex> stats_lock(stats_mutex_);
    stats_.executions_sent++;

    LOG(INFO) << "Sent V2C_Executions: " << executions->executions_size() << " executions";
}

void SchedulerSyncBridge::SendTriggerResponse(const std::string& job_id,
                                              const std::string& request_id,
                                              bool accepted,
                                              const std::string& error_message) {
    sync_v3::V2C_Envelope envelope;
    auto* response = envelope.mutable_trigger_response();
    response->set_vehicle_id(config_.vehicle_id);
    response->set_job_id(job_id);
    response->set_request_id(request_id);
    response->set_accepted(accepted);
    if (!error_message.empty()) {
        response->set_error_message(error_message);
    }
    response->set_timestamp_ms(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    SendV2CEnvelope(envelope);

    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.trigger_responses_sent++;

    LOG(INFO) << "Sent V2C_TriggerResponse: job=" << job_id
              << ", accepted=" << (accepted ? "true" : "false");
}

// =============================================================================
// C2V Message Handling (v3.2 Protocol)
// =============================================================================

void SchedulerSyncBridge::HandleCloudMessage(const std::vector<uint8_t>& payload) {
    sync_v3::C2V_Envelope envelope;
    if (!envelope.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
        LOG(WARNING) << "Failed to parse C2V_Envelope (" << payload.size() << " bytes)";
        return;
    }

    switch (envelope.message_case()) {
        // v3.2 messages
        case sync_v3::C2V_Envelope::kSync:
            HandleSyncMessage(envelope.sync());
            break;
        case sync_v3::C2V_Envelope::kGapDetect:
            HandleGapDetect(envelope.gap_detect());
            break;

        // Other messages
        case sync_v3::C2V_Envelope::kExecutionAck:
            HandleExecutionAck(envelope.execution_ack());
            break;
        case sync_v3::C2V_Envelope::kTriggerJob:
            HandleTriggerJob(envelope.trigger_job());
            break;
        case sync_v3::C2V_Envelope::MESSAGE_NOT_SET:
            LOG(WARNING) << "Received empty C2V_Envelope";
            break;
    }
}

void SchedulerSyncBridge::HandleSyncMessage(const sync_v3::SyncMessage& msg) {
    // v3.2 Event-driven handler:
    // - Receive cloud's state (jobs + acks + checksum)
    // - ACK what we received
    // - Check if we're in sync (quiescent) or need to continue
    //
    // This is purely reactive - poll loop handles proactive dirty job sending

    LOG(INFO) << "Received C2V SyncMessage: jobs=" << msg.jobs_size()
              << ", acked_jobs=" << msg.acked_jobs_size()
              << ", checksum=" << std::hex << msg.state_checksum() << std::dec;

    last_seen_c2v_checksum_.store(msg.state_checksum());

    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.sync_messages_received++;
    }

    // Step 1: Process ACKs from cloud - mark jobs as synced
    for (const auto& ack : msg.acked_jobs()) {
        SetJobRemoteVersion(ack.job_id(), ack.cloud_seq(), ack.vehicle_seq());
    }

    // Step 2: Apply received jobs and prepare ACKs
    std::vector<sync_v3::JobVersionAck> acks_to_send;
    for (const auto& job : msg.jobs()) {
        ProcessCloudJob(job);
        // ACK each job we received
        sync_v3::JobVersionAck ack;
        ack.set_job_id(job.job_id());
        ack.set_cloud_seq(job.version().cloud_seq());
        ack.set_vehicle_seq(job.version().vehicle_seq());
        acks_to_send.push_back(ack);
    }

    // Step 3: Check quiescence - are we in sync?
    uint64_t our_checksum = ComputeStateChecksumXxHash();
    auto dirty_jobs = GetDirtyJobs();

    if (our_checksum == msg.state_checksum() && dirty_jobs.empty()) {
        // QUIESCENT - checksums match and no dirty jobs
        LOG(INFO) << "QUIESCENT: vehicle=" << std::hex << our_checksum
                  << " cloud=" << msg.state_checksum() << std::dec;
        sync_state_.store(SyncState::QUIESCENT);

        {
            std::lock_guard<std::mutex> stats_lock(stats_mutex_);
            stats_.quiescent_count++;
        }

        // Send ACKs if we have any (final handshake)
        if (!acks_to_send.empty()) {
            SendSyncMessage({}, acks_to_send);
        }
        return;
    }

    // Step 4: Not quiescent - respond with ACKs (and dirty jobs if any)
    // Note: Poll loop also sends dirty jobs proactively, but we include
    // them here for faster convergence when responding to cloud messages
    sync_state_.store(SyncState::SYNCING);

    if (!dirty_jobs.empty() || !acks_to_send.empty()) {
        std::vector<sync_v3::JobRecord> jobs_to_send;
        for (const auto& state : dirty_jobs) {
            jobs_to_send.push_back(StateToJobRecord(state));
        }
        SendSyncMessage(jobs_to_send, acks_to_send);
    } else if (our_checksum != msg.state_checksum()) {
        // Checksum mismatch but no dirty jobs - need gap detection
        LOG(INFO) << "Checksum mismatch (ours=" << std::hex << our_checksum
                  << " cloud=" << msg.state_checksum() << std::dec
                  << ") but no dirty jobs, triggering gap detection";
        SendGapDetect(GetAllJobIds(), {});
    }
}

void SchedulerSyncBridge::HandleGapDetect(const sync_v3::GapDetect& msg) {
    LOG(INFO) << "Received C2V GapDetect: job_ids=" << msg.job_ids_size()
              << ", request_job_ids=" << msg.request_job_ids_size();

    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.gap_detects_received++;
    }

    auto our_ids = GetAllJobIds();
    std::set<std::string> our_set(our_ids.begin(), our_ids.end());
    std::set<std::string> cloud_set(msg.job_ids().begin(), msg.job_ids().end());

    // Jobs we need from cloud
    std::vector<std::string> request_from_cloud;
    for (const auto& id : cloud_set) {
        if (our_set.find(id) == our_set.end()) {
            request_from_cloud.push_back(id);
        }
    }

    // Jobs cloud needs from us
    std::vector<sync_v3::JobRecord> jobs_to_send;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        for (const auto& id : our_set) {
            if (cloud_set.find(id) == cloud_set.end()) {
                auto it = synced_state_.find(id);
                if (it != synced_state_.end()) {
                    jobs_to_send.push_back(StateToJobRecord(it->second));
                }
            }
        }

        // Fulfill specific requests from cloud
        for (const auto& id : msg.request_job_ids()) {
            auto it = synced_state_.find(id);
            if (it != synced_state_.end()) {
                // Check if already in jobs_to_send
                bool already_added = false;
                for (const auto& job : jobs_to_send) {
                    if (job.job_id() == id) {
                        already_added = true;
                        break;
                    }
                }
                if (!already_added) {
                    jobs_to_send.push_back(StateToJobRecord(it->second));
                }
            }
        }
    }

    LOG(INFO) << "Gap detection: request_from_cloud=" << request_from_cloud.size()
              << ", jobs_to_send=" << jobs_to_send.size();

    // If job_ids match but checksums differ, the issue is content mismatch.
    // Fall back to sending ALL our jobs to force sync convergence.
    if (request_from_cloud.empty() && jobs_to_send.empty()) {
        LOG(INFO) << "Gap detection: job_ids match but checksum differs - "
                  << "sending all " << our_ids.size() << " jobs to force sync";
        std::lock_guard<std::mutex> lock(state_mutex_);
        for (const auto& [id, state] : synced_state_) {
            jobs_to_send.push_back(StateToJobRecord(state));
        }
    }

    // Send responses
    if (!request_from_cloud.empty()) {
        SendGapDetect(our_ids, request_from_cloud);
    }
    if (!jobs_to_send.empty()) {
        SendSyncMessage(jobs_to_send, {});
    }
}

void SchedulerSyncBridge::HandleExecutionAck(const sync_v3::C2V_ExecutionAck& ack) {
    LOG(INFO) << "Received C2V_ExecutionAck: " << ack.execution_ids_size() << " executions";

    std::lock_guard<std::mutex> lock(executions_mutex_);
    for (const auto& exec_id : ack.execution_ids()) {
        pending_execution_acks_.erase(exec_id);
    }

    std::lock_guard<std::mutex> stats_lock(stats_mutex_);
    stats_.execution_acks_received++;
}

void SchedulerSyncBridge::HandleTriggerJob(const sync_v3::C2V_TriggerJob& trigger) {
    LOG(INFO) << "Received C2V_TriggerJob: job=" << trigger.job_id()
              << ", request_id=" << trigger.request_id();

    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.trigger_requests_received++;
    }

    // Check for request expiry
    if (trigger.expires_at_ms() > 0) {
        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        if (static_cast<uint64_t>(now_ms) > trigger.expires_at_ms()) {
            LOG(WARNING) << "TriggerJob request expired for job " << trigger.job_id();
            SendTriggerResponse(trigger.job_id(), trigger.request_id(), false, "Request expired");
            return;
        }
    }

    // Execute trigger via Scheduler gRPC
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() +
                         std::chrono::milliseconds(config_.operation_timeout_ms));

    scheduler_pb::trigger_job_request grpc_request;
    grpc_request.set_job_id(trigger.job_id());

    scheduler_pb::trigger_job_response grpc_response;
    auto status = trigger_job_stub_->trigger_job(&context, grpc_request, &grpc_response);

    bool success = false;
    std::string error_message;

    if (!status.ok()) {
        error_message = "gRPC error: " + status.error_message();
    } else if (!grpc_response.success()) {
        error_message = grpc_response.message();
    } else {
        success = true;
    }

    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        if (success) {
            stats_.trigger_requests_succeeded++;
        } else {
            stats_.trigger_requests_failed++;
        }
    }

    SendTriggerResponse(trigger.job_id(), trigger.request_id(), success, error_message);
}

// =============================================================================
// Job Operations
// =============================================================================

SchedulerSyncBridge::OperationResult SchedulerSyncBridge::CreateJobFromCloud(
    const sync_v3::JobRecord& job) {

    OperationResult result;
    result.job_id = job.job_id();

    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() +
                         std::chrono::milliseconds(config_.operation_timeout_ms));

    scheduler_pb::create_job_request request;
    auto* new_job = request.mutable_job();

    new_job->set_job_id(job.job_id());
    new_job->set_title(job.title());
    new_job->set_service(job.service());
    new_job->set_method(job.method());
    new_job->set_parameters_json(job.parameters_json());
    new_job->set_scheduled_time_ms(job.scheduled_time_ms());
    new_job->set_recurrence_rule(job.recurrence_rule());
    new_job->set_end_time_ms(job.end_time_ms());

    new_job->set_wake_policy(job.wake_policy() == sync_v3::WAKE_REQUIRED
        ? scheduler_types_pb::WAKE_REQUIRED : scheduler_types_pb::WAKE_NO_WAKE);
    new_job->set_sleep_policy(job.sleep_policy() == sync_v3::SLEEP_INHIBIT
        ? scheduler_types_pb::SLEEP_INHIBIT : scheduler_types_pb::SLEEP_NORMAL);
    new_job->set_wake_lead_time_s(job.wake_lead_time_s());

    new_job->set_paused(job.paused());
    new_job->set_authority(static_cast<scheduler_types_pb::job_authority_t>(job.authority()));
    new_job->set_cloud_seq(job.version().cloud_seq());
    new_job->set_vehicle_seq(job.version().vehicle_seq());

    if (job.deleted()) {
        new_job->set_deleted(true);
    }

    scheduler_pb::create_job_response response;
    auto status = create_job_stub_->create_job(&context, request, &response);

    if (!status.ok()) {
        result.success = false;
        result.error_message = "gRPC error: " + status.error_message();
        return result;
    }

    if (!response.success()) {
        result.success = false;
        result.error_message = response.message();
        return result;
    }

    result.success = true;
    result.job_id = response.job_id();

    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.jobs_created_from_cloud++;
    }

    LOG(INFO) << "Created job from cloud: " << job.title() << " (id=" << result.job_id << ")";
    return result;
}

SchedulerSyncBridge::OperationResult SchedulerSyncBridge::UpdateJobFromCloud(
    const sync_v3::JobRecord& job) {

    OperationResult result;
    result.job_id = job.job_id();

    if (job.job_id().empty()) {
        result.success = false;
        result.error_message = "job_id is required for update";
        return result;
    }

    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() +
                         std::chrono::milliseconds(config_.operation_timeout_ms));

    scheduler_pb::update_job_request request;
    request.set_job_id(job.job_id());

    auto* updates = request.mutable_updates();
    updates->set_title(job.title());
    updates->set_scheduled_time_ms(job.scheduled_time_ms());
    updates->set_recurrence_rule(job.recurrence_rule());
    updates->set_parameters_json(job.parameters_json());
    updates->set_end_time_ms(job.end_time_ms());
    updates->set_paused(job.paused());
    updates->set_authority(static_cast<scheduler_types_pb::job_authority_t>(job.authority()));
    updates->set_cloud_seq(job.version().cloud_seq());
    updates->set_vehicle_seq(job.version().vehicle_seq());

    if (job.deleted()) {
        updates->set_deleted(true);
    }

    scheduler_pb::update_job_response response;
    auto status = update_job_stub_->update_job(&context, request, &response);

    if (!status.ok()) {
        result.success = false;
        result.error_message = "gRPC error: " + status.error_message();
        return result;
    }

    if (!response.success()) {
        result.success = false;
        result.error_message = response.message();
        return result;
    }

    result.success = true;

    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.jobs_updated_from_cloud++;
    }

    LOG(INFO) << "Updated job from cloud: " << job.job_id();
    return result;
}

SchedulerSyncBridge::OperationResult SchedulerSyncBridge::DeleteJobFromScheduler(
    const std::string& job_id) {

    OperationResult result;
    result.job_id = job_id;

    if (job_id.empty()) {
        result.success = false;
        result.error_message = "job_id is required for delete";
        return result;
    }

    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() +
                         std::chrono::milliseconds(config_.operation_timeout_ms));

    scheduler_pb::delete_job_request request;
    request.set_job_id(job_id);

    scheduler_pb::delete_job_response response;
    auto status = delete_job_stub_->delete_job(&context, request, &response);

    if (!status.ok()) {
        result.success = false;
        result.error_message = "gRPC error: " + status.error_message();
        return result;
    }

    if (!response.success()) {
        result.success = false;
        result.error_message = response.message();
        return result;
    }

    result.success = true;

    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.jobs_deleted_from_cloud++;
    }

    LOG(INFO) << "Deleted job from cloud sync: " << job_id;
    return result;
}

void SchedulerSyncBridge::ProcessCloudJob(const sync_v3::JobRecord& remote_job) {
    const std::string& job_id = remote_job.job_id();

    std::optional<sched_lib::VersionVector> local_version;
    sync_v3::JobAuthority authority = remote_job.authority();

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = synced_state_.find(job_id);
        if (it != synced_state_.end()) {
            local_version = it->second.version;
        }
    }

    // If not in synced_state_, query the scheduler directly.
    // This handles the race condition during initialization delay:
    // the scheduler may have the job but synced_state_ hasn't been populated yet.
    if (!local_version.has_value()) {
        auto scheduler_job = GetJobFromScheduler(job_id);
        if (scheduler_job) {
            local_version = scheduler_job->version;
            LOG(INFO) << "Job " << job_id << ": found in scheduler (not in synced_state_), "
                      << "version={" << local_version->cloud_seq << "," << local_version->vehicle_seq << "}";
        }
    }

    sched_lib::VersionVector remote_version(
        remote_job.version().cloud_seq(),
        remote_job.version().vehicle_seq());

    sched_lib::SyncResult result = sched_lib::SyncEngine::process_remote(
        remote_version,
        local_version,
        static_cast<sched_lib::JobAuthority>(authority),
        false  // We are vehicle side
    );

    switch (result.action) {
        case sched_lib::SyncResult::NO_ACTION:
            // Already in sync - synced_version will be set when QUIESCENT
            VLOG(1) << "Job " << job_id << ": no action (already in sync)";
            break;

        case sched_lib::SyncResult::ACCEPT_REMOTE:
            // Apply cloud job - synced_version will be set when QUIESCENT
            VLOG(1) << "Job " << job_id << ": accepting remote version";
            ApplyCloudJob(remote_job);
            break;

        case sched_lib::SyncResult::REJECT_REMOTE:
            // Keep local - synced_version stays unchanged (cloud needs our version)
            // is_dirty() will remain true until cloud confirms our version
            VLOG(1) << "Job " << job_id << ": rejecting remote (local dominates)";
            break;

        case sched_lib::SyncResult::CONFLICT_RESOLVED:
            LOG(INFO) << "Job " << job_id << ": conflict resolved, winner="
                      << sched_lib::job_authority_to_string(result.winner);
            if (result.winner == sched_lib::JobAuthority::CLOUD) {
                // Cloud won - apply their version
                ApplyCloudJob(remote_job);
            }
            // Update version to merged version
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                auto it = synced_state_.find(job_id);
                if (it != synced_state_.end()) {
                    it->second.version = result.resolved_version;
                    // synced_version will be set when QUIESCENT
                }
            }
            break;
    }

    // Update the scheduler's remote_version to track what cloud has.
    // This is called regardless of ACCEPT/REJECT/NO_ACTION because the incoming
    // version IS what cloud has - we need to track this for is_dirty() computation.
    {
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + 5s);

        scheduler_pb::set_job_remote_version_request request;
        request.set_job_id(job_id);
        request.set_cloud_seq(remote_version.cloud_seq);
        request.set_vehicle_seq(remote_version.vehicle_seq);

        scheduler_pb::set_job_remote_version_response response;
        auto status = set_remote_version_stub_->set_job_remote_version(&context, request, &response);

        if (!status.ok()) {
            LOG(WARNING) << "Failed to set remote_version for job " << job_id << ": " << status.error_message();
        } else if (!response.success()) {
            LOG(WARNING) << "Failed to set remote_version for job " << job_id << ": " << response.message();
        }
    }
}

void SchedulerSyncBridge::ApplyCloudJob(const sync_v3::JobRecord& remote_job) {
    const std::string& job_id = remote_job.job_id();

    bool exists_locally = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        exists_locally = (synced_state_.find(job_id) != synced_state_.end());
    }

    OperationResult result;
    if (exists_locally) {
        result = UpdateJobFromCloud(remote_job);
    } else {
        result = CreateJobFromCloud(remote_job);
    }

    if (result.success) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        synced_state_[job_id] = SyncedJobState::FromJobRecord(remote_job);
        // synced_version starts at {0,0}, so is_dirty() returns true until cloud confirms

        LOG(INFO) << "Applied cloud job " << job_id << " (" << remote_job.title() << ")";
    } else {
        LOG(WARNING) << "Failed to apply cloud job " << job_id << ": " << result.error_message;

        std::lock_guard<std::mutex> stats_lock(stats_mutex_);
        stats_.jobs_rejected++;
    }
}

// =============================================================================
// State & Checksum
// =============================================================================

uint64_t SchedulerSyncBridge::ComputeStateChecksumXxHash() const {
    // Query scheduler directly - checksum represents actual scheduler state
    // This ensures local changes are detected and synced to cloud
    auto scheduler_jobs = const_cast<SchedulerSyncBridge*>(this)->QuerySchedulerJobs();

    std::vector<sched_lib::Job> jobs;
    jobs.reserve(scheduler_jobs.size());

    for (const auto& state : scheduler_jobs) {
        auto lib_job = ToLibraryJob(state);
        jobs.push_back(lib_job);
    }

    std::sort(jobs.begin(), jobs.end(),
              [](const sched_lib::Job& a, const sched_lib::Job& b) {
                  return a.job_id < b.job_id;
              });

    // DEBUG: Log jobs used in checksum
    LOG(INFO) << "DEBUG ComputeStateChecksumXxHash: " << jobs.size() << " jobs from scheduler:";
    for (const auto& job : jobs) {
        uint64_t job_hash = sched_lib::compute_job_content_hash(job);
        LOG(INFO) << "  - " << job.job_id
                  << " version={" << job.local_version.cloud_seq << "," << job.local_version.vehicle_seq << "}"
                  << " deleted=" << job.deleted
                  << " authority=" << static_cast<int>(job.authority)
                  << " content_hash=" << std::hex << job_hash << std::dec;
    }

    uint64_t checksum = sched_lib::compute_state_checksum(jobs);
    LOG(INFO) << "DEBUG ComputeStateChecksumXxHash: result=" << std::hex << checksum << std::dec;
    return checksum;
}

void SchedulerSyncBridge::RecordExecution(const std::string& job_id,
                                         uint64_t executed_at_ms,
                                         uint64_t duration_ms,
                                         sync_v3::JobStatus status,
                                         const std::string& result_json,
                                         const std::string& error_message) {
    sync_v3::ExecutionRecord exec;
    exec.set_execution_id(GenerateExecutionId());
    exec.set_job_id(job_id);
    exec.set_executed_at_ms(executed_at_ms);
    exec.set_duration_ms(duration_ms);
    exec.set_status(status);
    if (!result_json.empty()) {
        exec.set_result_json(result_json);
    }
    if (!error_message.empty()) {
        exec.set_error_message(error_message);
    }

    {
        std::lock_guard<std::mutex> lock(executions_mutex_);
        pending_execution_acks_[exec.execution_id()] = exec;
    }

    // Send immediately
    SendExecutions();

    VLOG(1) << "Recorded execution for job " << job_id;
}

// =============================================================================
// Helpers
// =============================================================================

std::string SchedulerSyncBridge::GenerateInstanceId() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint64_t> dis;

    std::stringstream ss;
    ss << "ssb_" << std::hex << std::setw(16) << std::setfill('0') << dis(gen);
    return ss.str();
}

std::string SchedulerSyncBridge::GenerateExecutionId() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<uint64_t> dist;

    std::ostringstream oss;
    oss << "exec-" << std::hex << dist(gen);
    return oss.str();
}

void SchedulerSyncBridge::LoadPersistedState() {
    if (config_.state_persistence_path.empty()) return;

    std::ifstream file(config_.state_persistence_path, std::ios::binary);
    if (!file.is_open()) {
        LOG(INFO) << "No persisted state found at " << config_.state_persistence_path;
        return;
    }

    uint64_t seq;
    file.read(reinterpret_cast<char*>(&seq), sizeof(seq));
    if (file.gcount() == sizeof(seq)) {
        sequence_number_.store(seq);
        LOG(INFO) << "Loaded persisted sequence number: " << seq;
    }

    file.close();
}

void SchedulerSyncBridge::PersistState() {
    if (config_.state_persistence_path.empty()) return;

    std::ofstream file(config_.state_persistence_path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        LOG(WARNING) << "Failed to open state file for writing: "
                     << config_.state_persistence_path;
        return;
    }

    uint64_t seq = sequence_number_.load();
    file.write(reinterpret_cast<const char*>(&seq), sizeof(seq));

    file.close();
    LOG(INFO) << "Persisted sync state (sequence=" << seq << ")";
}

sync_v3::JobStatus SchedulerSyncBridge::MapStatus(
    scheduler_types_pb::job_status_t status) {
    switch (status) {
        case scheduler_types_pb::JOB_STATUS_PENDING:
            return sync_v3::JOB_STATUS_PENDING;
        case scheduler_types_pb::JOB_STATUS_RUNNING:
            return sync_v3::JOB_STATUS_RUNNING;
        case scheduler_types_pb::JOB_STATUS_COMPLETED:
            return sync_v3::JOB_STATUS_COMPLETED;
        case scheduler_types_pb::JOB_STATUS_FAILED:
            return sync_v3::JOB_STATUS_FAILED;
        case scheduler_types_pb::JOB_STATUS_CANCELLED:
            return sync_v3::JOB_STATUS_CANCELLED;
    }
    return sync_v3::JOB_STATUS_PENDING;
}

sync_v3::WakePolicy SchedulerSyncBridge::MapWakePolicy(
    scheduler_types_pb::wake_policy_t policy) {
    switch (policy) {
        case scheduler_types_pb::WAKE_REQUIRED:
            return sync_v3::WAKE_REQUIRED;
        case scheduler_types_pb::WAKE_NO_WAKE:
        default:
            return sync_v3::WAKE_NO_WAKE;
    }
}

sync_v3::SleepPolicy SchedulerSyncBridge::MapSleepPolicy(
    scheduler_types_pb::sleep_policy_t policy) {
    switch (policy) {
        case scheduler_types_pb::SLEEP_INHIBIT:
            return sync_v3::SLEEP_INHIBIT;
        case scheduler_types_pb::SLEEP_NORMAL:
        default:
            return sync_v3::SLEEP_NORMAL;
    }
}

// =============================================================================
// v3.2 Helper Methods (Dirty-first sync)
// =============================================================================

std::vector<SyncedJobState> SchedulerSyncBridge::GetDirtyJobs() const {
    std::vector<SyncedJobState> dirty;
    std::lock_guard<std::mutex> lock(state_mutex_);
    for (const auto& [job_id, state] : synced_state_) {
        if (state.is_dirty()) {
            dirty.push_back(state);
        }
    }
    return dirty;
}

std::vector<std::string> SchedulerSyncBridge::GetAllJobIds() const {
    std::vector<std::string> ids;
    std::lock_guard<std::mutex> lock(state_mutex_);
    for (const auto& [job_id, _] : synced_state_) {
        ids.push_back(job_id);
    }
    return ids;
}

void SchedulerSyncBridge::SetJobRemoteVersion(const std::string& job_id, uint64_t cloud_seq, uint64_t vehicle_seq) {
    // Update local synced_state_
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = synced_state_.find(job_id);
        if (it != synced_state_.end()) {
            it->second.synced_version = sched_lib::VersionVector(cloud_seq, vehicle_seq);
            VLOG(1) << "Updated synced_version for job " << job_id
                    << " to {" << cloud_seq << "," << vehicle_seq << "}";
        }
    }

    // Also update the scheduler's remote_version via gRPC
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));

    scheduler_pb::set_job_remote_version_request request;
    request.set_job_id(job_id);
    request.set_cloud_seq(cloud_seq);
    request.set_vehicle_seq(vehicle_seq);

    scheduler_pb::set_job_remote_version_response response;
    auto status = set_remote_version_stub_->set_job_remote_version(&context, request, &response);

    if (!status.ok()) {
        LOG(WARNING) << "Failed to set remote_version for job " << job_id << ": " << status.error_message();
    } else if (!response.success()) {
        LOG(WARNING) << "Failed to set remote_version for job " << job_id << ": " << response.message();
    }
}

sync_v3::JobRecord SchedulerSyncBridge::StateToJobRecord(const SyncedJobState& state) const {
    sync_v3::JobRecord record;
    state.ToJobRecord(&record);
    return record;
}

}  // namespace ifex::reference
