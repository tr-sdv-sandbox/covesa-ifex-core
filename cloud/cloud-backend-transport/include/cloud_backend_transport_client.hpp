#pragma once

#include "cloud-backend-transport-service.grpc.pb.h"

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace ifex::cloud {

/// gRPC client for CloudBackendTransportServer.
///
/// Provides typed API for sending messages to vehicles and subscribing
/// to vehicle messages, status changes, and delivery acknowledgments.
class CloudBackendTransportClient {
public:
    /// Callback types for event streams
    using VehicleMessageCallback = std::function<void(
        const std::string& vehicle_id,
        const std::vector<uint8_t>& payload,
        uint64_t sequence,
        int64_t timestamp_ms)>;

    using VehicleStatusCallback = std::function<void(
        const std::string& vehicle_id,
        swdv::cloud_backend_transport_service::vehicle_status_t status,
        int64_t timestamp_ms)>;

    using AckCallback = std::function<void(
        const std::string& vehicle_id,
        uint64_t sequence)>;

    using QueueStatusCallback = std::function<void(
        const std::string& vehicle_id,
        swdv::cloud_backend_transport_service::queue_level_t level,
        uint32_t queue_size,
        uint32_t queue_capacity)>;

    /// Create client connected to specified server address.
    /// @param server_address gRPC address (e.g., "localhost:50100")
    explicit CloudBackendTransportClient(const std::string& server_address);

    ~CloudBackendTransportClient();

    // Non-copyable
    CloudBackendTransportClient(const CloudBackendTransportClient&) = delete;
    CloudBackendTransportClient& operator=(const CloudBackendTransportClient&) = delete;

    // =========================================================================
    // Methods
    // =========================================================================

    /// Send message to a vehicle.
    /// @param vehicle_id Target vehicle identifier
    /// @param payload Binary payload to send
    /// @param persistence Delivery guarantee level
    /// @return Send result with sequence number and status
    swdv::cloud_backend_transport_service::send_response_t SendToVehicle(
        const std::string& vehicle_id,
        const std::vector<uint8_t>& payload,
        swdv::cloud_backend_transport_service::persistence_t persistence =
            swdv::cloud_backend_transport_service::persistence_t::BEST_EFFORT);

    /// Get current status of a vehicle.
    /// @param vehicle_id Vehicle to query
    /// @return Vehicle status (UNKNOWN, ONLINE, OFFLINE) and last seen timestamp
    std::pair<swdv::cloud_backend_transport_service::vehicle_status_t, int64_t>
    GetVehicleStatus(const std::string& vehicle_id);

    /// Get channel binding info.
    /// @return content_id, partition_id, total_partitions
    swdv::cloud_backend_transport_service::channel_info_t GetChannelInfo();

    /// Get outbound queue status for a vehicle.
    /// @param vehicle_id Vehicle to query
    /// @return Queue status
    swdv::cloud_backend_transport_service::queue_status_t GetQueueStatus(const std::string& vehicle_id);

    /// Get transport statistics.
    /// @return Statistics for this partition
    swdv::cloud_backend_transport_service::transport_stats_t GetStats();

    /// Check if transport is healthy.
    /// @return true if connected and ready
    bool IsHealthy();

    // =========================================================================
    // Event Subscriptions
    // =========================================================================

    /// Subscribe to vehicle messages.
    /// Callback is invoked for each message received from vehicles.
    /// @param callback Function to call for each message
    void SubscribeToVehicleMessages(VehicleMessageCallback callback);

    /// Subscribe to vehicle status changes.
    /// @param callback Function to call for status changes
    void SubscribeToVehicleStatus(VehicleStatusCallback callback);

    /// Subscribe to delivery acknowledgments.
    /// @param callback Function to call for each ack
    void SubscribeToAcks(AckCallback callback);

    /// Subscribe to queue status changes.
    /// @param callback Function to call for queue level changes
    void SubscribeToQueueStatus(QueueStatusCallback callback);

    /// Stop all subscriptions.
    void StopSubscriptions();

private:
    std::shared_ptr<grpc::Channel> channel_;

    // Stubs for each service
    std::unique_ptr<swdv::cloud_backend_transport_service::send_to_vehicle_service::Stub> send_stub_;
    std::unique_ptr<swdv::cloud_backend_transport_service::get_vehicle_status_service::Stub> status_stub_;
    std::unique_ptr<swdv::cloud_backend_transport_service::get_channel_info_service::Stub> channel_stub_;
    std::unique_ptr<swdv::cloud_backend_transport_service::get_queue_status_service::Stub> queue_stub_;
    std::unique_ptr<swdv::cloud_backend_transport_service::get_stats_service::Stub> stats_stub_;
    std::unique_ptr<swdv::cloud_backend_transport_service::healthy_service::Stub> health_stub_;
    std::unique_ptr<swdv::cloud_backend_transport_service::on_vehicle_message_service::Stub> msg_event_stub_;
    std::unique_ptr<swdv::cloud_backend_transport_service::on_ack_service::Stub> ack_event_stub_;
    std::unique_ptr<swdv::cloud_backend_transport_service::on_vehicle_status_service::Stub> status_event_stub_;
    std::unique_ptr<swdv::cloud_backend_transport_service::on_queue_status_changed_service::Stub> queue_event_stub_;

    // Subscription threads
    std::atomic<bool> running_{true};
    std::thread message_thread_;
    std::thread status_thread_;
    std::thread ack_thread_;
    std::thread queue_thread_;

    // Cancellation contexts
    std::unique_ptr<grpc::ClientContext> message_context_;
    std::unique_ptr<grpc::ClientContext> status_context_;
    std::unique_ptr<grpc::ClientContext> ack_context_;
    std::unique_ptr<grpc::ClientContext> queue_context_;
    std::mutex context_mutex_;
};

}  // namespace ifex::cloud
