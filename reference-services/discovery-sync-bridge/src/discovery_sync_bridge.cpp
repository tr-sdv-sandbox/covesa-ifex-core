/**
 * @file discovery_sync_bridge.cpp
 * @brief Implementation of DiscoverySyncBridge
 */

#include "discovery_sync_bridge.hpp"
#include "ifex_content_ids.hpp"

#include <glog/logging.h>
#include <grpcpp/grpcpp.h>

#include <chrono>
#include <iomanip>
#include <random>
#include <set>
#include <sstream>
#include <algorithm>

namespace ifex::reference {

namespace {

// Validate that a hash is a proper 64-character hex string (SHA-256)
bool IsValidSchemaHash(const std::string& hash) {
    if (hash.size() != 64) {
        return false;
    }
    return std::all_of(hash.begin(), hash.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    });
}

}  // namespace

using namespace std::chrono_literals;
namespace sync_pb = swdv::discovery_sync_envelope;
namespace discovery_pb = swdv::service_discovery;

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

    // Register c2v handler for cloud schema requests (hash-based protocol)
    transport_client_->on_content([this](const std::vector<uint8_t>& payload) {
        HandleC2vMessage(payload);
    });

    // Connect to Discovery service
    discovery_channel_ = grpc::CreateChannel(
        config_.discovery_endpoint,
        grpc::InsecureChannelCredentials());

    // Initialize hash-based query stubs
    hash_stub_ = discovery_pb::get_service_hashes_service::NewStub(discovery_channel_);
    schema_stub_ = discovery_pb::get_schemas_by_hash_service::NewStub(discovery_channel_);

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

    // Start worker thread
    poll_thread_ = std::thread(&DiscoverySyncBridge::PollLoop, this);

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

    // Wait for thread
    if (poll_thread_.joinable()) {
        poll_thread_.join();
    }

    // Persist final state
    PersistState();

    // Cleanup
    transport_client_.reset();

    LOG(INFO) << "DiscoverySyncBridge stopped";
}

bool DiscoverySyncBridge::IsConnected() const {
    if (!transport_client_) return false;
    return transport_client_->healthy();
}

DiscoverySyncStats DiscoverySyncBridge::GetStats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    DiscoverySyncStats stats = stats_;
    stats.is_initialized = initialized_.load();
    stats.is_connected = IsConnected();

    std::lock_guard<std::mutex> hashes_lock(hashes_mutex_);
    stats.hashes_tracked = current_hashes_.size();

    return stats;
}

void DiscoverySyncBridge::ForceManifestSync() {
    LOG(INFO) << "Forcing hash manifest sync";
    auto hashes = QueryServiceHashes();
    SendHashManifest(hashes);
}

// =============================================================================
// Internal Methods
// =============================================================================

void DiscoverySyncBridge::PollLoop() {
    LOG(INFO) << "Poll thread started, waiting " << config_.initialization_delay_ms
              << "ms for initialization";

    // Initialization delay - allow services to register
    {
        std::unique_lock<std::mutex> lock(cv_mutex_);
        cv_.wait_for(lock, std::chrono::milliseconds(config_.initialization_delay_ms),
                     [this]() { return stop_requested_.load(); });
    }

    if (stop_requested_.load()) {
        LOG(INFO) << "Poll thread stopping (during init)";
        return;
    }

    // Initial hash manifest sync (hash-based protocol)
    LOG(INFO) << "Initialization complete, sending initial hash manifest";
    auto hashes = QueryServiceHashes();
    SendHashManifest(hashes);

    // Track last sent hashes for change detection
    std::set<std::string> last_sent_hashes;
    for (const auto& [hash, name] : hashes) {
        last_sent_hashes.insert(hash);
    }

    // Update tracked hashes for stats
    {
        std::lock_guard<std::mutex> lock(hashes_mutex_);
        current_hashes_ = last_sent_hashes;
    }

    initialized_.store(true);
    LOG(INFO) << "Initial sync complete, " << hashes.size() << " service hashes sent";

    // Main poll loop - hash-based protocol
    while (!stop_requested_.load()) {
        {
            std::unique_lock<std::mutex> lock(cv_mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(config_.poll_interval_ms),
                         [this]() { return stop_requested_.load(); });
        }

        if (stop_requested_.load()) break;

        VLOG(1) << "Polling Discovery for hash changes...";

        // Query current hashes
        auto current_hashes = QueryServiceHashes();

        // Build set for comparison
        std::set<std::string> current_hash_set;
        for (const auto& [hash, name] : current_hashes) {
            current_hash_set.insert(hash);
        }

        // Check if hashes changed
        if (current_hash_set != last_sent_hashes) {
            LOG(INFO) << "Service hashes changed, sending updated manifest";
            SendHashManifest(current_hashes);
            last_sent_hashes = current_hash_set;

            // Update tracked hashes for stats
            {
                std::lock_guard<std::mutex> lock(hashes_mutex_);
                current_hashes_ = current_hash_set;
            }
        } else {
            VLOG(1) << "No hash changes detected";
        }

        // Send heartbeat if no activity (optional)
        MaybeSendHeartbeat();
    }

    LOG(INFO) << "Poll thread stopped";
}

// Legacy methods removed - using hash-based protocol only

void DiscoverySyncBridge::MaybeSendHeartbeat() {
    if (config_.heartbeat_interval_ms == 0) return;

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - last_activity_time_).count();

    if (elapsed >= config_.heartbeat_interval_ms) {
        // For hash-based protocol, heartbeat is just re-sending the current manifest
        // This confirms the vehicle is alive and its service set hasn't changed
        auto hashes = QueryServiceHashes();

        sync_pb::discovery_envelope_t envelope;
        envelope.set_vehicle_id(config_.vehicle_id);

        auto* manifest = envelope.mutable_manifest();
        for (const auto& [hash, name] : hashes) {
            auto* entry = manifest->add_hashes();
            entry->set_service_name(name);
            entry->set_schema_hash(hash);
        }

        std::string serialized;
        if (envelope.SerializeToString(&serialized)) {
            std::vector<uint8_t> payload(serialized.begin(), serialized.end());
            auto result = transport_client_->publish(payload, client::Persistence::Volatile);

            if (result.ok()) {
                std::lock_guard<std::mutex> lock(stats_mutex_);
                stats_.heartbeats_sent++;
                stats_.bytes_sent += serialized.size();
                VLOG(1) << "Sent heartbeat (hash manifest with " << hashes.size() << " hashes)";
            }
        }

        last_activity_time_ = now;
    }
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
    // Hash-based protocol doesn't need to persist state - hashes are always fresh from Discovery
    if (config_.state_persistence_path.empty()) return;
    LOG(INFO) << "Persistence configured but not used for hash-based protocol";
}

void DiscoverySyncBridge::PersistState() {
    // Hash-based protocol doesn't need to persist state
}

void DiscoverySyncBridge::UpdateStats(uint64_t bytes_sent, bool is_manifest) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.bytes_sent += bytes_sent;
    stats_.last_sync_timestamp_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

    if (is_manifest) {
        stats_.manifests_sent++;
    } else {
        stats_.schema_responses_sent++;
    }
}

// =============================================================================
// Hash-based sync protocol (new, bandwidth-efficient)
// =============================================================================

std::vector<std::pair<std::string, std::string>> DiscoverySyncBridge::QueryServiceHashes() {
    std::vector<std::pair<std::string, std::string>> result;

    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + 5s);

    discovery_pb::get_service_hashes_request request;
    discovery_pb::get_service_hashes_response response;

    auto status = hash_stub_->get_service_hashes(&context, request, &response);

    if (!status.ok()) {
        LOG(WARNING) << "Failed to query service hashes: " << status.error_message();
        return result;
    }

    for (const auto& entry : response.hashes()) {
        // Sanity check: only accept valid SHA-256 hashes (64 hex chars)
        if (!IsValidSchemaHash(entry.schema_hash())) {
            LOG(WARNING) << "Skipping invalid hash entry:"
                         << " service_name='" << entry.service_name() << "'"
                         << " schema_hash='" << entry.schema_hash() << "'"
                         << " (hash length=" << entry.schema_hash().size() << ", expected 64)";
            continue;
        }
        result.emplace_back(entry.schema_hash(), entry.service_name());
    }

    VLOG(1) << "Queried " << result.size() << " valid service hashes from Discovery";
    return result;
}

std::map<std::string, std::string> DiscoverySyncBridge::QuerySchemasByHash(
    const std::vector<std::string>& hashes) {
    std::map<std::string, std::string> result;

    if (hashes.empty()) return result;

    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + 10s);

    discovery_pb::get_schemas_by_hash_request request;
    for (const auto& hash : hashes) {
        request.add_hashes(hash);
    }

    discovery_pb::get_schemas_by_hash_response response;
    auto status = schema_stub_->get_schemas_by_hash(&context, request, &response);

    if (!status.ok()) {
        LOG(WARNING) << "Failed to query schemas by hash: " << status.error_message();
        return result;
    }

    for (const auto& entry : response.schemas()) {
        result[entry.schema_hash()] = entry.ifex_schema();
    }

    VLOG(1) << "Retrieved " << result.size() << " schemas by hash";
    return result;
}

void DiscoverySyncBridge::SendHashManifest(
    const std::vector<std::pair<std::string, std::string>>& hashes) {

    sync_pb::discovery_envelope_t envelope;
    envelope.set_vehicle_id(config_.vehicle_id);

    auto* manifest = envelope.mutable_manifest();
    for (const auto& [hash, name] : hashes) {
        auto* entry = manifest->add_hashes();
        entry->set_service_name(name);
        entry->set_schema_hash(hash);
    }

    std::string serialized;
    if (!envelope.SerializeToString(&serialized)) {
        LOG(ERROR) << "Failed to serialize hash manifest";
        return;
    }

    std::vector<uint8_t> payload(serialized.begin(), serialized.end());
    auto result = transport_client_->publish(payload, client::Persistence::Volatile);

    if (result.ok()) {
        UpdateStats(serialized.size(), true);
        LOG(INFO) << "Published hash manifest with " << hashes.size()
                  << " hashes (" << serialized.size() << " bytes)";
    } else {
        LOG(WARNING) << "Failed to publish hash manifest: status="
                     << static_cast<int>(result.status);
    }

    last_activity_time_ = std::chrono::steady_clock::now();
}

void DiscoverySyncBridge::SendSchemas(const std::map<std::string, std::string>& schemas) {
    if (schemas.empty()) return;

    sync_pb::discovery_envelope_t envelope;
    envelope.set_vehicle_id(config_.vehicle_id);

    auto* schema_map = envelope.mutable_schemas();
    for (const auto& [hash, yaml] : schemas) {
        auto* entry = schema_map->add_schemas();
        entry->set_schema_hash(hash);
        entry->set_ifex_schema(yaml);
    }

    std::string serialized;
    if (!envelope.SerializeToString(&serialized)) {
        LOG(ERROR) << "Failed to serialize schema response";
        return;
    }

    std::vector<uint8_t> payload(serialized.begin(), serialized.end());
    auto result = transport_client_->publish(payload, client::Persistence::Volatile);

    if (result.ok()) {
        UpdateStats(serialized.size(), false);
        LOG(INFO) << "Published " << schemas.size()
                  << " schemas (" << serialized.size() << " bytes)";
    } else {
        LOG(WARNING) << "Failed to publish schemas: status="
                     << static_cast<int>(result.status);
    }

    last_activity_time_ = std::chrono::steady_clock::now();
}

void DiscoverySyncBridge::HandleC2vMessage(const std::vector<uint8_t>& payload) {
    sync_pb::discovery_envelope_t envelope;
    if (!envelope.ParseFromArray(payload.data(), payload.size())) {
        LOG(WARNING) << "Failed to parse c2v discovery envelope";
        return;
    }

    if (envelope.has_request()) {
        // Cloud is requesting specific schemas
        const auto& request = envelope.request();
        LOG(INFO) << "Cloud requested " << request.hashes_size() << " schemas";

        std::vector<std::string> requested_hashes;
        for (const auto& hash : request.hashes()) {
            requested_hashes.push_back(hash);
        }

        // Query Discovery for the requested schemas
        auto schemas = QuerySchemasByHash(requested_hashes);

        // Send the schemas back to cloud
        SendSchemas(schemas);
    }
}

}  // namespace ifex::reference
