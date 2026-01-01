/**
 * @file discovery_sync_bridge.cpp
 * @brief Implementation of DiscoverySyncBridge
 */

#include "discovery_sync_bridge.hpp"
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
namespace sync_pb = swdv::discovery_sync_envelope;
namespace discovery_pb = swdv::service_discovery;

// =============================================================================
// SyncedServiceState
// =============================================================================

uint64_t SyncedServiceState::ComputeHash() const {
    // Simple hash combining key fields
    std::hash<std::string> str_hash;
    std::hash<uint64_t> u64_hash;
    std::hash<int> int_hash;

    uint64_t h = str_hash(registration_id);
    h ^= str_hash(name) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= str_hash(version) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= str_hash(address) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= int_hash(static_cast<int>(status)) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= u64_hash(last_heartbeat_ms) + 0x9e3779b9 + (h << 6) + (h >> 2);

    return h;
}

// =============================================================================
// DiscoverySyncBridge
// =============================================================================

DiscoverySyncBridge::DiscoverySyncBridge(const DiscoverySyncBridgeConfig& config)
    : config_(config)
    , instance_id_(GenerateInstanceId()) {
    LOG(INFO) << "Creating DiscoverySyncBridge instance: " << instance_id_;
}

DiscoverySyncBridge::~DiscoverySyncBridge() {
    Stop();
}

bool DiscoverySyncBridge::Start() {
    if (running_.load()) {
        LOG(WARNING) << "DiscoverySyncBridge already running";
        return true;
    }

    LOG(INFO) << "Starting DiscoverySyncBridge...";
    LOG(INFO) << "  Discovery endpoint: " << config_.discovery_endpoint;
    LOG(INFO) << "  Backend Transport endpoint: " << config_.backend_transport_endpoint;
    LOG(INFO) << "  Sync content_id: " << config_.sync_content_id;
    LOG(INFO) << "  Initialization delay: " << config_.initialization_delay_ms << "ms";

    // Connect to Backend Transport
    auto bt_channel = grpc::CreateChannel(
        config_.backend_transport_endpoint,
        grpc::InsecureChannelCredentials());

    transport_client_ = std::make_unique<client::BackendTransportClient>(
        bt_channel, config_.sync_content_id);

    // Connect to Discovery service
    discovery_channel_ = grpc::CreateChannel(
        config_.discovery_endpoint,
        grpc::InsecureChannelCredentials());

    query_stub_ = discovery_pb::query_services_service::NewStub(discovery_channel_);

    // Verify Discovery connection
    auto state = discovery_channel_->GetState(true);
    auto deadline = std::chrono::system_clock::now() + 5s;
    if (!discovery_channel_->WaitForConnected(deadline)) {
        LOG(ERROR) << "Failed to connect to Discovery service";
        return false;
    }

    // Load persisted state if available
    LoadPersistedState();

    running_.store(true);
    stop_requested_.store(false);
    last_activity_time_ = std::chrono::steady_clock::now();

    // Start worker threads
    poll_thread_ = std::thread(&DiscoverySyncBridge::PollLoop, this);

    if (config_.batch_window_ms > 0) {
        batch_thread_ = std::thread(&DiscoverySyncBridge::BatchLoop, this);
    }

    LOG(INFO) << "DiscoverySyncBridge started";
    return true;
}

void DiscoverySyncBridge::Stop() {
    if (!running_.load()) {
        return;
    }

    LOG(INFO) << "Stopping DiscoverySyncBridge...";

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
    query_stub_.reset();

    LOG(INFO) << "DiscoverySyncBridge stopped";
}

bool DiscoverySyncBridge::IsConnected() const {
    if (!transport_client_) return false;
    return transport_client_->healthy();
}

DiscoverySyncStats DiscoverySyncBridge::GetStats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    DiscoverySyncStats stats = stats_;
    stats.current_sequence = sequence_number_.load();
    stats.is_initialized = initialized_.load();
    stats.is_connected = IsConnected();

    std::lock_guard<std::mutex> state_lock(state_mutex_);
    stats.services_tracked = synced_state_.size();

    return stats;
}

void DiscoverySyncBridge::ForceFullSync() {
    LOG(INFO) << "Forcing full sync";
    auto services = QueryDiscoveryServices();
    SendFullSync(services);
}

uint32_t DiscoverySyncBridge::GetStateChecksum() const {
    return ComputeStateChecksum();
}

// =============================================================================
// Internal Methods
// =============================================================================

void DiscoverySyncBridge::PollLoop() {
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
    auto services = QueryDiscoveryServices();
    SendFullSync(services);

    // Update synced state
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        synced_state_.clear();
        for (const auto& svc : services) {
            synced_state_[svc.registration_id] = svc;
        }
    }

    initialized_.store(true);
    LOG(INFO) << "Initial sync complete, " << services.size() << " services synced";

    // Main poll loop
    while (!stop_requested_.load()) {
        {
            std::unique_lock<std::mutex> lock(cv_mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(config_.poll_interval_ms),
                         [this]() { return stop_requested_.load(); });
        }

        if (stop_requested_.load()) break;

        VLOG(1) << "Polling Discovery for changes...";

        // Query current state
        auto current = QueryDiscoveryServices();

        VLOG(1) << "Found " << current.size() << " services in Discovery";

        // Detect and publish changes
        DetectChanges(current);

        // Flush events if no batching (batch_window_ms == 0)
        if (config_.batch_window_ms == 0) {
            FlushEvents();
        }

        // Send heartbeat if no activity
        MaybeSendHeartbeat();
    }

    LOG(INFO) << "Poll thread stopped";
}

void DiscoverySyncBridge::BatchLoop() {
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

std::vector<SyncedServiceState> DiscoverySyncBridge::QueryDiscoveryServices() {
    std::vector<SyncedServiceState> result;

    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + 5s);

    discovery_pb::query_services_request request;
    // Empty filter = return all services

    discovery_pb::query_services_response response;
    auto status = query_stub_->query_services(&context, request, &response);

    if (!status.ok()) {
        LOG(WARNING) << "Failed to query Discovery: " << status.error_message();
        return result;
    }

    for (const auto& svc : response.services()) {
        SyncedServiceState state;
        // Use name as the unique identifier (Discovery doesn't return registration_id in query)
        state.registration_id = svc.name();  // Using name as ID
        state.name = svc.name();
        state.version = svc.version();

        if (svc.has_endpoint()) {
            state.address = svc.endpoint().address();
        }

        // Map status (Discovery proto uses unprefixed enum values)
        switch (svc.status()) {
            case discovery_pb::AVAILABLE:
                state.status = sync_pb::AVAILABLE;
                break;
            case discovery_pb::UNAVAILABLE:
                state.status = sync_pb::UNAVAILABLE;
                break;
            case discovery_pb::STARTING:
                state.status = sync_pb::STARTING;
                break;
            case discovery_pb::STOPPING:
                state.status = sync_pb::STOPPING;
                break;
            case discovery_pb::ERROR:
                state.status = sync_pb::ERROR;
                break;
            default:
                state.status = sync_pb::UNAVAILABLE;
        }

        state.last_heartbeat_ms = svc.last_heartbeat();

        result.push_back(std::move(state));
    }

    return result;
}

void DiscoverySyncBridge::DetectChanges(const std::vector<SyncedServiceState>& current) {
    std::lock_guard<std::mutex> lock(state_mutex_);

    // Build map of current services
    std::unordered_map<std::string, const SyncedServiceState*> current_map;
    for (const auto& svc : current) {
        current_map[svc.registration_id] = &svc;
    }

    // Check for unregistered services
    std::vector<std::string> to_remove;
    for (const auto& [reg_id, synced] : synced_state_) {
        if (current_map.find(reg_id) == current_map.end()) {
            // Service was unregistered
            sync_pb::sync_event_t event;
            event.set_event_type(sync_pb::SERVICE_UNREGISTERED);
            event.set_sequence_number(++sequence_number_);
            event.set_timestamp_ns(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
            event.set_registration_id(reg_id);

            QueueEvent(std::move(event));
            to_remove.push_back(reg_id);

            LOG(INFO) << "Service unregistered: " << synced.name
                      << " (id=" << reg_id << ")";
        }
    }

    for (const auto& reg_id : to_remove) {
        synced_state_.erase(reg_id);
    }

    // Check for new or changed services
    for (const auto& svc : current) {
        auto it = synced_state_.find(svc.registration_id);

        if (it == synced_state_.end()) {
            // New service registered
            sync_pb::sync_event_t event;
            event.set_event_type(sync_pb::SERVICE_REGISTERED);
            event.set_sequence_number(++sequence_number_);
            event.set_timestamp_ns(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
            event.set_registration_id(svc.registration_id);
            *event.mutable_service_info() = BuildServiceInfo(svc);

            QueueEvent(std::move(event));
            synced_state_[svc.registration_id] = svc;

            LOG(INFO) << "Service registered: " << svc.name
                      << " (id=" << svc.registration_id << ")";
        } else {
            // Check if changed
            if (svc.ComputeHash() != it->second.ComputeHash()) {
                sync_pb::sync_event_t event;
                event.set_event_type(sync_pb::SERVICE_STATUS_CHANGED);
                event.set_sequence_number(++sequence_number_);
                event.set_timestamp_ns(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count());
                event.set_registration_id(svc.registration_id);
                *event.mutable_service_info() = BuildServiceInfo(svc);

                QueueEvent(std::move(event));
                it->second = svc;

                VLOG(1) << "Service status changed: " << svc.name;
            }
        }
    }
}

void DiscoverySyncBridge::QueueEvent(sync_pb::sync_event_t event) {
    std::lock_guard<std::mutex> lock(events_mutex_);
    pending_events_.push_back(std::move(event));
    last_activity_time_ = std::chrono::steady_clock::now();
    // Note: FlushEvents is called from the poll loop to avoid deadlock
    // since DetectChanges holds state_mutex_ when calling this
}

void DiscoverySyncBridge::FlushEvents() {
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
        message.set_total_services(static_cast<uint32_t>(synced_state_.size()));
    }

    for (auto& event : events) {
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
        UpdateStats(serialized.size(), false);
        VLOG(1) << "Published " << events.size() << " sync events ("
                << serialized.size() << " bytes)";
    } else {
        LOG(WARNING) << "Failed to publish sync events: status="
                     << static_cast<int>(result.status);
    }
}

void DiscoverySyncBridge::SendFullSync(const std::vector<SyncedServiceState>& services) {
    sync_pb::sync_message_t message;
    message.set_vehicle_id(config_.vehicle_id);
    message.set_bridge_instance_id(instance_id_);
    message.set_total_services(static_cast<uint32_t>(services.size()));

    // Create FULL_SYNC event with all services
    sync_pb::sync_event_t event;
    event.set_event_type(sync_pb::FULL_SYNC);
    event.set_sequence_number(++sequence_number_);
    event.set_timestamp_ns(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    // For FULL_SYNC, we send multiple events, one per service
    *message.add_events() = event;

    for (const auto& svc : services) {
        sync_pb::sync_event_t svc_event;
        svc_event.set_event_type(sync_pb::SERVICE_REGISTERED);
        svc_event.set_sequence_number(++sequence_number_);
        svc_event.set_timestamp_ns(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        svc_event.set_registration_id(svc.registration_id);
        *svc_event.mutable_service_info() = BuildServiceInfo(svc);

        *message.add_events() = std::move(svc_event);
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
        UpdateStats(serialized.size(), true);
        LOG(INFO) << "Published FULL_SYNC with " << services.size()
                  << " services (" << serialized.size() << " bytes)";
    } else {
        LOG(WARNING) << "Failed to publish full sync: status="
                     << static_cast<int>(result.status);
    }

    last_activity_time_ = std::chrono::steady_clock::now();
}

void DiscoverySyncBridge::MaybeSendHeartbeat() {
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
            message.set_total_services(static_cast<uint32_t>(synced_state_.size()));
        }

        sync_pb::sync_event_t event;
        event.set_event_type(sync_pb::HEARTBEAT);
        event.set_sequence_number(++sequence_number_);
        event.set_timestamp_ns(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
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

sync_pb::service_info_t DiscoverySyncBridge::BuildServiceInfo(
    const SyncedServiceState& state) {

    sync_pb::service_info_t info;
    info.set_registration_id(state.registration_id);
    info.set_name(state.name);
    info.set_version(state.version);
    info.mutable_endpoint()->set_address(state.address);
    info.mutable_endpoint()->set_transport(sync_pb::GRPC);  // Default to gRPC
    info.set_status(state.status);
    info.set_last_heartbeat_ms(state.last_heartbeat_ms);

    return info;
}

uint32_t DiscoverySyncBridge::ComputeStateChecksum() const {
    std::lock_guard<std::mutex> lock(state_mutex_);

    // Simple CRC32-like checksum
    uint32_t crc = 0xFFFFFFFF;

    // Sort by registration_id for deterministic ordering
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

std::string DiscoverySyncBridge::GenerateInstanceId() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint64_t> dis;

    std::stringstream ss;
    ss << "dsb_" << std::hex << std::setw(16) << std::setfill('0') << dis(gen);
    return ss.str();
}

void DiscoverySyncBridge::LoadPersistedState() {
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

void DiscoverySyncBridge::PersistState() {
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

void DiscoverySyncBridge::UpdateStats(uint64_t bytes_sent, bool is_full_sync) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.events_sent++;
    stats_.bytes_sent += bytes_sent;
    stats_.last_sync_timestamp_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

    if (is_full_sync) {
        stats_.full_syncs_sent++;
    } else {
        stats_.delta_syncs_sent++;
    }
}

}  // namespace ifex::reference
