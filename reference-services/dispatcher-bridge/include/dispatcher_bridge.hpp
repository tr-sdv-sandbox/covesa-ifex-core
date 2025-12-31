/**
 * @file dispatcher_bridge.hpp
 * @brief Bridge between Backend Transport and Dispatcher for cloud RPC forwarding
 *
 * The DispatcherBridge enables cloud applications to invoke any onboard IFEX service
 * by forwarding RPC requests received via Backend Transport to the local Dispatcher.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace grpc {
class Channel;
}

namespace ifex::reference {

/// Configuration for DispatcherBridge
struct DispatcherBridgeConfig {
    /// Dispatcher service endpoint (e.g., "localhost:50052")
    std::string dispatcher_endpoint = "localhost:50052";

    /// Backend Transport service endpoint (e.g., "localhost:50060")
    std::string backend_transport_endpoint = "localhost:50060";

    /// Content ID for RPC messages (default: DISPATCHER_RPC from ifex_content_ids.hpp)
    uint32_t rpc_content_id = 200;

    /// Maximum concurrent pending requests
    uint32_t max_concurrent_requests = 100;

    /// Default timeout for requests without explicit timeout (ms)
    uint32_t default_timeout_ms = 30000;

    /// Interval for checking timed-out requests (ms)
    uint32_t timeout_check_interval_ms = 1000;

    /// Number of worker threads for processing requests
    uint32_t num_workers = 4;
};

/**
 * @brief Bridge between Backend Transport and Dispatcher
 *
 * Receives RPC requests from cloud via Backend Transport, forwards them to the
 * local Dispatcher service, and sends responses back via Backend Transport.
 *
 * Request flow:
 * 1. Cloud publishes rpc_request_t to c2v/{vehicle_id}/{content_id}
 * 2. Backend Transport delivers to DispatcherBridge via on_content
 * 3. DispatcherBridge decodes envelope, validates request
 * 4. DispatcherBridge calls Dispatcher.call_method()
 * 5. DispatcherBridge encodes rpc_response_t, publishes via Backend Transport
 * 6. Response delivered to cloud via v2c/{vehicle_id}/{content_id}
 *
 * Thread safety:
 * - All public methods are thread-safe
 * - Request handlers run in worker threads
 * - Statistics use atomic operations
 */
class DispatcherBridge {
public:
    explicit DispatcherBridge(const DispatcherBridgeConfig& config);
    ~DispatcherBridge();

    // Non-copyable, non-movable
    DispatcherBridge(const DispatcherBridge&) = delete;
    DispatcherBridge& operator=(const DispatcherBridge&) = delete;

    /**
     * @brief Start the bridge
     *
     * Connects to Backend Transport and Dispatcher, starts processing requests.
     *
     * @return true if started successfully
     */
    bool Start();

    /**
     * @brief Stop the bridge gracefully
     *
     * Stops accepting new requests, waits for pending requests to complete
     * (with timeout), then shuts down.
     */
    void Stop();

    /**
     * @brief Check if bridge is running and healthy
     */
    bool IsRunning() const;

    /**
     * @brief Check if bridge is healthy (connected to both services)
     */
    bool IsHealthy() const;

    /// Statistics for monitoring
    struct Stats {
        uint64_t requests_received = 0;    ///< Total requests received
        uint64_t requests_completed = 0;   ///< Successfully completed
        uint64_t requests_failed = 0;      ///< Failed (service errors)
        uint64_t requests_timed_out = 0;   ///< Timed out
        uint64_t requests_rejected = 0;    ///< Rejected (duplicate, limit, etc.)
        uint32_t pending_count = 0;        ///< Currently pending
    };

    /**
     * @brief Get current statistics
     */
    Stats GetStats() const;

private:
    /// Pending request metadata
    struct PendingRequest {
        std::string correlation_id;
        std::chrono::steady_clock::time_point start_time;
        uint32_t timeout_ms;
        std::atomic<bool> completed{false};
    };

    /// Handle incoming RPC request from Backend Transport
    void HandleIncomingRequest(const std::vector<uint8_t>& payload);

    /// Execute request via Dispatcher (runs in worker thread)
    void ExecuteRequest(std::shared_ptr<PendingRequest> pending,
                       const std::string& service_name,
                       const std::string& method_name,
                       const std::string& parameters_json,
                       int64_t request_timestamp_ns);

    /// Send RPC response back via Backend Transport
    void SendResponse(const std::string& correlation_id,
                     uint8_t status,
                     const std::string& result_json,
                     const std::string& error_message,
                     uint32_t duration_ms,
                     const std::string& service_endpoint);

    /// Background thread for timeout checking
    void TimeoutCheckerLoop();

    /// Map Dispatcher call_status_t to rpc_status_t
    static uint8_t MapDispatcherStatus(int dispatcher_status);

    // Configuration
    DispatcherBridgeConfig config_;

    // gRPC channels and stubs
    std::shared_ptr<grpc::Channel> dispatcher_channel_;

    // Backend Transport client (pimpl to avoid header dependency)
    class TransportClientWrapper;
    std::unique_ptr<TransportClientWrapper> transport_;

    // Pending requests
    std::unordered_map<std::string, std::shared_ptr<PendingRequest>> pending_requests_;
    mutable std::mutex pending_mutex_;

    // Worker threads
    std::vector<std::thread> workers_;
    std::atomic<bool> running_{false};

    // Timeout checker thread
    std::thread timeout_thread_;

    // Statistics (atomic for thread-safety)
    std::atomic<uint64_t> requests_received_{0};
    std::atomic<uint64_t> requests_completed_{0};
    std::atomic<uint64_t> requests_failed_{0};
    std::atomic<uint64_t> requests_timed_out_{0};
    std::atomic<uint64_t> requests_rejected_{0};
};

}  // namespace ifex::reference
