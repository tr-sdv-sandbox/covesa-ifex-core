#pragma once

#include "message_queue.hpp"
#include "mqtt_client.hpp"
#include "backend-transport-service.grpc.pb.h"

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ifex::reference {

/// Backend Transport Service - gRPC frontend for MQTT backend
///
/// Handles 100+ concurrent gRPC clients sharing a single MQTT connection.
/// Features:
/// - Per-content-id message queues with ordering guarantees
/// - Configurable persistence (fire-and-forget to disk-persistent)
/// - Backpressure signaling when queues fill up
/// - Graceful shutdown with message persistence
/// - Server-streaming events for content, connection status, queue status
class BackendTransportServer final
    : public swdv::backend_transport_service::publish_service::Service,
      public swdv::backend_transport_service::get_connection_status_service::Service,
      public swdv::backend_transport_service::get_queue_status_service::Service,
      public swdv::backend_transport_service::get_stats_service::Service,
      public swdv::backend_transport_service::healthy_service::Service,
      public swdv::backend_transport_service::on_content_service::Service,
      public swdv::backend_transport_service::on_ack_service::Service,
      public swdv::backend_transport_service::on_connection_changed_service::Service,
      public swdv::backend_transport_service::on_queue_status_changed_service::Service {
public:
    struct Config {
        // MQTT settings
        std::string mqtt_host = "localhost";
        int mqtt_port = 1883;
        std::string mqtt_username;
        std::string mqtt_password;

        // Vehicle/topic settings
        std::string vehicle_id = "vehicle-001";
        std::string v2c_prefix = "v2c";
        std::string c2v_prefix = "c2v";

        // Queue settings
        size_t queue_size_per_content_id = 1000;
        std::string persistence_dir = "/var/lib/ifex/backend-transport";

        // Service discovery
        std::string discovery_endpoint;
    };

    explicit BackendTransportServer(const Config& config);
    ~BackendTransportServer();

    /// Start MQTT connection and message processing
    bool Start();

    /// Stop gracefully - persists pending messages
    void Stop();

    /// Register with IFEX service discovery
    bool RegisterWithDiscovery(int port, const std::string& ifex_schema);

    // =========================================================================
    // gRPC Method Implementations
    // =========================================================================

    // publish_service - v2c
    grpc::Status publish(
        grpc::ServerContext* context,
        const swdv::backend_transport_service::publish_request* request,
        swdv::backend_transport_service::publish_response* response) override;

    // get_connection_status_service
    grpc::Status get_connection_status(
        grpc::ServerContext* context,
        const swdv::backend_transport_service::get_connection_status_request* request,
        swdv::backend_transport_service::get_connection_status_response* response) override;

    // get_queue_status_service
    grpc::Status get_queue_status(
        grpc::ServerContext* context,
        const swdv::backend_transport_service::get_queue_status_request* request,
        swdv::backend_transport_service::get_queue_status_response* response) override;

    // get_stats_service
    grpc::Status get_stats(
        grpc::ServerContext* context,
        const swdv::backend_transport_service::get_stats_request* request,
        swdv::backend_transport_service::get_stats_response* response) override;

    // healthy_service
    grpc::Status healthy(
        grpc::ServerContext* context,
        const swdv::backend_transport_service::healthy_request* request,
        swdv::backend_transport_service::healthy_response* response) override;

    // =========================================================================
    // gRPC Streaming Event Implementations
    // =========================================================================

    // on_content_service - server streaming for c2v messages
    grpc::Status subscribe(
        grpc::ServerContext* context,
        const swdv::backend_transport_service::on_content_subscribe_request* request,
        grpc::ServerWriter<swdv::backend_transport_service::on_content>* writer) override;

    // on_ack_service - server streaming for delivery confirmations
    grpc::Status subscribe(
        grpc::ServerContext* context,
        const swdv::backend_transport_service::on_ack_subscribe_request* request,
        grpc::ServerWriter<swdv::backend_transport_service::on_ack>* writer) override;

    // on_connection_changed_service - server streaming
    grpc::Status subscribe(
        grpc::ServerContext* context,
        const swdv::backend_transport_service::on_connection_changed_subscribe_request* request,
        grpc::ServerWriter<swdv::backend_transport_service::on_connection_changed>* writer) override;

    // on_queue_status_changed_service - server streaming
    grpc::Status subscribe(
        grpc::ServerContext* context,
        const swdv::backend_transport_service::on_queue_status_changed_subscribe_request* request,
        grpc::ServerWriter<swdv::backend_transport_service::on_queue_status_changed>* writer) override;

private:
    // MQTT event handlers
    void OnMqttConnected();
    void OnMqttDisconnected(int reason);
    void OnMqttMessage(const std::string& topic, const std::vector<uint8_t>& payload);

    // Queue manager callback - actually send via MQTT
    bool SendToMqtt(uint32_t content_id, const std::vector<uint8_t>& payload);

    // Topic helpers
    std::string V2cTopic(uint32_t content_id) const;
    std::string C2vSubscribePattern() const;
    uint32_t ExtractContentIdFromTopic(const std::string& topic) const;

    // Stream management
    void AddContentStream(grpc::ServerWriter<swdv::backend_transport_service::on_content>* writer,
                          const std::unordered_set<uint32_t>& content_ids);
    void RemoveContentStream(grpc::ServerWriter<swdv::backend_transport_service::on_content>* writer);
    void BroadcastContent(uint32_t content_id, const std::vector<uint8_t>& payload);

    void AddConnectionStream(grpc::ServerWriter<swdv::backend_transport_service::on_connection_changed>* writer);
    void RemoveConnectionStream(grpc::ServerWriter<swdv::backend_transport_service::on_connection_changed>* writer);
    void BroadcastConnectionStatus();

    void AddQueueStream(grpc::ServerWriter<swdv::backend_transport_service::on_queue_status_changed>* writer);
    void RemoveQueueStream(grpc::ServerWriter<swdv::backend_transport_service::on_queue_status_changed>* writer);
    void BroadcastQueueStatus();

    // Ack stream management
    void AddAckStream(grpc::ServerWriter<swdv::backend_transport_service::on_ack>* writer,
                      const std::unordered_set<uint32_t>& content_ids);
    void RemoveAckStream(grpc::ServerWriter<swdv::backend_transport_service::on_ack>* writer);
    void BroadcastAck(uint32_t content_id, uint64_t sequence);

    // Helpers
    int64_t NowNs() const;
    swdv::backend_transport_service::connection_state_t CurrentConnectionState() const;

    Config config_;
    std::unique_ptr<MqttClient> mqtt_client_;
    std::unique_ptr<MessageQueueManager> queue_manager_;

    // Connection state
    std::atomic<bool> connected_{false};
    std::string disconnect_reason_;
    std::atomic<int64_t> last_status_change_ns_{0};

    // Statistics
    std::atomic<uint64_t> messages_sent_{0};
    std::atomic<uint64_t> messages_failed_{0};
    std::atomic<uint64_t> bytes_sent_{0};
    std::atomic<uint64_t> messages_received_{0};
    std::atomic<uint64_t> bytes_received_{0};
    std::atomic<int64_t> last_send_timestamp_ns_{0};
    std::atomic<int64_t> last_receive_timestamp_ns_{0};

    // C2V content subscriptions (which content_ids we're subscribed to)
    std::shared_mutex subscriptions_mutex_;
    std::unordered_set<uint32_t> subscribed_content_ids_;

    // Stream subscribers (gRPC clients listening for events)
    // Each content stream has an associated set of content_ids it's interested in
    struct ContentStreamSubscription {
        grpc::ServerWriter<swdv::backend_transport_service::on_content>* writer;
        std::unordered_set<uint32_t> content_ids;
    };
    std::shared_mutex content_streams_mutex_;
    std::vector<ContentStreamSubscription> content_streams_;

    std::shared_mutex connection_streams_mutex_;
    std::vector<grpc::ServerWriter<swdv::backend_transport_service::on_connection_changed>*> connection_streams_;

    std::shared_mutex queue_streams_mutex_;
    std::vector<grpc::ServerWriter<swdv::backend_transport_service::on_queue_status_changed>*> queue_streams_;

    // Ack stream subscribers
    struct AckStreamSubscription {
        grpc::ServerWriter<swdv::backend_transport_service::on_ack>* writer;
        std::unordered_set<uint32_t> content_ids;
    };
    std::shared_mutex ack_streams_mutex_;
    std::vector<AckStreamSubscription> ack_streams_;

    // Track last queue status to detect changes
    bool last_queue_full_ = false;
};

}  // namespace ifex::reference
