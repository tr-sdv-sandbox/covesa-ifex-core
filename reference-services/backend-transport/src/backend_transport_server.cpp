#include "backend_transport_server.hpp"

#include <glog/logging.h>
#include <grpcpp/grpcpp.h>

#include <chrono>
#include <regex>

namespace ifex::reference {

BackendTransportServer::BackendTransportServer(const Config& config) : config_(config) {
    // Initialize MQTT client
    MqttClient::Config mqtt_config;
    mqtt_config.host = config_.mqtt_host;
    mqtt_config.port = config_.mqtt_port;
    mqtt_config.username = config_.mqtt_username;
    mqtt_config.password = config_.mqtt_password;
    mqtt_config.client_id = "ifex-backend-transport-" + config_.vehicle_id;

    mqtt_client_ = std::make_unique<MqttClient>(mqtt_config);

    // Set MQTT callbacks
    mqtt_client_->OnConnect([this]() { OnMqttConnected(); });
    mqtt_client_->OnDisconnect([this](int reason) { OnMqttDisconnected(reason); });
    mqtt_client_->OnMessage([this](const std::string& topic, const std::vector<uint8_t>& payload) {
        OnMqttMessage(topic, payload);
    });

    // Initialize message queue manager
    MessageQueueManager::Config queue_config;
    queue_config.default_queue_size = config_.queue_size_per_content_id;
    queue_config.persistence_dir = config_.persistence_dir;

    queue_manager_ = std::make_unique<MessageQueueManager>(queue_config);
}

BackendTransportServer::~BackendTransportServer() {
    Stop();
}

bool BackendTransportServer::Start() {
    LOG(INFO) << "Starting Backend Transport Service...";

    // Connect to MQTT
    if (!mqtt_client_->Connect()) {
        LOG(ERROR) << "Failed to connect to MQTT broker";
        return false;
    }

    // Start queue manager
    queue_manager_->Start(
        [this](uint32_t content_id, const std::vector<uint8_t>& payload) {
            return SendToMqtt(content_id, payload);
        },
        [this]() { return mqtt_client_->IsConnected(); }
    );

    LOG(INFO) << "Backend Transport Service started";
    return true;
}

void BackendTransportServer::Stop() {
    LOG(INFO) << "Stopping Backend Transport Service...";

    // Stop queue manager (persists messages)
    if (queue_manager_) {
        queue_manager_->Stop();
    }

    // Disconnect MQTT
    if (mqtt_client_) {
        mqtt_client_->Disconnect();
    }

    LOG(INFO) << "Backend Transport Service stopped";
}

bool BackendTransportServer::RegisterWithDiscovery(int port, const std::string& ifex_schema) {
    // TODO: Implement service discovery registration
    LOG(INFO) << "Service running on port " << port;
    return true;
}

// =============================================================================
// gRPC Method Implementations
// =============================================================================

grpc::Status BackendTransportServer::publish(
    grpc::ServerContext* context,
    const swdv::backend_transport_service::publish_request* request,
    swdv::backend_transport_service::publish_response* response) {

    const auto& req = request->request();
    uint32_t content_id = req.content_id();
    const std::string& payload_str = req.payload();
    std::vector<uint8_t> payload(payload_str.begin(), payload_str.end());

    // Map protobuf persistence to internal enum
    Persistence persistence = static_cast<Persistence>(req.persistence());

    auto* result = response->mutable_result();

    // Enqueue message - sequence is assigned atomically inside
    uint64_t sequence = queue_manager_->Enqueue(content_id, std::move(payload), persistence);

    if (sequence > 0) {
        result->set_sequence(sequence);
        result->set_status(swdv::backend_transport_service::OK);
        // Stats (messages_sent_, bytes_sent_) are updated when actually sent over MQTT
    } else {
        result->set_sequence(0);
        result->set_status(swdv::backend_transport_service::BUFFER_FULL);
        messages_failed_++;
    }

    return grpc::Status::OK;
}

grpc::Status BackendTransportServer::get_connection_status(
    grpc::ServerContext* context,
    const swdv::backend_transport_service::get_connection_status_request* request,
    swdv::backend_transport_service::get_connection_status_response* response) {

    auto* status = response->mutable_status();
    status->set_state(CurrentConnectionState());
    status->set_reason(disconnect_reason_);
    status->set_timestamp_ns(last_status_change_ns_.load());

    return grpc::Status::OK;
}

grpc::Status BackendTransportServer::get_queue_status(
    grpc::ServerContext* context,
    const swdv::backend_transport_service::get_queue_status_request* request,
    swdv::backend_transport_service::get_queue_status_response* response) {

    auto stats = queue_manager_->GetStats();

    auto* status = response->mutable_status();
    status->set_is_full(stats.queues_full > 0);
    status->set_queue_size(static_cast<uint32_t>(stats.total_queued));
    status->set_queue_capacity(static_cast<uint32_t>(config_.queue_size_per_content_id));

    return grpc::Status::OK;
}

grpc::Status BackendTransportServer::get_stats(
    grpc::ServerContext* context,
    const swdv::backend_transport_service::get_stats_request* request,
    swdv::backend_transport_service::get_stats_response* response) {

    auto* stats = response->mutable_stats();
    stats->set_messages_sent(messages_sent_.load());
    stats->set_messages_failed(messages_failed_.load());
    stats->set_bytes_sent(bytes_sent_.load());
    stats->set_messages_received(messages_received_.load());
    stats->set_bytes_received(bytes_received_.load());
    stats->set_last_send_timestamp_ns(last_send_timestamp_ns_.load());
    stats->set_last_receive_timestamp_ns(last_receive_timestamp_ns_.load());

    return grpc::Status::OK;
}

grpc::Status BackendTransportServer::healthy(
    grpc::ServerContext* context,
    const swdv::backend_transport_service::healthy_request* request,
    swdv::backend_transport_service::healthy_response* response) {

    response->set_is_healthy(connected_.load());
    return grpc::Status::OK;
}

// =============================================================================
// gRPC Streaming Event Implementations
// =============================================================================

grpc::Status BackendTransportServer::subscribe(
    grpc::ServerContext* context,
    const swdv::backend_transport_service::on_content_subscribe_request* request,
    grpc::ServerWriter<swdv::backend_transport_service::on_content>* writer) {

    // Extract content_ids from request
    std::unordered_set<uint32_t> content_ids;
    for (int i = 0; i < request->content_ids_size(); ++i) {
        content_ids.insert(request->content_ids(i));
    }

    if (content_ids.empty()) {
        LOG(WARNING) << "Client tried to subscribe with empty content_ids";
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                           "content_ids must not be empty");
    }

    LOG(INFO) << "Client subscribed to content stream for " << content_ids.size() << " content_id(s)";

    // Subscribe to MQTT topics for each content_id
    for (uint32_t content_id : content_ids) {
        std::string topic = config_.c2v_prefix + "/" + config_.vehicle_id + "/" + std::to_string(content_id);

        // Track global subscriptions
        {
            std::unique_lock<std::shared_mutex> lock(subscriptions_mutex_);
            subscribed_content_ids_.insert(content_id);
        }

        // Subscribe on MQTT if not already subscribed
        mqtt_client_->Subscribe(topic);
        LOG(INFO) << "Subscribed to MQTT topic: " << topic;
    }

    AddContentStream(writer, content_ids);

    // Keep stream open until client disconnects
    while (!context->IsCancelled()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    RemoveContentStream(writer);

    LOG(INFO) << "Client unsubscribed from content stream";
    return grpc::Status::OK;
}

grpc::Status BackendTransportServer::subscribe(
    grpc::ServerContext* context,
    const swdv::backend_transport_service::on_ack_subscribe_request* request,
    grpc::ServerWriter<swdv::backend_transport_service::on_ack>* writer) {

    // Extract content_ids from request
    std::unordered_set<uint32_t> content_ids;
    for (int i = 0; i < request->content_ids_size(); ++i) {
        content_ids.insert(request->content_ids(i));
    }

    if (content_ids.empty()) {
        LOG(WARNING) << "Client tried to subscribe to acks with empty content_ids";
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                           "content_ids must not be empty");
    }

    LOG(INFO) << "Client subscribed to ack stream for " << content_ids.size() << " content_id(s)";

    AddAckStream(writer, content_ids);

    // Keep stream open until client disconnects
    while (!context->IsCancelled()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    RemoveAckStream(writer);

    LOG(INFO) << "Client unsubscribed from ack stream";
    return grpc::Status::OK;
}

grpc::Status BackendTransportServer::subscribe(
    grpc::ServerContext* context,
    const swdv::backend_transport_service::on_connection_changed_subscribe_request* request,
    grpc::ServerWriter<swdv::backend_transport_service::on_connection_changed>* writer) {

    LOG(INFO) << "Client subscribed to connection status stream";

    AddConnectionStream(writer);

    // Send current status immediately
    swdv::backend_transport_service::on_connection_changed event;
    auto* status = event.mutable_status();
    status->set_state(CurrentConnectionState());
    status->set_reason(disconnect_reason_);
    status->set_timestamp_ns(last_status_change_ns_.load());
    writer->Write(event);

    // Keep stream open until client disconnects
    while (!context->IsCancelled()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    RemoveConnectionStream(writer);

    LOG(INFO) << "Client unsubscribed from connection status stream";
    return grpc::Status::OK;
}

grpc::Status BackendTransportServer::subscribe(
    grpc::ServerContext* context,
    const swdv::backend_transport_service::on_queue_status_changed_subscribe_request* request,
    grpc::ServerWriter<swdv::backend_transport_service::on_queue_status_changed>* writer) {

    LOG(INFO) << "Client subscribed to queue status stream";

    AddQueueStream(writer);

    // Keep stream open until client disconnects
    while (!context->IsCancelled()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    RemoveQueueStream(writer);

    LOG(INFO) << "Client unsubscribed from queue status stream";
    return grpc::Status::OK;
}

// =============================================================================
// Private Methods
// =============================================================================

void BackendTransportServer::OnMqttConnected() {
    LOG(INFO) << "MQTT connected";

    connected_ = true;
    disconnect_reason_.clear();
    last_status_change_ns_ = NowNs();

    // Resubscribe to all c2v topics
    {
        std::shared_lock<std::shared_mutex> lock(subscriptions_mutex_);
        for (uint32_t content_id : subscribed_content_ids_) {
            std::string topic = config_.c2v_prefix + "/" + config_.vehicle_id + "/" + std::to_string(content_id);
            mqtt_client_->Subscribe(topic);
        }
    }

    BroadcastConnectionStatus();
}

void BackendTransportServer::OnMqttDisconnected(int reason) {
    LOG(WARNING) << "MQTT disconnected (reason=" << reason << ")";

    connected_ = false;
    disconnect_reason_ = "Disconnected (code " + std::to_string(reason) + ")";
    last_status_change_ns_ = NowNs();

    BroadcastConnectionStatus();
}

void BackendTransportServer::OnMqttMessage(const std::string& topic, const std::vector<uint8_t>& payload) {
    uint32_t content_id = ExtractContentIdFromTopic(topic);

    if (content_id == 0) {
        LOG(WARNING) << "Could not extract content_id from topic: " << topic;
        return;
    }

    messages_received_++;
    bytes_received_ += payload.size();
    last_receive_timestamp_ns_ = NowNs();

    BroadcastContent(content_id, payload);
}

bool BackendTransportServer::SendToMqtt(uint32_t content_id, const std::vector<uint8_t>& payload) {
    std::string topic = V2cTopic(content_id);
    bool success = mqtt_client_->Publish(topic, payload, 1, false);
    if (success) {
        messages_sent_++;
        bytes_sent_ += payload.size();
        last_send_timestamp_ns_ = NowNs();
    }
    return success;
}

std::string BackendTransportServer::V2cTopic(uint32_t content_id) const {
    return config_.v2c_prefix + "/" + config_.vehicle_id + "/" + std::to_string(content_id);
}

std::string BackendTransportServer::C2vSubscribePattern() const {
    return config_.c2v_prefix + "/" + config_.vehicle_id + "/#";
}

uint32_t BackendTransportServer::ExtractContentIdFromTopic(const std::string& topic) const {
    // Topic format: c2v/{vehicle_id}/{content_id}
    std::string prefix = config_.c2v_prefix + "/" + config_.vehicle_id + "/";
    if (topic.rfind(prefix, 0) != 0) {
        return 0;
    }

    std::string content_id_str = topic.substr(prefix.size());
    try {
        return std::stoul(content_id_str);
    } catch (...) {
        return 0;
    }
}

void BackendTransportServer::AddContentStream(
    grpc::ServerWriter<swdv::backend_transport_service::on_content>* writer,
    const std::unordered_set<uint32_t>& content_ids) {
    std::unique_lock<std::shared_mutex> lock(content_streams_mutex_);
    content_streams_.push_back({writer, content_ids});
}

void BackendTransportServer::RemoveContentStream(
    grpc::ServerWriter<swdv::backend_transport_service::on_content>* writer) {
    std::unique_lock<std::shared_mutex> lock(content_streams_mutex_);
    content_streams_.erase(
        std::remove_if(content_streams_.begin(), content_streams_.end(),
                       [writer](const ContentStreamSubscription& sub) {
                           return sub.writer == writer;
                       }),
        content_streams_.end());
}

void BackendTransportServer::BroadcastContent(uint32_t content_id, const std::vector<uint8_t>& payload) {
    swdv::backend_transport_service::on_content event;
    auto* msg = event.mutable_message();
    msg->set_content_id(content_id);
    msg->set_payload(std::string(payload.begin(), payload.end()));

    std::shared_lock<std::shared_mutex> lock(content_streams_mutex_);
    for (const auto& sub : content_streams_) {
        // Only send to streams that subscribed to this content_id
        if (sub.content_ids.count(content_id) > 0) {
            sub.writer->Write(event);
        }
    }
}

void BackendTransportServer::AddConnectionStream(
    grpc::ServerWriter<swdv::backend_transport_service::on_connection_changed>* writer) {
    std::unique_lock<std::shared_mutex> lock(connection_streams_mutex_);
    connection_streams_.push_back(writer);
}

void BackendTransportServer::RemoveConnectionStream(
    grpc::ServerWriter<swdv::backend_transport_service::on_connection_changed>* writer) {
    std::unique_lock<std::shared_mutex> lock(connection_streams_mutex_);
    connection_streams_.erase(
        std::remove(connection_streams_.begin(), connection_streams_.end(), writer),
        connection_streams_.end());
}

void BackendTransportServer::BroadcastConnectionStatus() {
    swdv::backend_transport_service::on_connection_changed event;
    auto* status = event.mutable_status();
    status->set_state(CurrentConnectionState());
    status->set_reason(disconnect_reason_);
    status->set_timestamp_ns(last_status_change_ns_.load());

    std::shared_lock<std::shared_mutex> lock(connection_streams_mutex_);
    for (auto* writer : connection_streams_) {
        writer->Write(event);
    }
}

void BackendTransportServer::AddQueueStream(
    grpc::ServerWriter<swdv::backend_transport_service::on_queue_status_changed>* writer) {
    std::unique_lock<std::shared_mutex> lock(queue_streams_mutex_);
    queue_streams_.push_back(writer);
}

void BackendTransportServer::RemoveQueueStream(
    grpc::ServerWriter<swdv::backend_transport_service::on_queue_status_changed>* writer) {
    std::unique_lock<std::shared_mutex> lock(queue_streams_mutex_);
    queue_streams_.erase(
        std::remove(queue_streams_.begin(), queue_streams_.end(), writer),
        queue_streams_.end());
}

void BackendTransportServer::BroadcastQueueStatus() {
    auto stats = queue_manager_->GetStats();
    bool is_full = stats.queues_full > 0;

    // Only broadcast on change
    if (is_full == last_queue_full_) {
        return;
    }
    last_queue_full_ = is_full;

    swdv::backend_transport_service::on_queue_status_changed event;
    auto* status = event.mutable_status();
    status->set_is_full(is_full);
    status->set_queue_size(static_cast<uint32_t>(stats.total_queued));
    status->set_queue_capacity(static_cast<uint32_t>(config_.queue_size_per_content_id));

    std::shared_lock<std::shared_mutex> lock(queue_streams_mutex_);
    for (auto* writer : queue_streams_) {
        writer->Write(event);
    }
}

int64_t BackendTransportServer::NowNs() const {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

swdv::backend_transport_service::connection_state_t BackendTransportServer::CurrentConnectionState() const {
    if (connected_) {
        return swdv::backend_transport_service::CONNECTED;
    }
    return swdv::backend_transport_service::DISCONNECTED;
}

void BackendTransportServer::AddAckStream(
    grpc::ServerWriter<swdv::backend_transport_service::on_ack>* writer,
    const std::unordered_set<uint32_t>& content_ids) {
    std::unique_lock<std::shared_mutex> lock(ack_streams_mutex_);
    ack_streams_.push_back({writer, content_ids});
}

void BackendTransportServer::RemoveAckStream(
    grpc::ServerWriter<swdv::backend_transport_service::on_ack>* writer) {
    std::unique_lock<std::shared_mutex> lock(ack_streams_mutex_);
    ack_streams_.erase(
        std::remove_if(ack_streams_.begin(), ack_streams_.end(),
                       [writer](const AckStreamSubscription& sub) {
                           return sub.writer == writer;
                       }),
        ack_streams_.end());
}

void BackendTransportServer::BroadcastAck(uint32_t content_id, uint64_t sequence) {
    swdv::backend_transport_service::on_ack event;
    auto* ack = event.mutable_ack();
    ack->set_content_id(content_id);
    ack->set_sequence(sequence);

    std::shared_lock<std::shared_mutex> lock(ack_streams_mutex_);
    for (const auto& sub : ack_streams_) {
        // Only send to streams that subscribed to this content_id
        if (sub.content_ids.count(content_id) > 0) {
            sub.writer->Write(event);
        }
    }
}

}  // namespace ifex::reference
