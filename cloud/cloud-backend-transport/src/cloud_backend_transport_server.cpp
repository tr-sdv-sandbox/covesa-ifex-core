#include "cloud_backend_transport_server.hpp"

#include <glog/logging.h>

#include <chrono>
#include <functional>
#include <thread>

namespace ifex::cloud {

namespace {
// Simple hash for partition assignment
uint32_t HashVehicleId(const std::string& vehicle_id) {
    uint32_t hash = 0;
    for (char c : vehicle_id) {
        hash = hash * 31 + static_cast<uint32_t>(c);
    }
    return hash;
}
}  // namespace

CloudBackendTransportServer::CloudBackendTransportServer(const Config& config)
    : config_(config) {
    mosquitto_lib_init();
    mosq_ = mosquitto_new(nullptr, true, this);
    if (!mosq_) {
        LOG(ERROR) << "Failed to create mosquitto client";
        return;
    }

    mosquitto_connect_callback_set(mosq_, OnConnectCallback);
    mosquitto_disconnect_callback_set(mosq_, OnDisconnectCallback);
    mosquitto_message_callback_set(mosq_, OnMessageCallback);

    if (!config_.mqtt_username.empty()) {
        mosquitto_username_pw_set(mosq_, config_.mqtt_username.c_str(),
                                   config_.mqtt_password.c_str());
    }
}

CloudBackendTransportServer::~CloudBackendTransportServer() {
    Stop();
    if (mosq_) {
        mosquitto_destroy(mosq_);
        mosq_ = nullptr;
    }
    mosquitto_lib_cleanup();
}

bool CloudBackendTransportServer::Start() {
    if (running_.load()) {
        return true;
    }

    LOG(INFO) << "Connecting to MQTT broker " << config_.mqtt_host << ":" << config_.mqtt_port;
    int rc = mosquitto_connect(mosq_, config_.mqtt_host.c_str(), config_.mqtt_port, 60);
    if (rc != MOSQ_ERR_SUCCESS) {
        LOG(ERROR) << "Failed to connect to MQTT: " << mosquitto_strerror(rc);
        return false;
    }

    running_.store(true);
    rc = mosquitto_loop_start(mosq_);
    if (rc != MOSQ_ERR_SUCCESS) {
        LOG(ERROR) << "Failed to start MQTT loop: " << mosquitto_strerror(rc);
        running_.store(false);
        return false;
    }

    LOG(INFO) << "CloudBackendTransportServer started for content_id=" << config_.content_id
              << " partition=" << config_.partition_id << "/" << config_.total_partitions;
    return true;
}

void CloudBackendTransportServer::Stop() {
    if (!running_.load()) {
        return;
    }
    running_.store(false);

    if (mosq_) {
        mosquitto_disconnect(mosq_);
        mosquitto_loop_stop(mosq_, true);
    }
    connected_.store(false);

    LOG(INFO) << "CloudBackendTransportServer stopped";
}

// =============================================================================
// MQTT Callbacks
// =============================================================================

void CloudBackendTransportServer::OnConnectCallback(struct mosquitto* /*mosq*/, void* userdata, int rc) {
    auto* self = static_cast<CloudBackendTransportServer*>(userdata);
    self->OnConnect(rc);
}

void CloudBackendTransportServer::OnDisconnectCallback(struct mosquitto* /*mosq*/, void* userdata, int rc) {
    auto* self = static_cast<CloudBackendTransportServer*>(userdata);
    self->OnDisconnect(rc);
}

void CloudBackendTransportServer::OnMessageCallback(struct mosquitto* /*mosq*/, void* userdata,
                                                     const struct mosquitto_message* msg) {
    auto* self = static_cast<CloudBackendTransportServer*>(userdata);
    std::vector<uint8_t> payload(static_cast<uint8_t*>(msg->payload),
                                  static_cast<uint8_t*>(msg->payload) + msg->payloadlen);
    self->OnMessage(msg->topic, payload);
}

void CloudBackendTransportServer::OnConnect(int rc) {
    if (rc != 0) {
        LOG(ERROR) << "MQTT connect failed: " << mosquitto_strerror(rc);
        return;
    }

    connected_.store(true);
    LOG(INFO) << "Connected to MQTT broker";

    // Subscribe to v2c messages for our content_id
    std::string v2c_pattern = V2cSubscribePattern();
    int sub_rc = mosquitto_subscribe(mosq_, nullptr, v2c_pattern.c_str(), 1);
    if (sub_rc != MOSQ_ERR_SUCCESS) {
        LOG(ERROR) << "Failed to subscribe to " << v2c_pattern << ": " << mosquitto_strerror(sub_rc);
    } else {
        LOG(INFO) << "Subscribed to " << v2c_pattern;
    }

    // Subscribe to status messages
    std::string status_pattern = StatusSubscribePattern();
    sub_rc = mosquitto_subscribe(mosq_, nullptr, status_pattern.c_str(), 1);
    if (sub_rc != MOSQ_ERR_SUCCESS) {
        LOG(ERROR) << "Failed to subscribe to " << status_pattern << ": " << mosquitto_strerror(sub_rc);
    } else {
        LOG(INFO) << "Subscribed to " << status_pattern;
    }
}

void CloudBackendTransportServer::OnDisconnect(int rc) {
    connected_.store(false);
    if (rc != 0) {
        LOG(WARNING) << "Unexpected MQTT disconnect: " << mosquitto_strerror(rc);
    } else {
        LOG(INFO) << "Disconnected from MQTT broker";
    }
}

void CloudBackendTransportServer::OnMessage(const std::string& topic,
                                             const std::vector<uint8_t>& payload) {
    std::string vehicle_id;
    uint32_t content_id;

    // Try parsing as v2c message
    if (ParseV2cTopic(topic, vehicle_id, content_id)) {
        if (content_id != config_.content_id) {
            return;  // Not our content_id
        }
        if (!OwnsVehicle(vehicle_id)) {
            return;  // Not our partition
        }

        // Update vehicle state
        {
            std::unique_lock lock(vehicles_mutex_);
            auto& state = vehicles_[vehicle_id];
            state.is_online = true;
            state.last_seen_ms = NowMs();
            state.inbound_sequence++;
        }

        messages_received_.fetch_add(1);
        bytes_received_.fetch_add(payload.size());

        // Get sequence for this message
        uint64_t seq;
        {
            std::shared_lock lock(vehicles_mutex_);
            seq = vehicles_[vehicle_id].inbound_sequence;
        }

        BroadcastVehicleMessage(vehicle_id, payload, seq);
        return;
    }

    // Try parsing as status message
    if (ParseStatusTopic(topic, vehicle_id)) {
        if (!OwnsVehicle(vehicle_id)) {
            return;  // Not our partition
        }

        bool is_online = !payload.empty() && payload[0] == '1';
        auto status = is_online
            ? swdv::cloud_backend_transport_service::vehicle_status_t::ONLINE
            : swdv::cloud_backend_transport_service::vehicle_status_t::OFFLINE;

        {
            std::unique_lock lock(vehicles_mutex_);
            auto& state = vehicles_[vehicle_id];
            state.is_online = is_online;
            if (is_online) {
                state.last_seen_ms = NowMs();
            }
        }

        BroadcastVehicleStatus(vehicle_id, status);
    }
}

// =============================================================================
// Topic Helpers
// =============================================================================

std::string CloudBackendTransportServer::V2cSubscribePattern() const {
    // v2c/+/{content_id}
    return config_.v2c_prefix + "/+/" + std::to_string(config_.content_id);
}

std::string CloudBackendTransportServer::C2vTopic(const std::string& vehicle_id) const {
    // c2v/{vehicle_id}/{content_id}
    return config_.c2v_prefix + "/" + vehicle_id + "/" + std::to_string(config_.content_id);
}

std::string CloudBackendTransportServer::StatusSubscribePattern() const {
    // v2c/+/is_online
    return config_.v2c_prefix + "/+/is_online";
}

bool CloudBackendTransportServer::ParseV2cTopic(const std::string& topic,
                                                 std::string& vehicle_id,
                                                 uint32_t& content_id) const {
    // Expected: v2c/{vehicle_id}/{content_id}
    if (topic.find(config_.v2c_prefix + "/") != 0) {
        return false;
    }

    size_t first_slash = config_.v2c_prefix.size();
    size_t second_slash = topic.find('/', first_slash + 1);
    if (second_slash == std::string::npos) {
        return false;
    }

    vehicle_id = topic.substr(first_slash + 1, second_slash - first_slash - 1);
    std::string content_str = topic.substr(second_slash + 1);

    // Skip if this is a status topic
    if (content_str == "is_online") {
        return false;
    }

    try {
        content_id = static_cast<uint32_t>(std::stoul(content_str));
        return true;
    } catch (...) {
        return false;
    }
}

bool CloudBackendTransportServer::ParseStatusTopic(const std::string& topic,
                                                    std::string& vehicle_id) const {
    // Expected: v2c/{vehicle_id}/is_online
    if (topic.find(config_.v2c_prefix + "/") != 0) {
        return false;
    }
    const std::string suffix = "/is_online";
    if (topic.size() < suffix.size() ||
        topic.compare(topic.size() - suffix.size(), suffix.size(), suffix) != 0) {
        return false;
    }

    size_t prefix_len = config_.v2c_prefix.size() + 1;  // "v2c/"
    size_t suffix_start = topic.size() - 10;  // "/is_online"
    if (suffix_start <= prefix_len) {
        return false;
    }

    vehicle_id = topic.substr(prefix_len, suffix_start - prefix_len);
    return !vehicle_id.empty();
}

bool CloudBackendTransportServer::OwnsVehicle(const std::string& vehicle_id) const {
    if (config_.total_partitions <= 1) {
        return true;  // Single partition handles all
    }
    uint32_t partition = HashVehicleId(vehicle_id) % config_.total_partitions;
    return partition == config_.partition_id;
}

// =============================================================================
// Stream Broadcasting
// =============================================================================

void CloudBackendTransportServer::BroadcastVehicleMessage(const std::string& vehicle_id,
                                                           const std::vector<uint8_t>& payload,
                                                           uint64_t sequence) {
    swdv::cloud_backend_transport_service::on_vehicle_message msg;
    auto* vm = msg.mutable_message();
    vm->set_vehicle_id(vehicle_id);
    vm->set_payload(payload.data(), payload.size());
    vm->set_sequence(sequence);
    vm->set_timestamp_ms(NowMs());

    std::shared_lock lock(message_streams_mutex_);
    for (auto* writer : message_streams_) {
        writer->Write(msg);
    }
}

void CloudBackendTransportServer::BroadcastVehicleStatus(
    const std::string& vehicle_id,
    swdv::cloud_backend_transport_service::vehicle_status_t status) {
    swdv::cloud_backend_transport_service::on_vehicle_status msg;
    auto* evt = msg.mutable_event();
    evt->set_vehicle_id(vehicle_id);
    evt->set_status(status);
    evt->set_timestamp_ms(NowMs());

    {
        std::shared_lock lock(vehicles_mutex_);
        auto it = vehicles_.find(vehicle_id);
        if (it != vehicles_.end()) {
            evt->set_last_seen_ms(it->second.last_seen_ms);
        }
    }

    std::shared_lock lock(status_streams_mutex_);
    for (auto* writer : status_streams_) {
        writer->Write(msg);
    }
}

void CloudBackendTransportServer::BroadcastAck(const std::string& vehicle_id, uint64_t sequence) {
    swdv::cloud_backend_transport_service::on_ack msg;
    auto* ack = msg.mutable_ack();
    ack->set_vehicle_id(vehicle_id);
    ack->set_sequence(sequence);

    std::shared_lock lock(ack_streams_mutex_);
    for (auto* writer : ack_streams_) {
        writer->Write(msg);
    }
}

// =============================================================================
// Helpers
// =============================================================================

int64_t CloudBackendTransportServer::NowMs() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

uint64_t CloudBackendTransportServer::NextSequence(const std::string& vehicle_id) {
    std::unique_lock lock(vehicles_mutex_);
    return ++vehicles_[vehicle_id].outbound_sequence;
}

// =============================================================================
// gRPC Method Implementations
// =============================================================================

grpc::Status CloudBackendTransportServer::send_to_vehicle(
    grpc::ServerContext* /*context*/,
    const swdv::cloud_backend_transport_service::send_to_vehicle_request* request,
    swdv::cloud_backend_transport_service::send_to_vehicle_response* response) {

    const auto& req = request->request();
    const std::string& vehicle_id = req.vehicle_id();
    auto* result = response->mutable_result();

    // Check partition ownership
    if (!OwnsVehicle(vehicle_id)) {
        result->set_status(swdv::cloud_backend_transport_service::publish_status_t::WRONG_PARTITION);
        result->set_sequence(0);
        return grpc::Status::OK;
    }

    // Check connection
    if (!connected_.load()) {
        result->set_status(swdv::cloud_backend_transport_service::publish_status_t::INVALID_REQUEST);
        result->set_sequence(0);
        return grpc::Status::OK;
    }

    // Publish to MQTT
    std::string topic = C2vTopic(vehicle_id);
    const std::string& payload = req.payload();

    int rc = mosquitto_publish(mosq_, nullptr, topic.c_str(),
                                static_cast<int>(payload.size()),
                                payload.data(), 1, false);

    if (rc != MOSQ_ERR_SUCCESS) {
        LOG(ERROR) << "Failed to publish to " << topic << ": " << mosquitto_strerror(rc);
        result->set_status(swdv::cloud_backend_transport_service::publish_status_t::INVALID_REQUEST);
        result->set_sequence(0);
        messages_failed_.fetch_add(1);
        return grpc::Status::OK;
    }

    uint64_t seq = NextSequence(vehicle_id);
    result->set_status(swdv::cloud_backend_transport_service::publish_status_t::OK);
    result->set_sequence(seq);
    result->set_queue_level(swdv::cloud_backend_transport_service::queue_level_t::NORMAL);

    messages_sent_.fetch_add(1);
    bytes_sent_.fetch_add(payload.size());

    // Simulate immediate ACK for MQTT (no real delivery confirmation)
    BroadcastAck(vehicle_id, seq);

    return grpc::Status::OK;
}

grpc::Status CloudBackendTransportServer::get_vehicle_status(
    grpc::ServerContext* /*context*/,
    const swdv::cloud_backend_transport_service::get_vehicle_status_request* request,
    swdv::cloud_backend_transport_service::get_vehicle_status_response* response) {

    const std::string& vehicle_id = request->vehicle_id();

    std::shared_lock lock(vehicles_mutex_);
    auto it = vehicles_.find(vehicle_id);

    if (it == vehicles_.end()) {
        response->set_status(swdv::cloud_backend_transport_service::vehicle_status_t::UNKNOWN);
        response->set_last_seen_ms(0);
    } else {
        response->set_status(it->second.is_online
            ? swdv::cloud_backend_transport_service::vehicle_status_t::ONLINE
            : swdv::cloud_backend_transport_service::vehicle_status_t::OFFLINE);
        response->set_last_seen_ms(it->second.last_seen_ms);
    }

    return grpc::Status::OK;
}

grpc::Status CloudBackendTransportServer::get_channel_info(
    grpc::ServerContext* /*context*/,
    const swdv::cloud_backend_transport_service::get_channel_info_request* /*request*/,
    swdv::cloud_backend_transport_service::get_channel_info_response* response) {

    auto* info = response->mutable_info();
    info->set_content_id(config_.content_id);
    info->set_partition_id(config_.partition_id);
    info->set_total_partitions(config_.total_partitions);

    return grpc::Status::OK;
}

grpc::Status CloudBackendTransportServer::get_queue_status(
    grpc::ServerContext* /*context*/,
    const swdv::cloud_backend_transport_service::get_queue_status_request* request,
    swdv::cloud_backend_transport_service::get_queue_status_response* response) {

    auto* status = response->mutable_status();
    status->set_vehicle_id(request->vehicle_id());
    // For simple MQTT implementation, queue is always at NORMAL level
    status->set_level(swdv::cloud_backend_transport_service::queue_level_t::NORMAL);
    status->set_queue_size(0);
    status->set_queue_capacity(1000);

    return grpc::Status::OK;
}

grpc::Status CloudBackendTransportServer::get_stats(
    grpc::ServerContext* /*context*/,
    const swdv::cloud_backend_transport_service::get_stats_request* /*request*/,
    swdv::cloud_backend_transport_service::get_stats_response* response) {

    auto* stats = response->mutable_stats();
    stats->set_messages_sent(messages_sent_.load());
    stats->set_messages_failed(messages_failed_.load());
    stats->set_bytes_sent(bytes_sent_.load());
    stats->set_messages_received(messages_received_.load());
    stats->set_bytes_received(bytes_received_.load());

    // Count online vehicles
    uint32_t online = 0;
    uint32_t total = 0;
    {
        std::shared_lock lock(vehicles_mutex_);
        total = static_cast<uint32_t>(vehicles_.size());
        for (const auto& [vid, state] : vehicles_) {
            if (state.is_online) {
                online++;
            }
        }
    }
    stats->set_vehicles_online(online);
    stats->set_vehicles_total(total);

    return grpc::Status::OK;
}

grpc::Status CloudBackendTransportServer::healthy(
    grpc::ServerContext* /*context*/,
    const swdv::cloud_backend_transport_service::healthy_request* /*request*/,
    swdv::cloud_backend_transport_service::healthy_response* response) {

    response->set_is_healthy(connected_.load() && running_.load());
    return grpc::Status::OK;
}

// =============================================================================
// gRPC Streaming Event Implementations
// =============================================================================

grpc::Status CloudBackendTransportServer::subscribe(
    grpc::ServerContext* context,
    const swdv::cloud_backend_transport_service::on_vehicle_message_subscribe_request* /*request*/,
    grpc::ServerWriter<swdv::cloud_backend_transport_service::on_vehicle_message>* writer) {

    {
        std::unique_lock lock(message_streams_mutex_);
        message_streams_.push_back(writer);
    }

    // Block until client disconnects
    while (running_.load() && !context->IsCancelled()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    {
        std::unique_lock lock(message_streams_mutex_);
        message_streams_.erase(
            std::remove(message_streams_.begin(), message_streams_.end(), writer),
            message_streams_.end());
    }

    return grpc::Status::OK;
}

grpc::Status CloudBackendTransportServer::subscribe(
    grpc::ServerContext* context,
    const swdv::cloud_backend_transport_service::on_ack_subscribe_request* /*request*/,
    grpc::ServerWriter<swdv::cloud_backend_transport_service::on_ack>* writer) {

    {
        std::unique_lock lock(ack_streams_mutex_);
        ack_streams_.push_back(writer);
    }

    while (running_.load() && !context->IsCancelled()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    {
        std::unique_lock lock(ack_streams_mutex_);
        ack_streams_.erase(
            std::remove(ack_streams_.begin(), ack_streams_.end(), writer),
            ack_streams_.end());
    }

    return grpc::Status::OK;
}

grpc::Status CloudBackendTransportServer::subscribe(
    grpc::ServerContext* context,
    const swdv::cloud_backend_transport_service::on_vehicle_status_subscribe_request* /*request*/,
    grpc::ServerWriter<swdv::cloud_backend_transport_service::on_vehicle_status>* writer) {

    {
        std::unique_lock lock(status_streams_mutex_);
        status_streams_.push_back(writer);
    }

    while (running_.load() && !context->IsCancelled()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    {
        std::unique_lock lock(status_streams_mutex_);
        status_streams_.erase(
            std::remove(status_streams_.begin(), status_streams_.end(), writer),
            status_streams_.end());
    }

    return grpc::Status::OK;
}

grpc::Status CloudBackendTransportServer::subscribe(
    grpc::ServerContext* context,
    const swdv::cloud_backend_transport_service::on_queue_status_changed_subscribe_request* /*request*/,
    grpc::ServerWriter<swdv::cloud_backend_transport_service::on_queue_status_changed>* writer) {

    {
        std::unique_lock lock(queue_streams_mutex_);
        queue_streams_.push_back(writer);
    }

    while (running_.load() && !context->IsCancelled()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    {
        std::unique_lock lock(queue_streams_mutex_);
        queue_streams_.erase(
            std::remove(queue_streams_.begin(), queue_streams_.end(), writer),
            queue_streams_.end());
    }

    return grpc::Status::OK;
}

}  // namespace ifex::cloud
