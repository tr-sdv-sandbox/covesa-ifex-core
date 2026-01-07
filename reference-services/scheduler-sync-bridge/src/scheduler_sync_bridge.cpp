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
namespace sync_pb = swdv::scheduler_sync_envelope;
namespace cmd_pb = swdv::scheduler_command_envelope;
namespace scheduler_pb = swdv::ifex_scheduler;

// =============================================================================
// SyncedJobState
// =============================================================================

uint64_t SyncedJobState::ComputeHash() const {
    // Simple hash combining key fields
    std::hash<std::string> str_hash;
    std::hash<uint64_t> u64_hash;
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
    h ^= u64_hash(updated_at_ms) + 0x9e3779b9 + (h << 6) + (h >> 2);

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
    SendFullSync(jobs);
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
    SendFullSync(jobs);

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

        // Detect and publish changes
        DetectChanges(current);

        // Flush events if no batching
        if (config_.batch_window_ms == 0) {
            FlushEvents();
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

        FlushEvents();
    }

    // Final flush
    FlushEvents();

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
        state.scheduled_time = job.scheduled_time();
        state.recurrence_rule = job.recurrence_rule();
        state.next_run_time = job.next_run_time();
        state.status = MapStatus(job.status());

        // Parse timestamps (assuming ISO format or ms)
        // For simplicity, use current time if not available
        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        state.created_at_ms = now_ms;  // Would parse from job.created_at()
        state.updated_at_ms = now_ms;  // Would parse from job.updated_at()

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
    std::vector<std::string> to_remove;
    for (const auto& [job_id, synced] : synced_state_) {
        if (current_map.find(job_id) == current_map.end()) {
            // Job was deleted
            sync_pb::sync_event_t event;
            event.set_event_type(sync_pb::JOB_DELETED);
            event.set_sequence_number(++sequence_number_);
            event.set_timestamp_ms(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
            event.set_job_id(job_id);

            QueueEvent(std::move(event));
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
                // Job completed/failed - send execution result
                sync_pb::sync_event_t event;
                event.set_event_type(sync_pb::JOB_EXECUTED);
                event.set_sequence_number(++sequence_number_);
                event.set_timestamp_ms(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count());
                event.set_job_id(job.job_id);

                auto* result = event.mutable_execution_result();
                result->set_job_id(job.job_id);
                result->set_status(job.status);
                result->set_executed_at_ms(job.updated_at_ms);
                result->set_duration_ms(0);  // Would calculate from actual execution
                if (!job.recurrence_rule.empty()) {
                    result->set_next_run_time(job.next_run_time);
                }

                QueueEvent(std::move(event));
                synced_terminal_jobs_.insert(job.job_id);

                LOG(INFO) << "Job executed: " << job.title << " (id=" << job.job_id
                          << ", status=" << static_cast<int>(job.status) << ")";
            } else {
                // New active job
                sync_pb::sync_event_t event;
                event.set_event_type(sync_pb::JOB_CREATED);
                event.set_sequence_number(++sequence_number_);
                event.set_timestamp_ms(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count());
                event.set_job_id(job.job_id);
                *event.mutable_job_info() = BuildJobInfo(job);

                QueueEvent(std::move(event));
                synced_state_[job.job_id] = job;

                LOG(INFO) << "Job created: " << job.title << " (id=" << job.job_id << ")";
            }
        } else {
            // Existing job - check for changes
            const auto& synced = it->second;

            if (job.IsTerminal()) {
                // Job transitioned to terminal state
                sync_pb::sync_event_t event;
                event.set_event_type(sync_pb::JOB_EXECUTED);
                event.set_sequence_number(++sequence_number_);
                event.set_timestamp_ms(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count());
                event.set_job_id(job.job_id);

                *event.mutable_execution_result() = BuildExecutionResult(job, synced);

                QueueEvent(std::move(event));

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
                    job.status == sync_pb::RUNNING &&
                    synced.status == sync_pb::PENDING) {
                    should_sync = false;
                    VLOG(1) << "Skipping RUNNING state update for: " << job.title;
                }

                if (should_sync) {
                    sync_pb::sync_event_t event;
                    event.set_event_type(sync_pb::JOB_UPDATED);
                    event.set_sequence_number(++sequence_number_);
                    event.set_timestamp_ms(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count());
                    event.set_job_id(job.job_id);
                    *event.mutable_job_info() = BuildJobInfo(job);

                    QueueEvent(std::move(event));

                    VLOG(1) << "Job updated: " << job.title;
                }

                it->second = job;
            }
        }
    }
}

void SchedulerSyncBridge::QueueEvent(sync_pb::sync_event_t event) {
    std::lock_guard<std::mutex> lock(events_mutex_);
    pending_events_.push_back(std::move(event));
    last_activity_time_ = std::chrono::steady_clock::now();
}

void SchedulerSyncBridge::FlushEvents() {
    std::vector<sync_pb::sync_event_t> events;
    {
        std::lock_guard<std::mutex> lock(events_mutex_);
        if (pending_events_.empty()) return;
        events.swap(pending_events_);
    }

    // Build sync message
    sync_pb::sync_message_t message;
    message.set_vehicle_id(config_.vehicle_id);
    message.set_bridge_instance_id(instance_id_);
    message.set_state_checksum(ComputeStateChecksum());

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        message.set_active_jobs_count(static_cast<uint32_t>(synced_state_.size()));
    }

    bool has_execution_result = false;
    for (auto& event : events) {
        if (event.event_type() == sync_pb::JOB_EXECUTED) {
            has_execution_result = true;
        }
        *message.add_events() = std::move(event);
    }

    // Serialize and publish
    std::string serialized;
    if (!message.SerializeToString(&serialized)) {
        LOG(ERROR) << "Failed to serialize sync message";
        return;
    }

    std::vector<uint8_t> payload(serialized.begin(), serialized.end());
    auto result = transport_client_->publish(payload, client::Persistence::Volatile);

    if (result.ok()) {
        UpdateStats(serialized.size(), false, has_execution_result);
        VLOG(1) << "Published " << events.size() << " sync events ("
                << serialized.size() << " bytes)";
    } else {
        LOG(WARNING) << "Failed to publish sync events: status="
                     << static_cast<int>(result.status);
    }
}

void SchedulerSyncBridge::SendFullSync(const std::vector<SyncedJobState>& jobs) {
    sync_pb::sync_message_t message;
    message.set_vehicle_id(config_.vehicle_id);
    message.set_bridge_instance_id(instance_id_);

    // Count active jobs
    uint32_t active_count = 0;
    for (const auto& job : jobs) {
        if (!job.IsTerminal()) {
            active_count++;
        }
    }
    message.set_active_jobs_count(active_count);

    // Create FULL_SYNC event
    sync_pb::sync_event_t event;
    event.set_event_type(sync_pb::FULL_SYNC);
    event.set_sequence_number(++sequence_number_);
    event.set_timestamp_ms(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    *message.add_events() = event;

    // Add JOB_CREATED event for each active job
    for (const auto& job : jobs) {
        if (job.IsTerminal()) {
            continue;  // Skip terminal jobs in full sync
        }

        sync_pb::sync_event_t job_event;
        job_event.set_event_type(sync_pb::JOB_CREATED);
        job_event.set_sequence_number(++sequence_number_);
        job_event.set_timestamp_ms(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        job_event.set_job_id(job.job_id);
        *job_event.mutable_job_info() = BuildJobInfo(job);

        *message.add_events() = std::move(job_event);
    }

    message.set_state_checksum(ComputeStateChecksum());

    // Serialize and publish
    std::string serialized;
    if (!message.SerializeToString(&serialized)) {
        LOG(ERROR) << "Failed to serialize full sync message";
        return;
    }

    std::vector<uint8_t> payload(serialized.begin(), serialized.end());
    auto result = transport_client_->publish(payload, client::Persistence::BestEffort);

    if (result.ok()) {
        UpdateStats(serialized.size(), true, false);
        LOG(INFO) << "Published FULL_SYNC with " << active_count
                  << " active jobs (" << serialized.size() << " bytes)";
    } else {
        LOG(WARNING) << "Failed to publish full sync: status="
                     << static_cast<int>(result.status);
    }

    last_activity_time_ = std::chrono::steady_clock::now();
}

void SchedulerSyncBridge::MaybeSendHeartbeat() {
    if (config_.heartbeat_interval_ms == 0) return;

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - last_activity_time_).count();

    if (elapsed >= config_.heartbeat_interval_ms) {
        sync_pb::sync_message_t message;
        message.set_vehicle_id(config_.vehicle_id);
        message.set_bridge_instance_id(instance_id_);
        message.set_state_checksum(ComputeStateChecksum());

        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            message.set_active_jobs_count(static_cast<uint32_t>(synced_state_.size()));
        }

        sync_pb::sync_event_t event;
        event.set_event_type(sync_pb::HEARTBEAT);
        event.set_sequence_number(++sequence_number_);
        event.set_timestamp_ms(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());

        *message.add_events() = std::move(event);

        std::string serialized;
        if (message.SerializeToString(&serialized)) {
            std::vector<uint8_t> payload(serialized.begin(), serialized.end());
            auto result = transport_client_->publish(payload, client::Persistence::Volatile);

            if (result.ok()) {
                std::lock_guard<std::mutex> lock(stats_mutex_);
                stats_.heartbeats_sent++;
                stats_.bytes_sent += serialized.size();
                VLOG(1) << "Sent heartbeat";
            }
        }

        last_activity_time_ = now;
    }
}

sync_pb::job_info_t SchedulerSyncBridge::BuildJobInfo(const SyncedJobState& state) {
    sync_pb::job_info_t info;
    info.set_job_id(state.job_id);
    info.set_title(state.title);
    info.set_service(state.service);
    info.set_method(state.method);
    info.set_parameters(state.parameters);
    info.set_scheduled_time(state.scheduled_time);
    info.set_recurrence_rule(state.recurrence_rule);
    info.set_next_run_time(state.next_run_time);
    info.set_status(state.status);
    info.set_created_at_ms(state.created_at_ms);
    info.set_updated_at_ms(state.updated_at_ms);

    return info;
}

sync_pb::execution_result_t SchedulerSyncBridge::BuildExecutionResult(
    const SyncedJobState& current,
    const SyncedJobState& previous) {

    sync_pb::execution_result_t result;
    result.set_job_id(current.job_id);
    result.set_status(current.status);
    result.set_executed_at_ms(current.updated_at_ms);

    // Calculate duration if we have previous state
    if (previous.updated_at_ms > 0 && current.updated_at_ms > previous.updated_at_ms) {
        result.set_duration_ms(static_cast<uint32_t>(
            current.updated_at_ms - previous.updated_at_ms));
    }

    // Set next_run_time for recurring jobs
    if (!current.recurrence_rule.empty()) {
        result.set_next_run_time(current.next_run_time);
    }

    return result;
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

sync_pb::job_sync_status_t SchedulerSyncBridge::MapStatus(
    scheduler_pb::job_status_t status) {
    switch (status) {
        case scheduler_pb::PENDING:
            return sync_pb::PENDING;
        case scheduler_pb::RUNNING:
            return sync_pb::RUNNING;
        case scheduler_pb::COMPLETED:
            return sync_pb::COMPLETED;
        case scheduler_pb::FAILED:
            return sync_pb::FAILED;
        case scheduler_pb::CANCELLED:
            return sync_pb::CANCELLED;
        default:
            return sync_pb::PENDING;
    }
}

// =============================================================================
// Cloud Command Handling (c2v)
// =============================================================================

void SchedulerSyncBridge::HandleCloudCommand(const std::vector<uint8_t>& payload) {
    // Decode the command envelope
    cmd_pb::scheduler_command_t command;
    if (!command.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
        LOG(WARNING) << "Failed to parse scheduler command from c2v payload ("
                     << payload.size() << " bytes)";
        return;
    }

    LOG(INFO) << "Received cloud command: id=" << command.command_id()
              << ", type=" << static_cast<int>(command.type());

    // Update stats
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.commands_received++;
    }

    // Process command synchronously - gRPC calls to Scheduler are fast
    ProcessCommand(command);
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
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.commands_delete++;
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
    job->set_title(def.title());
    job->set_service(def.service());
    job->set_method(def.method());
    job->set_parameters(def.parameters_json());
    job->set_scheduled_time(def.scheduled_time());
    job->set_recurrence_rule(def.recurrence_rule());
    job->set_end_time(def.end_time());
    job->set_service_address(def.service_address());

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
    updates->set_scheduled_time(update.scheduled_time());
    updates->set_recurrence_rule(update.recurrence_rule());
    updates->set_parameters(update.parameters_json());
    updates->set_end_time(update.end_time());

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

}  // namespace ifex::reference
