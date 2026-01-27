#pragma once

#include "cloud-backend-transport-service.grpc.pb.h"

#include <grpcpp/grpcpp.h>
#include <mosquitto.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ifex::cloud {

/// Simple MQTT-based cloud backend transport server for testing.
///
/// This is a reference implementation that:
/// - Connects directly to MQTT (no Kafka)
/// - Handles all vehicles (partition_id=0, total_partitions=1)
/// - Provides gRPC API for cloud service consumers
///
/// For production, use Kafka-based implementation in covesa-ifex-offboard-services.
class CloudBackendTransportServer final
    : public swdv::cloud_backend_transport_service::send_to_vehicle_service::Service,
      public swdv::cloud_backend_transport_service::get_vehicle_status_service::Service,
      public swdv::cloud_backend_transport_service::get_channel_info_service::Service,
      public swdv::cloud_backend_transport_service::get_queue_status_service::Service,
      public swdv::cloud_backend_transport_service::get_stats_service::Service,
      public swdv::cloud_backend_transport_service::list_vehicles_service::Service,
      public swdv::cloud_backend_transport_service::healthy_service::Service,
      public swdv::cloud_backend_transport_service::on_vehicle_message_service::Service,
      public swdv::cloud_backend_transport_service::on_ack_service::Service,
      public swdv::cloud_backend_transport_service::on_vehicle_status_service::Service,
      public swdv::cloud_backend_transport_service::on_queue_status_changed_service::Service {
public:
    struct Config {
        // MQTT settings
        std::string mqtt_host = "localhost";
        int mqtt_port = 1883;
        std::string mqtt_username;
        std::string mqtt_password;

        // Partitioning (for horizontal scaling)
        uint32_t partition_id = 0;
        uint32_t total_partitions = 1;

        // Topic prefixes
        std::string v2c_prefix = "v2c";
        std::string c2v_prefix = "c2v";
    };

    explicit CloudBackendTransportServer(const Config& config);
    ~CloudBackendTransportServer();

    // Non-copyable
    CloudBackendTransportServer(const CloudBackendTransportServer&) = delete;
    CloudBackendTransportServer& operator=(const CloudBackendTransportServer&) = delete;

    /// Start MQTT connection
    bool Start();

    /// Stop and disconnect
    void Stop();

    /// Check if connected to MQTT
    bool IsConnected() const { return connected_.load(); }

    // =========================================================================
    // gRPC Method Implementations
    // =========================================================================

    grpc::Status send_to_vehicle(
        grpc::ServerContext* context,
        const swdv::cloud_backend_transport_service::send_to_vehicle_request* request,
        swdv::cloud_backend_transport_service::send_to_vehicle_response* response) override;

    grpc::Status get_vehicle_status(
        grpc::ServerContext* context,
        const swdv::cloud_backend_transport_service::get_vehicle_status_request* request,
        swdv::cloud_backend_transport_service::get_vehicle_status_response* response) override;

    grpc::Status get_channel_info(
        grpc::ServerContext* context,
        const swdv::cloud_backend_transport_service::get_channel_info_request* request,
        swdv::cloud_backend_transport_service::get_channel_info_response* response) override;

    grpc::Status get_queue_status(
        grpc::ServerContext* context,
        const swdv::cloud_backend_transport_service::get_queue_status_request* request,
        swdv::cloud_backend_transport_service::get_queue_status_response* response) override;

    grpc::Status get_stats(
        grpc::ServerContext* context,
        const swdv::cloud_backend_transport_service::get_stats_request* request,
        swdv::cloud_backend_transport_service::get_stats_response* response) override;

    grpc::Status list_vehicles(
        grpc::ServerContext* context,
        const swdv::cloud_backend_transport_service::list_vehicles_request* request,
        swdv::cloud_backend_transport_service::list_vehicles_response* response) override;

    grpc::Status healthy(
        grpc::ServerContext* context,
        const swdv::cloud_backend_transport_service::healthy_request* request,
        swdv::cloud_backend_transport_service::healthy_response* response) override;

    // =========================================================================
    // gRPC Streaming Event Implementations
    // =========================================================================

    grpc::Status subscribe(
        grpc::ServerContext* context,
        const swdv::cloud_backend_transport_service::on_vehicle_message_subscribe_request* request,
        grpc::ServerWriter<swdv::cloud_backend_transport_service::on_vehicle_message>* writer) override;

    grpc::Status subscribe(
        grpc::ServerContext* context,
        const swdv::cloud_backend_transport_service::on_ack_subscribe_request* request,
        grpc::ServerWriter<swdv::cloud_backend_transport_service::on_ack>* writer) override;

    grpc::Status subscribe(
        grpc::ServerContext* context,
        const swdv::cloud_backend_transport_service::on_vehicle_status_subscribe_request* request,
        grpc::ServerWriter<swdv::cloud_backend_transport_service::on_vehicle_status>* writer) override;

    grpc::Status subscribe(
        grpc::ServerContext* context,
        const swdv::cloud_backend_transport_service::on_queue_status_changed_subscribe_request* request,
        grpc::ServerWriter<swdv::cloud_backend_transport_service::on_queue_status_changed>* writer) override;

private:
    // MQTT callbacks (static for C API)
    static void OnConnectCallback(struct mosquitto* mosq, void* userdata, int rc);
    static void OnDisconnectCallback(struct mosquitto* mosq, void* userdata, int rc);
    static void OnMessageCallback(struct mosquitto* mosq, void* userdata, const struct mosquitto_message* msg);

    // Instance methods
    void OnConnect(int rc);
    void OnDisconnect(int rc);
    void OnMessage(const std::string& topic, const std::vector<uint8_t>& payload);

    // Topic helpers
    std::string V2cSubscribePattern(uint32_t content_id) const;
    std::string C2vTopic(const std::string& vehicle_id, uint32_t content_id) const;
    std::string StatusSubscribePattern() const;
    bool ParseV2cTopic(const std::string& topic, std::string& vehicle_id, uint32_t& content_id) const;
    bool ParseStatusTopic(const std::string& topic, std::string& vehicle_id) const;

    // Partition check
    bool OwnsVehicle(const std::string& vehicle_id) const;

    // MQTT subscription management (on-demand)
    void SubscribeToContentId(uint32_t content_id);

    // Stream management
    void BroadcastVehicleMessage(const std::string& vehicle_id,
                                  const std::vector<uint8_t>& payload,
                                  uint64_t sequence,
                                  uint32_t content_id);
    void BroadcastVehicleStatus(const std::string& vehicle_id,
                                 swdv::cloud_backend_transport_service::vehicle_status_t status);
    void BroadcastAck(const std::string& vehicle_id, uint64_t sequence);

    // Helpers
    int64_t NowMs() const;
    uint64_t NextSequence(const std::string& vehicle_id);

    Config config_;
    struct mosquitto* mosq_ = nullptr;
    std::atomic<bool> connected_{false};
    std::atomic<bool> running_{false};

    // Per-vehicle state
    struct VehicleState {
        bool is_online = false;
        int64_t last_seen_ms = 0;
        uint64_t inbound_sequence = 0;   // From vehicle
        uint64_t outbound_sequence = 0;  // To vehicle
    };
    std::shared_mutex vehicles_mutex_;
    std::unordered_map<std::string, VehicleState> vehicles_;

    // Statistics
    std::atomic<uint64_t> messages_sent_{0};
    std::atomic<uint64_t> messages_failed_{0};
    std::atomic<uint64_t> bytes_sent_{0};
    std::atomic<uint64_t> messages_received_{0};
    std::atomic<uint64_t> bytes_received_{0};

    // Subscribed content_ids (for MQTT on-demand subscription)
    std::shared_mutex subscriptions_mutex_;
    std::set<uint32_t> subscribed_content_ids_;

    // Stream subscribers (message streams include content_id binding)
    struct MessageStreamSubscription {
        grpc::ServerWriter<swdv::cloud_backend_transport_service::on_vehicle_message>* writer;
        uint32_t content_id;
    };
    std::shared_mutex message_streams_mutex_;
    std::vector<MessageStreamSubscription> message_streams_;

    std::shared_mutex status_streams_mutex_;
    std::vector<grpc::ServerWriter<swdv::cloud_backend_transport_service::on_vehicle_status>*> status_streams_;

    std::shared_mutex ack_streams_mutex_;
    std::vector<grpc::ServerWriter<swdv::cloud_backend_transport_service::on_ack>*> ack_streams_;

    std::shared_mutex queue_streams_mutex_;
    std::vector<grpc::ServerWriter<swdv::cloud_backend_transport_service::on_queue_status_changed>*> queue_streams_;
};

}  // namespace ifex::cloud
