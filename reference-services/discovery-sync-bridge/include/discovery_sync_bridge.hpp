/**
 * @file discovery_sync_bridge.hpp
 * @brief Bridge service for synchronizing Discovery state to cloud
 *
 * The DiscoverySyncBridge monitors the Discovery service and publishes
 * state changes to the cloud via Backend Transport. It maintains sync
 * state to minimize traffic by only sending deltas after initial full sync.
 *
 * Design principles:
 * - Initialization delay: Wait for system to stabilize before syncing
 * - Delta sync: Track synced state, only publish changes
 * - Sequence numbers: Enable cloud to detect missed events
 * - State checksum: Enable verification without full resync
 * - Batching: Combine multiple events to reduce MQTT messages
 */

#pragma once

#include "backend_transport_client.hpp"
#include "discovery-sync-envelope.pb.h"
#include "service-discovery-service.grpc.pb.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>

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
 * @brief Cached state of a synced service
 */
struct SyncedServiceState {
    std::string registration_id;
    std::string name;
    std::string version;
    std::string address;
    swdv::discovery_sync_envelope::service_status_t status;
    uint64_t last_heartbeat_ms = 0;
    uint64_t last_synced_sequence = 0;

    /// Compute hash for change detection
    uint64_t ComputeHash() const;
};

/**
 * @brief Statistics for monitoring sync bridge health
 */
struct DiscoverySyncStats {
    uint64_t events_sent = 0;
    uint64_t full_syncs_sent = 0;
    uint64_t delta_syncs_sent = 0;
    uint64_t heartbeats_sent = 0;
    uint64_t bytes_sent = 0;
    uint64_t services_tracked = 0;
    uint64_t last_sync_timestamp_ns = 0;
    uint64_t current_sequence = 0;
    bool is_initialized = false;
    bool is_connected = false;
};

/**
 * @brief Bridge for synchronizing Discovery state to cloud
 *
 * Lifecycle:
 * 1. Start() - begins initialization phase
 * 2. After initialization_delay_ms, captures initial state
 * 3. Publishes FULL_SYNC event with all services
 * 4. Polls Discovery at poll_interval_ms
 * 5. Publishes delta events for any changes
 * 6. Stop() - graceful shutdown
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
     * @brief Force a full sync (for testing or recovery)
     */
    void ForceFullSync();

    /**
     * @brief Get current state checksum
     */
    uint32_t GetStateChecksum() const;

private:
    /// Configuration
    DiscoverySyncBridgeConfig config_;

    /// Backend Transport client for publishing
    std::unique_ptr<client::BackendTransportClient> transport_client_;

    /// gRPC channel to Discovery service
    std::shared_ptr<grpc::Channel> discovery_channel_;

    /// Discovery service stub
    std::unique_ptr<swdv::service_discovery::query_services_service::Stub> query_stub_;

    /// Cached sync state (registration_id -> state)
    std::unordered_map<std::string, SyncedServiceState> synced_state_;
    mutable std::mutex state_mutex_;

    /// Pending events to batch
    std::vector<swdv::discovery_sync_envelope::sync_event_t> pending_events_;
    mutable std::mutex events_mutex_;

    /// Statistics
    mutable DiscoverySyncStats stats_;
    mutable std::mutex stats_mutex_;

    /// Monotonic sequence number
    std::atomic<uint64_t> sequence_number_{0};

    /// Unique instance ID (for restart detection)
    std::string instance_id_;

    /// Running state
    std::atomic<bool> running_{false};
    std::atomic<bool> initialized_{false};
    std::atomic<bool> stop_requested_{false};

    /// Worker threads
    std::thread poll_thread_;
    std::thread batch_thread_;

    /// Condition variable for signaling
    std::condition_variable cv_;
    std::mutex cv_mutex_;

    /// Last heartbeat time
    std::chrono::steady_clock::time_point last_activity_time_;

    // Internal methods

    /// Main polling loop
    void PollLoop();

    /// Batch sending loop
    void BatchLoop();

    /// Query Discovery for current services
    std::vector<SyncedServiceState> QueryDiscoveryServices();

    /// Compare current state with synced state, generate events
    void DetectChanges(const std::vector<SyncedServiceState>& current);

    /// Queue an event for sending
    void QueueEvent(swdv::discovery_sync_envelope::sync_event_t event);

    /// Send queued events
    void FlushEvents();

    /// Send a full sync message
    void SendFullSync(const std::vector<SyncedServiceState>& services);

    /// Send heartbeat if no recent activity
    void MaybeSendHeartbeat();

    /// Build service_info_t from cached state
    swdv::discovery_sync_envelope::service_info_t BuildServiceInfo(
        const SyncedServiceState& state);

    /// Compute CRC32 checksum of current state
    uint32_t ComputeStateChecksum() const;

    /// Generate unique instance ID
    static std::string GenerateInstanceId();

    /// Load persisted state (if configured)
    void LoadPersistedState();

    /// Save state to disk (if configured)
    void PersistState();

    /// Update statistics
    void UpdateStats(uint64_t bytes_sent, bool is_full_sync);
};

}  // namespace ifex::reference
