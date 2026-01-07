/**
 * @file discovery_sync_bridge.hpp
 * @brief Bridge service for synchronizing Discovery state to cloud
 *
 * The DiscoverySyncBridge monitors the Discovery service and publishes
 * schema hashes to the cloud via Backend Transport using a hash-based protocol.
 *
 * Hash-based Protocol:
 * 1. Vehicle sends list of schema hashes (SHA-256 of IFEX YAML)
 * 2. Cloud requests full schemas only for unknown hashes
 * 3. Vehicle sends requested schemas
 *
 * Benefits:
 * - Minimal bandwidth: ~100 bytes per reconnect (just hashes)
 * - Fleet deduplication: Same schema stored once across 100K vehicles
 * - Efficient sync: Only new schemas transferred
 */

#pragma once

#include "backend_transport_client.hpp"
#include "discovery-sync-envelope.pb.h"
#include "service-discovery-service.grpc.pb.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace grpc {
class Channel;
}

namespace ifex::reference {

/**
 * @brief Configuration for DiscoverySyncBridge
 */
struct DiscoverySyncBridgeConfig {
    /// Discovery service endpoint
    std::string discovery_endpoint = "localhost:50051";

    /// Backend Transport endpoint for publishing
    std::string backend_transport_endpoint = "localhost:50060";

    /// Content ID for sync messages (default: DISCOVERY_SYNC = 201)
    uint32_t sync_content_id = 201;

    /// Vehicle identifier for messages
    std::string vehicle_id = "vehicle-001";

    /// Initialization delay before starting sync (ms)
    /// Allows services to register before we capture initial state
    uint32_t initialization_delay_ms = 5000;

    /// Polling interval for Discovery changes (ms)
    uint32_t poll_interval_ms = 1000;

    /// How long to batch events before sending (ms)
    /// 0 = send immediately
    uint32_t batch_window_ms = 100;

    /// Heartbeat interval when no changes (ms)
    /// 0 = no heartbeat
    uint32_t heartbeat_interval_ms = 30000;

    /// Path to persist sync state (empty = no persistence)
    std::string state_persistence_path;
};


/**
 * @brief Statistics for monitoring sync bridge health
 */
struct DiscoverySyncStats {
    uint64_t manifests_sent = 0;       ///< Hash manifest messages sent
    uint64_t schema_responses_sent = 0; ///< Schema response messages sent
    uint64_t heartbeats_sent = 0;
    uint64_t bytes_sent = 0;
    uint64_t hashes_tracked = 0;       ///< Number of schema hashes currently tracked
    uint64_t last_sync_timestamp_ms = 0;
    bool is_initialized = false;
    bool is_connected = false;
};

/**
 * @brief Bridge for synchronizing Discovery state to cloud via hash-based protocol
 *
 * Lifecycle:
 * 1. Start() - begins initialization phase
 * 2. After initialization_delay_ms, queries Discovery for schema hashes
 * 3. Publishes hash manifest (list of SHA-256 hashes)
 * 4. Polls Discovery at poll_interval_ms for hash changes
 * 5. Republishes manifest when hashes change
 * 6. Responds to cloud schema requests with full IFEX YAML
 * 7. Stop() - graceful shutdown
 */
class DiscoverySyncBridge {
public:
    explicit DiscoverySyncBridge(const DiscoverySyncBridgeConfig& config);
    ~DiscoverySyncBridge();

    // Non-copyable, non-movable
    DiscoverySyncBridge(const DiscoverySyncBridge&) = delete;
    DiscoverySyncBridge& operator=(const DiscoverySyncBridge&) = delete;

    /**
     * @brief Start the sync bridge
     * @return true if started successfully
     */
    bool Start();

    /**
     * @brief Stop the sync bridge gracefully
     */
    void Stop();

    /**
     * @brief Check if bridge is running
     */
    bool IsRunning() const { return running_.load(); }

    /**
     * @brief Check if initialization phase is complete
     */
    bool IsInitialized() const { return initialized_.load(); }

    /**
     * @brief Check if connected to Backend Transport
     */
    bool IsConnected() const;

    /**
     * @brief Get current statistics
     */
    DiscoverySyncStats GetStats() const;

    /**
     * @brief Force sending hash manifest (for testing or recovery)
     */
    void ForceManifestSync();

private:
    /// Configuration
    DiscoverySyncBridgeConfig config_;

    /// Backend Transport client for publishing
    std::unique_ptr<client::BackendTransportClient> transport_client_;

    /// gRPC channel to Discovery service
    std::shared_ptr<grpc::Channel> discovery_channel_;

    /// Discovery service stubs for hash-based protocol
    std::unique_ptr<swdv::service_discovery::get_service_hashes_service::Stub> hash_stub_;
    std::unique_ptr<swdv::service_discovery::get_schemas_by_hash_service::Stub> schema_stub_;

    /// Statistics
    mutable DiscoverySyncStats stats_;
    mutable std::mutex stats_mutex_;

    /// Unique instance ID (for restart detection)
    std::string instance_id_;

    /// Running state
    std::atomic<bool> running_{false};
    std::atomic<bool> initialized_{false};
    std::atomic<bool> stop_requested_{false};

    /// Worker thread
    std::thread poll_thread_;

    /// Condition variable for signaling
    std::condition_variable cv_;
    std::mutex cv_mutex_;

    /// Last heartbeat time
    std::chrono::steady_clock::time_point last_activity_time_;

    /// Currently tracked hashes (for stats)
    std::set<std::string> current_hashes_;
    mutable std::mutex hashes_mutex_;

    // Internal methods

    /// Main polling loop
    void PollLoop();

    /// Query Discovery for schema hashes
    std::vector<std::pair<std::string, std::string>> QueryServiceHashes();

    /// Query Discovery for schemas by hash
    std::map<std::string, std::string> QuerySchemasByHash(const std::vector<std::string>& hashes);

    /// Send schema manifest (hash list) to cloud
    void SendHashManifest(const std::vector<std::pair<std::string, std::string>>& hashes);

    /// Send schemas to cloud (response to schema request)
    void SendSchemas(const std::map<std::string, std::string>& schemas);

    /// Handle incoming c2v message (schema request from cloud)
    void HandleC2vMessage(const std::vector<uint8_t>& payload);

    /// Send heartbeat if no recent activity
    void MaybeSendHeartbeat();

    /// Generate unique instance ID
    static std::string GenerateInstanceId();

    /// Load persisted state (if configured)
    void LoadPersistedState();

    /// Save state to disk (if configured)
    void PersistState();

    /// Update statistics
    void UpdateStats(uint64_t bytes_sent, bool is_manifest);
};

}  // namespace ifex::reference
