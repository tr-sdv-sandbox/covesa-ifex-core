/**
 * @file scheduler_sync_bridge.cpp
 * @brief Implementation of SchedulerSyncBridge
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
#include <sstream>

namespace ifex::reference {

using namespace std::chrono_literals;
namespace sync_v2 = swdv::scheduler_sync_v2;
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
    job.version = sched_lib::VersionVector(state.version.cloud_seq, state.version.vehicle_seq);
    job.authority = static_cast<sched_lib::JobAuthority>(state.authority);
    job.deleted = state.deleted;
    job.deleted_at_ms = state.deleted_at_ms;
    return job;
}

// =============================================================================
// SyncedJobState
// =============================================================================

uint64_t SyncedJobState::ComputeHash() const {
    // Use centralized hash computation from ifex-scheduler library
    // Included fields: job_id, title, service, method, parameters_json, scheduled_time_ms,
    //                  recurrence_rule, end_time_ms, paused, wake_policy, sleep_policy, wake_lead_time_s
    // Excluded fields: status, next_run_time_ms, created_at_ms, updated_at_ms, version
    return sched_lib::compute_job_content_hash(ToLibraryJob(*this));
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

    LOG(INFO) << "Starting SchedulerSyncBridge...";
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

    get_jobs_stub_ = scheduler_pb::get_jobs_service::NewStub(scheduler_channel_);
    create_job_stub_ = scheduler_pb::create_job_service::NewStub(scheduler_channel_);
    update_job_stub_ = scheduler_pb::update_job_service::NewStub(scheduler_channel_);
    delete_job_stub_ = scheduler_pb::delete_job_service::NewStub(scheduler_channel_);
    pause_job_stub_ = scheduler_pb::pause_job_service::NewStub(scheduler_channel_);
    resume_job_stub_ = scheduler_pb::resume_job_service::NewStub(scheduler_channel_);
    trigger_job_stub_ = scheduler_pb::trigger_job_service::NewStub(scheduler_channel_);

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
    last_activity_time_ = std::chrono::steady_clock::now();

    // Start v2c worker threads
    poll_thread_ = std::thread(&SchedulerSyncBridge::PollLoop, this);

    if (config_.batch_window_ms > 0) {
        batch_thread_ = std::thread(&SchedulerSyncBridge::BatchLoop, this);
    }

    // Initialize c2v sync handling
    if (config_.enable_cloud_sync) {
        LOG(INFO) << "Enabling cloud sync handling (c2v)";

        // Subscribe to c2v content from Backend Transport
        // Cloud messages are processed synchronously - gRPC calls to Scheduler are fast
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
    if (batch_thread_.joinable()) {
        batch_thread_.join();
    }

    // Persist final state
    PersistState();

    // Cleanup
    transport_client_.reset();
    get_jobs_stub_.reset();
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
    LOG(INFO) << "Forcing full sync";
    auto jobs = QuerySchedulerJobs();
    SendV2SyncMessage(jobs, true /* include_all_jobs */);
}

uint64_t SchedulerSyncBridge::GetStateChecksum() const {
    return ComputeStateChecksumXxHash();
}

// =============================================================================
// Internal Methods
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

    // Initial full sync
    LOG(INFO) << "Initialization complete, performing initial full sync";
    auto jobs = QuerySchedulerJobs();
    SendV2SyncMessage(jobs, true /* include_all_jobs */);

    // Track ALL jobs in synced state (per spec: "All jobs: active AND tombstones")
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        synced_state_.clear();
        for (const auto& job : jobs) {
            synced_state_[job.job_id] = job;
        }
    }

    initialized_.store(true);
    LOG(INFO) << "Initial sync complete, " << jobs.size() << " jobs synced";

    // Main poll loop
    while (!stop_requested_.load()) {
        {
            std::unique_lock<std::mutex> lock(cv_mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(config_.poll_interval_ms),
                         [this]() { return stop_requested_.load(); });
        }

        if (stop_requested_.load()) break;

        VLOG(1) << "Polling Scheduler for changes...";

        // Query current state
        auto current = QuerySchedulerJobs();

        VLOG(1) << "Found " << current.size() << " jobs in Scheduler";

        // Detect changes
        DetectChanges(current);

        // Check for jobs that need sync and send delta sync
        {
            std::vector<SyncedJobState> jobs_to_sync;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                for (const auto& [job_id, state] : synced_state_) {
                    if (state.needs_sync) {
                        jobs_to_sync.push_back(state);
                    }
                }
            }
            if (!jobs_to_sync.empty()) {
                SendV2SyncMessage(jobs_to_sync, false /* delta sync */);

                // Mark jobs as synced
                std::lock_guard<std::mutex> lock(state_mutex_);
                for (const auto& job : jobs_to_sync) {
                    auto it = synced_state_.find(job.job_id);
                    if (it != synced_state_.end()) {
                        it->second.needs_sync = false;
                    }
                }
            }
        }

        // Send heartbeat if no activity
        MaybeSendHeartbeat();
    }

    LOG(INFO) << "Poll thread stopped";
}

void SchedulerSyncBridge::BatchLoop() {
    LOG(INFO) << "Batch thread started";

    while (!stop_requested_.load()) {
        {
            std::unique_lock<std::mutex> lock(cv_mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(config_.batch_window_ms),
                         [this]() { return stop_requested_.load(); });
        }

        if (stop_requested_.load()) break;

        // In v2, pending executions are batched and sent periodically
        std::lock_guard<std::mutex> lock(events_mutex_);
        if (!pending_executions_.empty()) {
            // Jobs that need sync will be included
            std::vector<SyncedJobState> jobs_to_sync;
            {
                std::lock_guard<std::mutex> state_lock(state_mutex_);
                for (const auto& [job_id, state] : synced_state_) {
                    if (state.needs_sync) {
                        jobs_to_sync.push_back(state);
                    }
                }
            }
            if (!jobs_to_sync.empty()) {
                SendV2SyncMessage(jobs_to_sync, false);
            }
        }
    }

    LOG(INFO) << "Batch thread stopped";
}

std::vector<SyncedJobState> SchedulerSyncBridge::QuerySchedulerJobs() {
    std::vector<SyncedJobState> result;

    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + 5s);

    scheduler_pb::get_jobs_request request;
    // Empty filter = return all jobs
    // Include completed jobs to detect terminal state transitions
    request.mutable_filter()->set_include_completed(true);

    scheduler_pb::get_jobs_response response;
    auto status = get_jobs_stub_->get_jobs(&context, request, &response);

    if (!status.ok()) {
        LOG(WARNING) << "Failed to query Scheduler: " << status.error_message();
        return result;
    }

    for (const auto& job : response.jobs()) {
        SyncedJobState state;
        state.job_id = job.id();
        state.title = job.title();
        state.service = job.service();
        state.method = job.method();
        state.parameters = job.parameters();
        state.scheduled_time_ms = job.scheduled_time_ms();
        state.scheduled_time = EpochMsToIso8601(job.scheduled_time_ms());  // For display/hash
        state.recurrence_rule = job.recurrence_rule();
        state.end_time_ms = job.end_time_ms();
        state.next_run_time = EpochMsToIso8601(job.next_run_time_ms());  // For display/hash
        state.status = MapStatus(job.status());
        state.wake_policy = MapWakePolicy(job.wake_policy());
        state.sleep_policy = MapSleepPolicy(job.sleep_policy());
        state.wake_lead_time_s = job.wake_lead_time_s();
        state.paused = job.paused();

        VLOG(1) << "FetchJobsFromScheduler: job=" << job.id()
                << " scheduled_time_ms=" << job.scheduled_time_ms()
                << " wake_policy=" << static_cast<int>(job.wake_policy())
                << " paused=" << (job.paused() ? "true" : "false");

        // Use actual timestamps from job
        state.created_at_ms = job.created_at_ms();
        state.updated_at_ms = job.updated_at_ms();

        result.push_back(std::move(state));
    }

    return result;
}

void SchedulerSyncBridge::DetectChanges(const std::vector<SyncedJobState>& current) {
    std::lock_guard<std::mutex> lock(state_mutex_);

    // Build map of current jobs
    std::unordered_map<std::string, const SyncedJobState*> current_map;
    for (const auto& job : current) {
        current_map[job.job_id] = &job;
    }

    // Check for deleted jobs (in synced_state_ but not in current)
    // Mark as tombstone (deleted=true) and keep in synced_state_ for sync
    for (auto& [job_id, synced] : synced_state_) {
        if (current_map.find(job_id) == current_map.end() && !synced.deleted) {
            // Job was deleted locally - create tombstone
            synced.deleted = true;
            synced.deleted_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            synced.version.increment_vehicle();  // Increment version for deletion
            synced.needs_sync = true;
            LOG(INFO) << "Job deleted (tombstone created): " << synced.title << " (id=" << job_id << ")";
        }
    }

    // Check for new or changed jobs
    for (const auto& job : current) {
        auto it = synced_state_.find(job.job_id);

        if (it == synced_state_.end()) {
            // New job - add to synced state and mark for sync
            SyncedJobState new_state = job;
            new_state.needs_sync = true;
            new_state.version.increment_vehicle();  // New local job
            new_state.authority = sync_v2::AUTHORITY_VEHICLE;
            synced_state_[job.job_id] = new_state;

            // If already in terminal state, also record execution
            if (job.IsTerminal()) {
                RecordExecution(
                    job.job_id,
                    job.updated_at_ms,
                    0,  // duration_ms
                    job.status,
                    "",  // result_json
                    ""   // error_message
                );
                LOG(INFO) << "Job executed: " << job.title << " (id=" << job.job_id
                          << ", status=" << static_cast<int>(job.status) << ")";
            } else {
                LOG(INFO) << "Job created: " << job.title << " (id=" << job.job_id << ")";
            }
        } else {
            // Existing job - check for changes
            auto& synced = it->second;

            // Check if job transitioned to terminal state
            bool was_terminal = synced.IsTerminal();
            bool is_terminal = job.IsTerminal();

            if (!was_terminal && is_terminal) {
                // Job transitioned to terminal state - record execution
                RecordExecution(
                    job.job_id,
                    job.updated_at_ms,
                    0,  // duration_ms
                    job.status,
                    "",  // result_json
                    ""   // error_message
                );
                LOG(INFO) << "Job completed: " << job.title << " (id=" << job.job_id
                          << ", status=" << static_cast<int>(job.status) << ")";

                // Update the synced state's status to prevent re-detecting this transition
                // on subsequent polls. Status is excluded from ComputeHash(), so we must
                // update it explicitly here.
                synced.status = job.status;
                synced.updated_at_ms = job.updated_at_ms;
                synced.needs_sync = true;
            }

            // Update synced state if job content changed (status is excluded from hash)
            if (job.ComputeHash() != synced.ComputeHash()) {
                SyncedJobState updated_state = job;
                updated_state.version = synced.version;
                updated_state.version.increment_vehicle();  // Local change
                updated_state.authority = synced.authority;
                updated_state.needs_sync = true;
                it->second = updated_state;

                VLOG(1) << "Job updated: " << job.title;
            }
        }
    }
}

void SchedulerSyncBridge::MaybeSendHeartbeat() {
    if (config_.heartbeat_interval_ms == 0) return;

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - last_activity_time_).count();

    if (elapsed >= config_.heartbeat_interval_ms) {
        // For v2 protocol, heartbeat is just an empty sync message
        // The presence of the message with timestamp indicates liveness
        std::vector<SyncedJobState> empty_jobs;
        SendV2SyncMessage(empty_jobs, false);
        last_activity_time_ = std::chrono::steady_clock::now();

        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.heartbeats_sent++;
    }
}

std::string SchedulerSyncBridge::GenerateInstanceId() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint64_t> dis;

    std::stringstream ss;
    ss << "ssb_" << std::hex << std::setw(16) << std::setfill('0') << dis(gen);
    return ss.str();
}

void SchedulerSyncBridge::LoadPersistedState() {
    if (config_.state_persistence_path.empty()) return;

    std::ifstream file(config_.state_persistence_path, std::ios::binary);
    if (!file.is_open()) {
        LOG(INFO) << "No persisted state found at " << config_.state_persistence_path;
        return;
    }

    // Read sequence number
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

    // Write sequence number
    uint64_t seq = sequence_number_.load();
    file.write(reinterpret_cast<const char*>(&seq), sizeof(seq));

    file.close();
    LOG(INFO) << "Persisted sync state (sequence=" << seq << ")";
}

void SchedulerSyncBridge::UpdateStats(uint64_t bytes_sent, bool is_full_sync,
                                       bool is_execution_result) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.events_sent++;
    stats_.bytes_sent += bytes_sent;
    stats_.last_sync_timestamp_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

    if (is_full_sync) {
        stats_.full_syncs_sent++;
    } else if (is_execution_result) {
        stats_.execution_results_sent++;
    } else {
        stats_.delta_syncs_sent++;
    }
}

sync_v2::JobStatus SchedulerSyncBridge::MapStatus(
    scheduler_types_pb::job_status_t status) {
    // Map IFEX scheduler status to sync v2 status
    // Note: IFEX scheduler no longer has PAUSED status - it uses paused boolean
    switch (status) {
        case scheduler_types_pb::JOB_STATUS_PENDING:
            return sync_v2::JOB_STATUS_PENDING;
        case scheduler_types_pb::JOB_STATUS_RUNNING:
            return sync_v2::JOB_STATUS_RUNNING;
        case scheduler_types_pb::JOB_STATUS_COMPLETED:
            return sync_v2::JOB_STATUS_COMPLETED;
        case scheduler_types_pb::JOB_STATUS_FAILED:
            return sync_v2::JOB_STATUS_FAILED;
        case scheduler_types_pb::JOB_STATUS_CANCELLED:
            return sync_v2::JOB_STATUS_CANCELLED;
    }
    return sync_v2::JOB_STATUS_PENDING;  // Default
}

sync_v2::WakePolicy SchedulerSyncBridge::MapWakePolicy(
    scheduler_types_pb::wake_policy_t policy) {
    switch (policy) {
        case scheduler_types_pb::WAKE_REQUIRED:
            return sync_v2::WAKE_REQUIRED;
        case scheduler_types_pb::WAKE_NO_WAKE:
        default:
            return sync_v2::WAKE_NO_WAKE;
    }
}

sync_v2::SleepPolicy SchedulerSyncBridge::MapSleepPolicy(
    scheduler_types_pb::sleep_policy_t policy) {
    switch (policy) {
        case scheduler_types_pb::SLEEP_INHIBIT:
            return sync_v2::SLEEP_INHIBIT;
        case scheduler_types_pb::SLEEP_NORMAL:
        default:
            return sync_v2::SLEEP_NORMAL;
    }
}

// =============================================================================
// Cloud Sync Handling (c2v) - Pure State Sync Model
// =============================================================================

void SchedulerSyncBridge::HandleCloudMessage(const std::vector<uint8_t>& payload) {
    // Try parsing as different message types in priority order

    // 1. Try C2V_SyncMessage (main state sync)
    sync_v2::C2V_SyncMessage sync_msg;
    if (sync_msg.ParseFromArray(payload.data(), static_cast<int>(payload.size())) &&
        !sync_msg.vehicle_id().empty()) {
        LOG(INFO) << "Received C2V sync message: vehicle_id=" << sync_msg.vehicle_id()
                  << ", jobs=" << sync_msg.jobs_size()
                  << ", checksum=" << sync_msg.state_checksum();
        HandleV2SyncMessage(sync_msg);
        return;
    }

    // 2. Try TriggerJobRequest (the only imperative command)
    sync_v2::TriggerJobRequest trigger_req;
    if (trigger_req.ParseFromArray(payload.data(), static_cast<int>(payload.size())) &&
        !trigger_req.job_id().empty()) {
        LOG(INFO) << "Received TriggerJobRequest: job=" << trigger_req.job_id()
                  << ", requester=" << trigger_req.requester_id();
        HandleTriggerJobRequest(trigger_req);
        return;
    }

    LOG(WARNING) << "Failed to parse c2v payload as any known message type ("
                 << payload.size() << " bytes)";
}

SchedulerSyncBridge::OperationResult SchedulerSyncBridge::CreateJobFromCloud(
    const sync_v2::JobRecord& job) {

    OperationResult result;
    result.job_id = job.job_id();

    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() +
                         std::chrono::milliseconds(config_.operation_timeout_ms));

    scheduler_pb::create_job_request request;
    auto* new_job = request.mutable_job();

    // Pass the cloud-provided job_id so vehicle uses the same ID
    new_job->set_job_id(job.job_id());
    new_job->set_title(job.title());
    new_job->set_service(job.service());
    new_job->set_method(job.method());
    new_job->set_parameters(job.parameters_json());
    new_job->set_scheduled_time_ms(job.scheduled_time_ms());
    new_job->set_recurrence_rule(job.recurrence_rule());
    new_job->set_end_time_ms(job.end_time_ms());

    // Map wake/sleep policies
    new_job->set_wake_policy(job.wake_policy() == sync_v2::WAKE_REQUIRED
        ? scheduler_types_pb::WAKE_REQUIRED : scheduler_types_pb::WAKE_NO_WAKE);
    new_job->set_sleep_policy(job.sleep_policy() == sync_v2::SLEEP_INHIBIT
        ? scheduler_types_pb::SLEEP_INHIBIT : scheduler_types_pb::SLEEP_NORMAL);
    new_job->set_wake_lead_time_s(job.wake_lead_time_s());

    // Set paused state from the paused boolean field
    new_job->set_paused(job.paused());

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
    const sync_v2::JobRecord& job) {

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

    // Set update fields via the updates sub-message
    auto* updates = request.mutable_updates();
    updates->set_title(job.title());
    updates->set_scheduled_time_ms(job.scheduled_time_ms());
    updates->set_recurrence_rule(job.recurrence_rule());
    updates->set_parameters(job.parameters_json());
    updates->set_end_time_ms(job.end_time_ms());

    // Set paused state in the update
    updates->set_paused(job.paused());

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

void SchedulerSyncBridge::HandleTriggerJobRequest(
    const sync_v2::TriggerJobRequest& request) {

    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.trigger_requests_received++;
    }

    const std::string& job_id = request.job_id();

    // Check for request expiry
    if (request.expires_at_ms() > 0) {
        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        if (static_cast<uint64_t>(now_ms) > request.expires_at_ms()) {
            LOG(WARNING) << "TriggerJobRequest expired for job " << job_id;
            SendTriggerJobResponse(job_id, false, "Request expired");
            return;
        }
    }

    // Execute trigger via Scheduler gRPC
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() +
                         std::chrono::milliseconds(config_.operation_timeout_ms));

    scheduler_pb::trigger_job_request grpc_request;
    grpc_request.set_job_id(job_id);

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

    LOG(INFO) << "TriggerJobRequest for " << job_id << ": "
              << (success ? "ACCEPTED" : "REJECTED")
              << (error_message.empty() ? "" : " - " + error_message);

    SendTriggerJobResponse(job_id, success, error_message);
}

void SchedulerSyncBridge::SendTriggerJobResponse(const std::string& job_id,
                                                  bool accepted,
                                                  const std::string& error_message) {
    sync_v2::TriggerJobResponse response;
    response.set_job_id(job_id);
    response.set_accepted(accepted);
    if (!error_message.empty()) {
        response.set_error_message(error_message);
    }
    response.set_timestamp_ms(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    std::string serialized;
    if (!response.SerializeToString(&serialized)) {
        LOG(ERROR) << "Failed to serialize TriggerJobResponse";
        return;
    }

    std::vector<uint8_t> payload(serialized.begin(), serialized.end());
    auto result = transport_client_->publish(payload, client::Persistence::BestEffort);

    if (result.ok()) {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.bytes_sent += serialized.size();
        VLOG(1) << "Sent TriggerJobResponse for " << job_id;
    } else {
        LOG(WARNING) << "Failed to send TriggerJobResponse for " << job_id
                     << ": status=" << static_cast<int>(result.status);
    }

    last_activity_time_ = std::chrono::steady_clock::now();
}

// =============================================================================
// Sync Protocol v2 Methods
// =============================================================================

void SyncedJobState::ToJobRecord(sync_v2::JobRecord* record) const {
    record->set_job_id(job_id);
    record->set_authority(authority);

    auto* ver = record->mutable_version();
    ver->set_cloud_seq(version.cloud_seq);
    ver->set_vehicle_seq(version.vehicle_seq);

    record->set_deleted(deleted);
    if (deleted_at_ms > 0) {
        record->set_deleted_at_ms(deleted_at_ms);
    }

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

    // Set status
    record->set_status(status);

    // Set wake/sleep policies
    record->set_wake_policy(wake_policy);
    record->set_sleep_policy(sleep_policy);
    record->set_wake_lead_time_s(wake_lead_time_s);

    record->set_created_at_ms(created_at_ms);
    record->set_updated_at_ms(updated_at_ms);
}

SyncedJobState SyncedJobState::FromJobRecord(const sync_v2::JobRecord& record) {
    SyncedJobState state;
    state.job_id = record.job_id();
    state.authority = record.authority();
    state.version.cloud_seq = record.version().cloud_seq();
    state.version.vehicle_seq = record.version().vehicle_seq();
    state.deleted = record.deleted();
    state.deleted_at_ms = record.deleted_at_ms();

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

    // Copy status directly
    state.status = record.status();

    // Copy wake/sleep policies directly
    state.wake_policy = record.wake_policy();
    state.sleep_policy = record.sleep_policy();
    state.wake_lead_time_s = record.wake_lead_time_s();

    state.created_at_ms = record.created_at_ms();
    state.updated_at_ms = record.updated_at_ms();

    return state;
}

void SchedulerSyncBridge::SendV2SyncMessage(const std::vector<SyncedJobState>& jobs,
                                            bool include_all_jobs) {
    sync_v2::V2C_SyncMessage msg;
    msg.set_vehicle_id(config_.vehicle_id);
    msg.set_bridge_instance_id(instance_id_);

    // Add job records (include jobs that need sync in delta syncs)
    for (const auto& job : jobs) {
        if (include_all_jobs || job.needs_sync) {
            auto* record = msg.add_jobs();
            job.ToJobRecord(record);
        }
    }

    // Add any pending execution records
    {
        std::lock_guard<std::mutex> lock(events_mutex_);
        for (const auto& exec : pending_executions_) {
            *msg.add_executions() = exec;
        }
        pending_executions_.clear();
    }

    msg.set_sync_timestamp_ms(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    // Set checksum for quiescence detection
    msg.set_state_checksum(ComputeStateChecksumXxHash());

    std::string serialized;
    if (!msg.SerializeToString(&serialized)) {
        LOG(ERROR) << "Failed to serialize V2C_SyncMessage";
        return;
    }

    std::vector<uint8_t> payload(serialized.begin(), serialized.end());
    auto result = transport_client_->publish(payload, client::Persistence::Volatile);

    if (result.ok()) {
        // Increment sequence number for stats tracking
        ++sequence_number_;

        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.events_sent++;
        stats_.bytes_sent += serialized.size();
        if (include_all_jobs) {
            stats_.full_syncs_sent++;
        } else {
            stats_.delta_syncs_sent++;
        }
        VLOG(1) << "Sent V2C_SyncMessage with " << msg.jobs_size() << " jobs, "
                << msg.executions_size() << " executions";
    } else {
        LOG(WARNING) << "Failed to send V2C_SyncMessage: status="
                     << static_cast<int>(result.status);
    }

    last_activity_time_ = std::chrono::steady_clock::now();
}

void SchedulerSyncBridge::SendDeleteSyncMessage(const std::string& job_id) {
    sync_v2::V2C_SyncMessage msg;
    msg.set_vehicle_id(config_.vehicle_id);
    msg.set_bridge_instance_id(instance_id_);

    // Add a tombstone job record with deleted=true
    auto* record = msg.add_jobs();
    record->set_job_id(job_id);
    record->set_deleted(true);
    record->set_deleted_at_ms(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    msg.set_sync_timestamp_ms(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    msg.set_state_checksum(ComputeStateChecksumXxHash());

    std::string serialized;
    if (!msg.SerializeToString(&serialized)) {
        LOG(ERROR) << "Failed to serialize delete V2C_SyncMessage";
        return;
    }

    std::vector<uint8_t> payload(serialized.begin(), serialized.end());
    auto result = transport_client_->publish(payload, client::Persistence::Volatile);

    if (result.ok()) {
        ++sequence_number_;

        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.events_sent++;
        stats_.bytes_sent += serialized.size();
        stats_.delta_syncs_sent++;
        LOG(INFO) << "Sent delete sync for job " << job_id
                  << " with vehicle_id=" << config_.vehicle_id;
    } else {
        LOG(WARNING) << "Failed to send delete sync for job " << job_id
                     << ": status=" << static_cast<int>(result.status);
    }

    last_activity_time_ = std::chrono::steady_clock::now();
}

void SchedulerSyncBridge::HandleV2SyncMessage(const sync_v2::C2V_SyncMessage& msg) {
    LOG(INFO) << "Received C2V_SyncMessage from cloud: vehicle_id=" << msg.vehicle_id()
              << ", jobs=" << msg.jobs_size()
              << ", checksum=" << msg.state_checksum();

    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.syncs_received++;
    }

    // Process each job from cloud (including tombstones with deleted=true)
    for (const auto& remote_job : msg.jobs()) {
        ProcessCloudJob(remote_job);
    }
}

void SchedulerSyncBridge::ProcessCloudJob(const sync_v2::JobRecord& remote_job) {
    const std::string& job_id = remote_job.job_id();

    // Get local version if exists
    std::optional<sched_lib::VersionVector> local_version;
    sync_v2::JobAuthority authority = remote_job.authority();

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = synced_state_.find(job_id);
        if (it != synced_state_.end()) {
            local_version = it->second.version;
        }
    }

    // Use sync engine to determine action
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
            VLOG(1) << "Job " << job_id << ": no action (already in sync)";
            break;

        case sched_lib::SyncResult::ACCEPT_REMOTE:
            VLOG(1) << "Job " << job_id << ": accepting remote version";
            ApplyCloudJob(remote_job);
            break;

        case sched_lib::SyncResult::REJECT_REMOTE:
            VLOG(1) << "Job " << job_id << ": rejecting remote (local dominates)";
            // Our local version is newer - will be synced on next outbound
            break;

        case sched_lib::SyncResult::CONFLICT_RESOLVED:
            LOG(INFO) << "Job " << job_id << ": conflict resolved, winner=" << result.winner;
            if (result.winner == "cloud") {
                ApplyCloudJob(remote_job);
            }
            // Update version to merged version
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                auto it = synced_state_.find(job_id);
                if (it != synced_state_.end()) {
                    it->second.version = result.resolved_version;
                    it->second.needs_sync = false;
                }
            }
            break;
    }
}

uint64_t SchedulerSyncBridge::ComputeStateChecksumXxHash() const {
    std::lock_guard<std::mutex> lock(state_mutex_);

    // Use centralized checksum computation from ifex-scheduler library
    // Jobs must be sorted by job_id for deterministic results
    std::vector<sched_lib::Job> jobs;
    jobs.reserve(synced_state_.size());

    for (const auto& [id, state] : synced_state_) {
        jobs.push_back(ToLibraryJob(state));
    }

    // Sort by job_id for deterministic ordering (required by library)
    std::sort(jobs.begin(), jobs.end(),
              [](const sched_lib::Job& a, const sched_lib::Job& b) {
                  return a.job_id < b.job_id;
              });

    return sched_lib::compute_state_checksum(jobs);
}

void SchedulerSyncBridge::RecordExecution(const std::string& job_id,
                                         uint64_t executed_at_ms,
                                         uint64_t duration_ms,
                                         sync_v2::JobStatus status,
                                         const std::string& result_json,
                                         const std::string& error_message) {
    sync_v2::ExecutionRecord exec;
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
        std::lock_guard<std::mutex> lock(events_mutex_);
        pending_executions_.push_back(std::move(exec));
    }

    VLOG(1) << "Recorded execution for job " << job_id;
}

std::string SchedulerSyncBridge::GenerateSyncId() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<uint64_t> dist;

    std::ostringstream oss;
    oss << "sync-" << std::hex << dist(gen);
    return oss.str();
}

std::string SchedulerSyncBridge::GenerateExecutionId() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<uint64_t> dist;

    std::ostringstream oss;
    oss << "exec-" << std::hex << dist(gen);
    return oss.str();
}

// Apply a cloud job to local Scheduler and synced state
void SchedulerSyncBridge::ApplyCloudJob(const sync_v2::JobRecord& remote_job) {
    const std::string& job_id = remote_job.job_id();

    // Check if job exists locally
    bool exists_locally = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        exists_locally = (synced_state_.find(job_id) != synced_state_.end());
    }

    // Handle deleted jobs - keep tombstone until both sides confirm
    if (remote_job.deleted()) {
        auto result = DeleteJobFromScheduler(job_id);
        if (result.success || !exists_locally) {
            // Keep tombstone in synced_state_ for sync back to cloud
            // Cloud will delete from DB when it receives our V2C tombstone
            std::lock_guard<std::mutex> lock(state_mutex_);
            SyncedJobState tombstone;
            tombstone.job_id = job_id;
            tombstone.deleted = true;
            tombstone.deleted_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            tombstone.version.cloud_seq = remote_job.version().cloud_seq();
            tombstone.version.vehicle_seq = remote_job.version().vehicle_seq();
            tombstone.needs_sync = true;  // Will be sent in next V2C sync
            synced_state_[job_id] = tombstone;
        }
        LOG(INFO) << "Applied cloud deletion for job " << job_id << ", tombstone queued for sync";
        return;
    }

    // Create or update job in Scheduler
    OperationResult result;
    if (exists_locally) {
        result = UpdateJobFromCloud(remote_job);
    } else {
        result = CreateJobFromCloud(remote_job);
    }

    if (result.success) {
        // Update local synced state
        std::lock_guard<std::mutex> lock(state_mutex_);
        synced_state_[job_id] = SyncedJobState::FromJobRecord(remote_job);
        synced_state_[job_id].needs_sync = true;  // Confirm receipt in next sync
        // Mark as cloud authority since it came from cloud
        synced_state_[job_id].authority = sync_v2::AUTHORITY_CLOUD;

        LOG(INFO) << "Applied cloud job " << job_id << " (" << remote_job.title() << ")";
    } else {
        LOG(WARNING) << "Failed to apply cloud job " << job_id << ": " << result.error_message;

        std::lock_guard<std::mutex> stats_lock(stats_mutex_);
        stats_.jobs_rejected++;
    }
}

}  // namespace ifex::reference
