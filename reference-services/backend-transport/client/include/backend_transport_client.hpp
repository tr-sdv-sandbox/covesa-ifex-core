/**
 * @file backend_transport_client.hpp
 * @brief Client library for IFEX Backend Transport Service
 *
 * Provides a simple C++ API for vehicle-to-cloud and cloud-to-vehicle messaging.
 * Each client instance handles one content_id. Create multiple instances for
 * multiple content_ids (they can share the same gRPC channel).
 */

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// Forward declarations to avoid exposing gRPC in header
namespace grpc {
class Channel;
}

namespace ifex::client {

/// Message persistence level
enum class Persistence : uint8_t {
    None = 0,           ///< Fire and forget - can be dropped
    UntilDelivered = 1, ///< Retry until ack
    UntilRestart = 2,   ///< Keep until service restart
    Persistent = 3      ///< Survives power cycles
};

/// Publish operation result
enum class PublishStatus : uint8_t {
    Ok = 0,
    BufferFull = 1,
    MessageTooLong = 2,
    NotConnected = 3,
    Timeout = 4,
    Error = 255
};

/// Connection state
enum class ConnectionState : uint8_t {
    Unknown = 0,
    Connected = 1,
    Disconnected = 2,
    Connecting = 3,
    Reconnecting = 4
};

/// Connection status
struct ConnectionStatus {
    ConnectionState state = ConnectionState::Unknown;
    std::string reason;
    int64_t timestamp_ns = 0;
};

/// Queue status
struct QueueStatus {
    bool is_full = false;
    uint32_t queue_size = 0;
    uint32_t queue_capacity = 0;
};

/// Transport statistics
struct TransportStats {
    uint64_t messages_sent = 0;
    uint64_t messages_failed = 0;
    uint64_t bytes_sent = 0;
    uint64_t messages_received = 0;
    uint64_t bytes_received = 0;
    int64_t last_send_timestamp_ns = 0;
    int64_t last_receive_timestamp_ns = 0;
};

/// Result of a publish operation
struct PublishResult {
    uint64_t sequence = 0;      ///< Assigned sequence (0 on failure)
    PublishStatus status = PublishStatus::Error;

    bool ok() const { return status == PublishStatus::Ok; }
    operator bool() const { return ok(); }
};

/**
 * @brief Client for IFEX Backend Transport Service
 *
 * Each client instance is bound to a single content_id. This simplifies the
 * common case where a component sends/receives one type of content.
 *
 * For multiple content_ids, create multiple client instances sharing the
 * same gRPC channel.
 *
 * Thread safety:
 * - publish() is thread-safe
 * - Callbacks are invoked from background threads
 * - Status methods are thread-safe
 *
 * Example:
 * @code
 * auto channel = grpc::CreateChannel("localhost:50051", grpc::InsecureChannelCredentials());
 *
 * // Telemetry sender
 * BackendTransportClient telemetry(channel, 42);
 * auto result = telemetry.publish({0x01, 0x02, 0x03});
 * if (result) {
 *     std::cout << "Sent with sequence " << result.sequence << "\n";
 * }
 *
 * // Command receiver
 * BackendTransportClient commands(channel, 100);
 * commands.on_content([](const std::vector<uint8_t>& payload) {
 *     handle_command(payload);
 * });
 *
 * // Track delivery confirmations
 * telemetry.on_ack([](uint64_t seq) {
 *     std::cout << "Delivered: " << seq << "\n";
 * });
 * @endcode
 */
class BackendTransportClient {
public:
    /// Callback for received content (c2v)
    using ContentCallback = std::function<void(const std::vector<uint8_t>& payload)>;

    /// Callback for delivery acknowledgments
    using AckCallback = std::function<void(uint64_t sequence)>;

    /// Callback for connection status changes
    using ConnectionCallback = std::function<void(const ConnectionStatus& status)>;

    /**
     * @brief Create a client for a specific content_id
     * @param channel gRPC channel to the backend transport service
     * @param content_id Content identifier this client handles
     */
    BackendTransportClient(std::shared_ptr<grpc::Channel> channel, uint32_t content_id);

    ~BackendTransportClient();

    // Non-copyable, movable
    BackendTransportClient(const BackendTransportClient&) = delete;
    BackendTransportClient& operator=(const BackendTransportClient&) = delete;
    BackendTransportClient(BackendTransportClient&&) noexcept;
    BackendTransportClient& operator=(BackendTransportClient&&) noexcept;

    /// Get the content_id this client handles
    uint32_t content_id() const;

    // =========================================================================
    // Publishing (v2c)
    // =========================================================================

    /**
     * @brief Publish data to the cloud
     * @param payload Binary data to send
     * @param persistence Delivery guarantee level
     * @return Result with assigned sequence number, or error status
     */
    PublishResult publish(const std::vector<uint8_t>& payload,
                         Persistence persistence = Persistence::None);

    /**
     * @brief Publish data to the cloud (move version)
     */
    PublishResult publish(std::vector<uint8_t>&& payload,
                         Persistence persistence = Persistence::None);

    // =========================================================================
    // Subscriptions (streaming)
    // =========================================================================

    /**
     * @brief Subscribe to content from the cloud (c2v)
     *
     * Starts a background thread that receives messages and invokes the callback.
     * Only one content subscription can be active at a time.
     *
     * @param callback Called for each received message
     */
    void on_content(ContentCallback callback);

    /**
     * @brief Subscribe to delivery acknowledgments
     *
     * Receive notifications when published messages are delivered to the cloud.
     * Gaps in sequence numbers indicate dropped messages (per persistence policy).
     *
     * @param callback Called for each delivered message with its sequence number
     */
    void on_ack(AckCallback callback);

    /**
     * @brief Subscribe to connection status changes
     *
     * Receive notifications when the transport connection state changes.
     *
     * @param callback Called on each status change
     */
    void on_connection_changed(ConnectionCallback callback);

    /**
     * @brief Stop all subscriptions
     */
    void unsubscribe_all();

    // =========================================================================
    // Status queries
    // =========================================================================

    /**
     * @brief Check if transport is healthy (connected)
     */
    bool healthy();

    /**
     * @brief Get current connection status
     */
    ConnectionStatus connection_status();

    /**
     * @brief Get queue status
     */
    QueueStatus queue_status();

    /**
     * @brief Get transport statistics
     */
    TransportStats stats();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ifex::client
