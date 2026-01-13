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
namespace cmd_pb = swdv::scheduler_command_envelope;
namespace scheduler_pb = swdv::ifex_scheduler;

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
// SyncedJobState
// =============================================================================

uint64_t SyncedJobState::ComputeHash() const {
    // Hash only content fields - exclude metadata like updated_at_ms
    // which can change without actual job content changing
    std::hash<std::string> str_hash;
    std::hash<int> int_hash;

    uint64_t h = str_hash(job_id);
    h ^= str_hash(title) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= str_hash(service) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= str_hash(method) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= str_hash(parameters) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= str_hash(scheduled_time) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= str_hash(recurrence_rule) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= str_hash(next_run_time) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= int_hash(static_cast<int>(status)) + 0x9e3779b9 + (h << 6) + (h >> 2);
    // Note: updated_at_ms excluded - it's metadata, not content

    return h;
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
    LOG(INFO) << "  Terminal states only: " << (config_.terminal_states_only ? "yes" : "no");

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

    // Initialize c2v command handling
    if (config_.enable_cloud_commands) {
        LOG(INFO) << "Enabling cloud command handling (c2v)";

        // Subscribe to c2v content from Backend Transport
        // Commands are processed synchronously - gRPC calls to Scheduler are fast
        transport_client_->on_content(
            [this](const std::vector<uint8_t>& payload) {
                HandleCloudCommand(payload);
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

uint32_t SchedulerSyncBridge::GetStateChecksum() const {
    return ComputeStateChecksum();
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

    // Update synced state with active jobs only
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        synced_state_.clear();
        synced_terminal_jobs_.clear();
        for (const auto& job : jobs) {
            if (!job.IsTerminal()) {
                synced_state_[job.job_id] = job;
            } else {
                // Mark terminal jobs as already synced
                synced_terminal_jobs_.insert(job.job_id);
            }
        }
    }

    initialized_.store(true);
    LOG(INFO) << "Initial sync complete, " << jobs.size() << " jobs synced, "
              << synced_state_.size() << " active jobs tracked";

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

        // Check for jobs with pending sync state and send delta sync
        {
            std::vector<SyncedJobState> jobs_to_sync;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                for (const auto& [job_id, state] : synced_state_) {
                    if (state.sync_state == sync_v2::SYNC_STATE_PENDING) {
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
                        it->second.sync_state = sync_v2::SYNC_STATE_SYNCED;
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
            // Jobs with pending changes will be synced
            std::vector<SyncedJobState> jobs_to_sync;
            {
                std::lock_guard<std::mutex> state_lock(state_mutex_);
                for (const auto& [job_id, state] : synced_state_) {
                    if (state.sync_state == sync_v2::SYNC_STATE_PENDING) {
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
        state.next_run_time = EpochMsToIso8601(job.next_run_time_ms());  // For display/hash
        state.status = MapStatus(job.status());
        state.wake_policy = MapWakePolicy(job.wake_policy());
        state.sleep_policy = MapSleepPolicy(job.sleep_policy());
        state.wake_lead_time_s = job.wake_lead_time_s();

        VLOG(1) << "FetchJobsFromScheduler: job=" << job.id()
                << " scheduled_time_ms=" << job.scheduled_time_ms()
                << " wake_policy=" << static_cast<int>(job.wake_policy());

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

    namespace sync_v2_ns = swdv::scheduler_sync_v2;

    // Check for deleted jobs (in synced_state_ but not in current)
    std::vector<std::string> to_remove;
    for (const auto& [job_id, synced] : synced_state_) {
        if (current_map.find(job_id) == current_map.end()) {
            // Job was deleted - mark as deleted with pending sync
            // The job will be included in next sync with deleted=true
            to_remove.push_back(job_id);
            LOG(INFO) << "Job deleted: " << synced.title << " (id=" << job_id << ")";
        }
    }

    for (const auto& job_id : to_remove) {
        synced_state_.erase(job_id);
    }

    // Check for new or changed jobs
    for (const auto& job : current) {
        // Skip jobs already synced in terminal state
        if (synced_terminal_jobs_.count(job.job_id) > 0) {
            continue;
        }

        auto it = synced_state_.find(job.job_id);

        if (it == synced_state_.end()) {
            // New job or job transitioning to terminal state
            if (job.IsTerminal()) {
                // Job completed/failed - record execution for v2 sync
                RecordExecution(
                    job.job_id,
                    job.updated_at_ms,
                    0,  // duration_ms - would calculate from actual execution
                    job.status == sync_v2::JOB_STATUS_COMPLETED
                        ? sync_v2_ns::JOB_STATUS_COMPLETED
                        : sync_v2_ns::JOB_STATUS_FAILED,
                    "",  // result_json
                    ""   // error_message
                );

                synced_terminal_jobs_.insert(job.job_id);
                LOG(INFO) << "Job executed: " << job.title << " (id=" << job.job_id
                          << ", status=" << static_cast<int>(job.status) << ")";
            } else {
                // New active job - add to synced state with PENDING sync state
                SyncedJobState new_state = job;
                new_state.sync_state = sync_v2_ns::SYNC_STATE_PENDING;
                new_state.version.increment_vehicle();  // New local job
                new_state.authority = sync_v2_ns::AUTHORITY_VEHICLE;
                synced_state_[job.job_id] = new_state;

                LOG(INFO) << "Job created: " << job.title << " (id=" << job.job_id << ")";
            }
        } else {
            // Existing job - check for changes
            const auto& synced = it->second;

            if (job.IsTerminal()) {
                // Job transitioned to terminal state - record execution
                RecordExecution(
                    job.job_id,
                    job.updated_at_ms,
                    0,  // duration_ms
                    job.status == sync_v2::JOB_STATUS_COMPLETED
                        ? sync_v2_ns::JOB_STATUS_COMPLETED
                        : sync_v2_ns::JOB_STATUS_FAILED,
                    "",  // result_json
                    ""   // error_message
                );

                // Remove from active tracking, add to terminal set
                synced_state_.erase(job.job_id);
                synced_terminal_jobs_.insert(job.job_id);

                LOG(INFO) << "Job completed: " << job.title << " (id=" << job.job_id
                          << ", status=" << static_cast<int>(job.status) << ")";
            } else if (job.ComputeHash() != synced.ComputeHash()) {
                // Job was updated (but still active)
                bool should_sync = true;

                // If terminal_states_only, skip RUNNING state updates
                if (config_.terminal_states_only &&
                    job.status == sync_v2::JOB_STATUS_RUNNING &&
                    synced.status == sync_v2::JOB_STATUS_PENDING) {
                    should_sync = false;
                    VLOG(1) << "Skipping RUNNING state update for: " << job.title;
                }

                if (should_sync) {
                    // Update synced state with new values and mark as pending
                    SyncedJobState updated_state = job;
                    updated_state.version = synced.version;
                    updated_state.version.increment_vehicle();  // Local change
                    updated_state.authority = synced.authority;
                    updated_state.sync_state = sync_v2_ns::SYNC_STATE_PENDING;
                    it->second = updated_state;

                    VLOG(1) << "Job updated: " << job.title;
                } else {
                    it->second = job;
                    it->second.sync_state = synced.sync_state;  // Preserve sync state
                }
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

uint32_t SchedulerSyncBridge::ComputeStateChecksum() const {
    std::lock_guard<std::mutex> lock(state_mutex_);

    // Simple CRC32-like checksum
    uint32_t crc = 0xFFFFFFFF;

    // Sort by job_id for deterministic ordering
    std::vector<std::string> sorted_ids;
    for (const auto& [id, _] : synced_state_) {
        sorted_ids.push_back(id);
    }
    std::sort(sorted_ids.begin(), sorted_ids.end());

    for (const auto& id : sorted_ids) {
        const auto& state = synced_state_.at(id);
        uint64_t hash = state.ComputeHash();

        // Mix hash into CRC
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
    scheduler_pb::job_status_t status) {
    switch (status) {
        case scheduler_pb::PENDING:
            return sync_v2::JOB_STATUS_PENDING;
        case scheduler_pb::RUNNING:
            return sync_v2::JOB_STATUS_RUNNING;
        case scheduler_pb::COMPLETED:
            return sync_v2::JOB_STATUS_COMPLETED;
        case scheduler_pb::FAILED:
            return sync_v2::JOB_STATUS_FAILED;
        case scheduler_pb::CANCELLED:
            return sync_v2::JOB_STATUS_CANCELLED;
        default:
            return sync_v2::JOB_STATUS_PENDING;
    }
}

sync_v2::WakePolicy SchedulerSyncBridge::MapWakePolicy(
    scheduler_pb::wake_policy_t policy) {
    switch (policy) {
        case scheduler_pb::WAKE_REQUIRED:
            return sync_v2::WAKE_POLICY_WAKE_REQUIRED;
        case scheduler_pb::NO_WAKE:
        default:
            return sync_v2::WAKE_POLICY_NO_WAKE;
    }
}

sync_v2::SleepPolicy SchedulerSyncBridge::MapSleepPolicy(
    scheduler_pb::sleep_policy_t policy) {
    switch (policy) {
        case scheduler_pb::INHIBIT_UNTIL_COMPLETE:
            return sync_v2::SLEEP_POLICY_INHIBIT_UNTIL_COMPLETE;
        case scheduler_pb::SLEEP_NORMAL:
        default:
            return sync_v2::SLEEP_POLICY_NORMAL;
    }
}

// =============================================================================
// Cloud Command Handling (c2v)
// =============================================================================

void SchedulerSyncBridge::HandleCloudCommand(const std::vector<uint8_t>& payload) {
    // Try parsing as different message types
    // Order: command envelope first (most common), then v2 sync messages

    // Try command envelope
    cmd_pb::scheduler_command_t command;
    if (command.ParseFromArray(payload.data(), static_cast<int>(payload.size())) &&
        command.type() != cmd_pb::COMMAND_UNKNOWN) {
        LOG(INFO) << "Received cloud command: id=" << command.command_id()
                  << ", type=" << static_cast<int>(command.type());

        // Update stats
        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.commands_received++;
        }

        // Process command synchronously
        ProcessCommand(command);
        return;
    }

    // Try v2 C2V_SyncMessage
    namespace sync_v2_local = swdv::scheduler_sync_v2;
    sync_v2_local::C2V_SyncMessage sync_msg;
    if (sync_msg.ParseFromArray(payload.data(), static_cast<int>(payload.size())) &&
        !sync_msg.sync_id().empty()) {
        LOG(INFO) << "Received C2V sync message: sync_id=" << sync_msg.sync_id()
                  << ", jobs=" << sync_msg.jobs_size();
        HandleV2SyncMessage(sync_msg);
        return;
    }

    // Try v2 SyncAck
    sync_v2_local::SyncAck ack;
    if (ack.ParseFromArray(payload.data(), static_cast<int>(payload.size())) &&
        !ack.sync_id().empty()) {
        VLOG(1) << "Received sync ack: sync_id=" << ack.sync_id()
                << ", success=" << ack.success();
        // Sync ack is just informational for now - we don't track pending syncs
        return;
    }

    LOG(WARNING) << "Failed to parse c2v payload as any known message type ("
                 << payload.size() << " bytes)";
}

void SchedulerSyncBridge::ProcessCommand(
    const cmd_pb::scheduler_command_t& command) {

    CommandResult result;
    std::string command_type_str;

    switch (command.type()) {
        case cmd_pb::COMMAND_CREATE_JOB: {
            command_type_str = "CREATE_JOB";
            if (command.has_create_job()) {
                result = ExecuteCreateJob(command.create_job());
                std::lock_guard<std::mutex> lock(stats_mutex_);
                stats_.commands_create++;
            } else {
                result.success = false;
                result.error_message = "Missing create_job payload";
            }
            break;
        }

        case cmd_pb::COMMAND_UPDATE_JOB: {
            command_type_str = "UPDATE_JOB";
            if (command.has_update_job()) {
                result = ExecuteUpdateJob(command.update_job());
                std::lock_guard<std::mutex> lock(stats_mutex_);
                stats_.commands_update++;
            } else {
                result.success = false;
                result.error_message = "Missing update_job payload";
            }
            break;
        }

        case cmd_pb::COMMAND_DELETE_JOB: {
            command_type_str = "DELETE_JOB";
            result = ExecuteDeleteJob(command.delete_job_id());
            {
                std::lock_guard<std::mutex> lock(stats_mutex_);
                stats_.commands_delete++;
            }
            // Send proper V2C_SyncMessage with deleted_job_ids after successful delete
            if (result.success) {
                SendDeleteSyncMessage(command.delete_job_id());
            }
            break;
        }

        case cmd_pb::COMMAND_PAUSE_JOB: {
            command_type_str = "PAUSE_JOB";
            result = ExecutePauseJob(command.pause_job_id());
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.commands_pause++;
            break;
        }

        case cmd_pb::COMMAND_RESUME_JOB: {
            command_type_str = "RESUME_JOB";
            result = ExecuteResumeJob(command.resume_job_id());
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.commands_resume++;
            break;
        }

        case cmd_pb::COMMAND_TRIGGER_JOB: {
            command_type_str = "TRIGGER_JOB";
            result = ExecuteTriggerJob(command.trigger_job_id());
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.commands_trigger++;
            break;
        }

        default:
            command_type_str = "UNKNOWN";
            result.success = false;
            result.error_message = "Unknown command type";
            break;
    }

    // Update success/failure stats
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        if (result.success) {
            stats_.commands_succeeded++;
        } else {
            stats_.commands_failed++;
        }
    }

    LOG(INFO) << "Command " << command.command_id() << " (" << command_type_str << "): "
              << (result.success ? "SUCCESS" : "FAILED")
              << (result.error_message.empty() ? "" : " - " + result.error_message);

    // Send acknowledgment
    if (config_.send_command_acks) {
        SendCommandAck(command.command_id(), result.success,
                       result.error_message, result.job_id);
    }
}

SchedulerSyncBridge::CommandResult SchedulerSyncBridge::ExecuteCreateJob(
    const cmd_pb::job_definition_t& def) {

    CommandResult result;

    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() +
                         std::chrono::milliseconds(config_.command_timeout_ms));

    scheduler_pb::create_job_request request;
    auto* job = request.mutable_job();

    // Map job definition to scheduler job_create_t
    // Pass the cloud-provided job_id so vehicle uses the same ID
    if (!def.job_id().empty()) {
        job->set_job_id(def.job_id());
    }
    job->set_title(def.title());
    job->set_service(def.service());
    job->set_method(def.method());
    job->set_parameters(def.parameters_json());
    job->set_scheduled_time_ms(def.scheduled_time_ms());
    job->set_recurrence_rule(def.recurrence_rule());
    job->set_end_time_ms(def.end_time_ms());

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

    LOG(INFO) << "Created job: " << def.title() << " (id=" << result.job_id << ")";
    return result;
}

SchedulerSyncBridge::CommandResult SchedulerSyncBridge::ExecuteUpdateJob(
    const cmd_pb::job_update_t& update) {

    CommandResult result;
    result.job_id = update.job_id();

    if (update.job_id().empty()) {
        result.success = false;
        result.error_message = "job_id is required for update";
        return result;
    }

    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() +
                         std::chrono::milliseconds(config_.command_timeout_ms));

    scheduler_pb::update_job_request request;
    request.set_job_id(update.job_id());

    // Set update fields via the updates sub-message
    auto* updates = request.mutable_updates();
    updates->set_title(update.title());
    updates->set_scheduled_time_ms(update.scheduled_time_ms());
    updates->set_recurrence_rule(update.recurrence_rule());
    updates->set_parameters(update.parameters_json());
    updates->set_end_time_ms(update.end_time_ms());

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
    LOG(INFO) << "Updated job: " << update.job_id();
    return result;
}

SchedulerSyncBridge::CommandResult SchedulerSyncBridge::ExecuteDeleteJob(
    const std::string& job_id) {

    CommandResult result;
    result.job_id = job_id;

    if (job_id.empty()) {
        result.success = false;
        result.error_message = "job_id is required for delete";
        return result;
    }

    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() +
                         std::chrono::milliseconds(config_.command_timeout_ms));

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
    LOG(INFO) << "Deleted job: " << job_id;
    return result;
}

SchedulerSyncBridge::CommandResult SchedulerSyncBridge::ExecutePauseJob(
    const std::string& job_id) {

    CommandResult result;
    result.job_id = job_id;

    if (job_id.empty()) {
        result.success = false;
        result.error_message = "job_id is required for pause";
        return result;
    }

    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() +
                         std::chrono::milliseconds(config_.command_timeout_ms));

    scheduler_pb::pause_job_request request;
    request.set_job_id(job_id);

    scheduler_pb::pause_job_response response;
    auto status = pause_job_stub_->pause_job(&context, request, &response);

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
    LOG(INFO) << "Paused job: " << job_id;
    return result;
}

SchedulerSyncBridge::CommandResult SchedulerSyncBridge::ExecuteResumeJob(
    const std::string& job_id) {

    CommandResult result;
    result.job_id = job_id;

    if (job_id.empty()) {
        result.success = false;
        result.error_message = "job_id is required for resume";
        return result;
    }

    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() +
                         std::chrono::milliseconds(config_.command_timeout_ms));

    scheduler_pb::resume_job_request request;
    request.set_job_id(job_id);

    scheduler_pb::resume_job_response response;
    auto status = resume_job_stub_->resume_job(&context, request, &response);

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
    LOG(INFO) << "Resumed job: " << job_id;
    return result;
}

SchedulerSyncBridge::CommandResult SchedulerSyncBridge::ExecuteTriggerJob(
    const std::string& job_id) {

    CommandResult result;
    result.job_id = job_id;

    if (job_id.empty()) {
        result.success = false;
        result.error_message = "job_id is required for trigger";
        return result;
    }

    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() +
                         std::chrono::milliseconds(config_.command_timeout_ms));

    scheduler_pb::trigger_job_request request;
    request.set_job_id(job_id);

    scheduler_pb::trigger_job_response response;
    auto status = trigger_job_stub_->trigger_job(&context, request, &response);

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
    LOG(INFO) << "Triggered job: " << job_id;
    return result;
}

void SchedulerSyncBridge::SendCommandAck(const std::string& command_id,
                                          bool success,
                                          const std::string& error_message,
                                          const std::string& job_id) {
    cmd_pb::scheduler_command_ack_t ack;
    ack.set_command_id(command_id);
    ack.set_success(success);
    if (!error_message.empty()) {
        ack.set_error_message(error_message);
    }
    if (!job_id.empty()) {
        ack.set_job_id(job_id);
    }
    ack.set_timestamp_ms(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    std::string serialized;
    if (!ack.SerializeToString(&serialized)) {
        LOG(ERROR) << "Failed to serialize command ack";
        return;
    }

    std::vector<uint8_t> payload(serialized.begin(), serialized.end());
    auto result = transport_client_->publish(payload, client::Persistence::BestEffort);

    if (result.ok()) {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.command_acks_sent++;
        stats_.bytes_sent += serialized.size();
        VLOG(1) << "Sent command ack for " << command_id;
    } else {
        LOG(WARNING) << "Failed to send command ack for " << command_id
                     << ": status=" << static_cast<int>(result.status);
    }

    last_activity_time_ = std::chrono::steady_clock::now();
}

// =============================================================================
// Sync Protocol v2 Methods
// =============================================================================

namespace sync_v2 = swdv::scheduler_sync_v2;

void SyncedJobState::ToJobRecord(sync_v2::JobRecord* record) const {
    record->set_job_id(job_id);
    record->set_authority(authority);

    auto* ver = record->mutable_version();
    ver->set_cloud_seq(version.cloud_seq);
    ver->set_vehicle_seq(version.vehicle_seq);

    record->set_sync_state(sync_state);
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

    // Map status
    switch (status) {
        case sync_v2::JOB_STATUS_PENDING:
            record->set_status(sync_v2::JOB_STATUS_PENDING);
            break;
        case sync_v2::JOB_STATUS_RUNNING:
            record->set_status(sync_v2::JOB_STATUS_RUNNING);
            break;
        case sync_v2::JOB_STATUS_COMPLETED:
            record->set_status(sync_v2::JOB_STATUS_COMPLETED);
            break;
        case sync_v2::JOB_STATUS_FAILED:
            record->set_status(sync_v2::JOB_STATUS_FAILED);
            break;
        case sync_v2::JOB_STATUS_CANCELLED:
            record->set_status(sync_v2::JOB_STATUS_CANCELLED);
            break;
        default:
            record->set_status(sync_v2::JOB_STATUS_UNKNOWN);
    }

    // Map wake/sleep policies
    record->set_wake_policy(wake_policy == sync_v2::WAKE_POLICY_WAKE_REQUIRED
        ? sync_v2::WAKE_POLICY_WAKE_REQUIRED
        : sync_v2::WAKE_POLICY_NO_WAKE);
    record->set_sleep_policy(sleep_policy == sync_v2::SLEEP_POLICY_INHIBIT_UNTIL_COMPLETE
        ? sync_v2::SLEEP_POLICY_INHIBIT_UNTIL_COMPLETE
        : sync_v2::SLEEP_POLICY_NORMAL);
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
    state.sync_state = record.sync_state();
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

    // Map status
    switch (record.status()) {
        case sync_v2::JOB_STATUS_PENDING:
            state.status = sync_v2::JOB_STATUS_PENDING;
            break;
        case sync_v2::JOB_STATUS_RUNNING:
            state.status = sync_v2::JOB_STATUS_RUNNING;
            break;
        case sync_v2::JOB_STATUS_COMPLETED:
            state.status = sync_v2::JOB_STATUS_COMPLETED;
            break;
        case sync_v2::JOB_STATUS_FAILED:
            state.status = sync_v2::JOB_STATUS_FAILED;
            break;
        case sync_v2::JOB_STATUS_CANCELLED:
            state.status = sync_v2::JOB_STATUS_CANCELLED;
            break;
        default:
            state.status = sync_v2::JOB_STATUS_PENDING;
    }

    // Map wake/sleep policies
    state.wake_policy = (record.wake_policy() == sync_v2::WAKE_POLICY_WAKE_REQUIRED)
        ? sync_v2::WAKE_POLICY_WAKE_REQUIRED : sync_v2::WAKE_POLICY_NO_WAKE;
    state.sleep_policy = (record.sleep_policy() == sync_v2::SLEEP_POLICY_INHIBIT_UNTIL_COMPLETE)
        ? sync_v2::SLEEP_POLICY_INHIBIT_UNTIL_COMPLETE : sync_v2::SLEEP_POLICY_NORMAL;
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

    // Add job records
    for (const auto& job : jobs) {
        if (include_all_jobs || job.sync_state == sync_v2::SYNC_STATE_PENDING) {
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
    msg.set_sync_id(GenerateSyncId());

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

    // Add the deleted job ID
    msg.add_deleted_job_ids(job_id);

    msg.set_sync_timestamp_ms(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    msg.set_sync_id(GenerateSyncId());

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
    LOG(INFO) << "Received C2V_SyncMessage from cloud: sync_id=" << msg.sync_id()
              << ", jobs=" << msg.jobs_size()
              << ", deleted=" << msg.deleted_job_ids_size();

    std::vector<sync_v2::ConflictResolution> conflicts;

    // Process each job from cloud
    for (const auto& remote_job : msg.jobs()) {
        ProcessCloudJob(remote_job);
    }

    // Process deletions (tombstones)
    for (const auto& job_id : msg.deleted_job_ids()) {
        // Mark job as deleted locally
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = synced_state_.find(job_id);
        if (it != synced_state_.end()) {
            it->second.deleted = true;
            it->second.sync_state = sync_v2::SYNC_STATE_SYNCED;
        }
    }

    // Send acknowledgment
    SendSyncAck(msg.sync_id(), true, conflicts);
}

void SchedulerSyncBridge::ProcessCloudJob(const sync_v2::JobRecord& remote_job) {
    const std::string& job_id = remote_job.job_id();

    // Get local version if exists
    std::optional<sync::VersionVector> local_version;
    sync_v2::JobAuthority authority = remote_job.authority();

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = synced_state_.find(job_id);
        if (it != synced_state_.end()) {
            local_version = it->second.version;
        }
    }

    // Use sync engine to determine action
    sync::VersionVector remote_version(
        remote_job.version().cloud_seq(),
        remote_job.version().vehicle_seq());

    sync::SyncResult result = sync::SyncEngine::process_remote(
        remote_version,
        local_version,
        static_cast<sync::JobAuthority>(authority),
        false  // We are vehicle side
    );

    switch (result.action) {
        case sync::SyncResult::NO_ACTION:
            VLOG(1) << "Job " << job_id << ": no action (already in sync)";
            break;

        case sync::SyncResult::ACCEPT_REMOTE:
            VLOG(1) << "Job " << job_id << ": accepting remote version";
            ApplyCloudJob(remote_job);
            break;

        case sync::SyncResult::REJECT_REMOTE:
            VLOG(1) << "Job " << job_id << ": rejecting remote (local dominates)";
            // Our local version is newer - will be synced on next outbound
            break;

        case sync::SyncResult::CONFLICT_RESOLVED:
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
                    it->second.sync_state = sync_v2::SYNC_STATE_SYNCED;
                }
            }
            break;
    }
}

void SchedulerSyncBridge::SendSyncAck(const std::string& sync_id, bool success,
                                     const std::vector<sync_v2::ConflictResolution>& conflicts,
                                     const std::string& error_message) {
    sync_v2::SyncAck ack;
    ack.set_sync_id(sync_id);
    ack.set_success(success);
    for (const auto& conflict : conflicts) {
        *ack.add_conflicts() = conflict;
    }
    ack.set_ack_timestamp_ms(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    if (!error_message.empty()) {
        ack.set_error_message(error_message);
    }

    std::string serialized;
    if (!ack.SerializeToString(&serialized)) {
        LOG(ERROR) << "Failed to serialize SyncAck";
        return;
    }

    std::vector<uint8_t> payload(serialized.begin(), serialized.end());
    auto result = transport_client_->publish(payload, client::Persistence::BestEffort);

    if (result.ok()) {
        VLOG(1) << "Sent SyncAck for " << sync_id;
    } else {
        LOG(WARNING) << "Failed to send SyncAck: status=" << static_cast<int>(result.status);
    }
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

// Helper method to apply a cloud job to local state (forward declaration needed)
void SchedulerSyncBridge::ApplyCloudJob(const sync_v2::JobRecord& remote_job) {
    // Update local synced state
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        synced_state_[remote_job.job_id()] = SyncedJobState::FromJobRecord(remote_job);
        synced_state_[remote_job.job_id()].sync_state = sync_v2::SYNC_STATE_SYNCED;
    }

    // TODO: Also update the actual Scheduler service via gRPC
    // This would call create_job, update_job, or delete_job depending on the job state
    LOG(INFO) << "Applied cloud job " << remote_job.job_id() << " to local state";
}

}  // namespace ifex::reference
