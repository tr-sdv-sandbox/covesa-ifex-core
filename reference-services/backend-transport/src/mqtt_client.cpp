#include "mqtt_client.hpp"

#include <glog/logging.h>
#include <mosquitto.h>

#include <cstring>

namespace ifex::reference {

// =============================================================================
// RAII Wrappers Implementation
// =============================================================================

MosquittoLib::MosquittoLib() {
    mosquitto_lib_init();
    LOG(INFO) << "Mosquitto library initialized";
}

MosquittoLib::~MosquittoLib() {
    mosquitto_lib_cleanup();
    LOG(INFO) << "Mosquitto library cleaned up";
}

void MosquittoDeleter::operator()(struct mosquitto* mosq) const {
    if (mosq) {
        mosquitto_destroy(mosq);
    }
}

MosquittoPtr MakeMosquitto(const std::string& client_id, bool clean_session, void* userdata) {
    // Ensure library is initialized (singleton)
    MosquittoLib::Instance();

    struct mosquitto* mosq = mosquitto_new(
        client_id.empty() ? nullptr : client_id.c_str(),
        clean_session,
        userdata);

    return MosquittoPtr(mosq);
}

// =============================================================================
// MqttClient Implementation
// =============================================================================

MqttClient::MqttClient(const Config& config) : config_(config) {
    // Generate client ID if not provided
    std::string client_id = config_.client_id;
    if (client_id.empty()) {
        client_id = "ifex-backend-" + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    }

    // Create mosquitto instance with RAII
    mosq_ = MakeMosquitto(client_id, config_.clean_session, this);
    if (!mosq_) {
        LOG(ERROR) << "Failed to create mosquitto client";
        return;
    }

    // Set callbacks
    mosquitto_connect_callback_set(mosq_.get(), OnConnectStatic);
    mosquitto_disconnect_callback_set(mosq_.get(), OnDisconnectStatic);
    mosquitto_message_callback_set(mosq_.get(), OnMessageStatic);
    mosquitto_publish_callback_set(mosq_.get(), OnPublishStatic);

    // Set credentials if provided
    if (!config_.username.empty()) {
        mosquitto_username_pw_set(mosq_.get(), config_.username.c_str(),
                                  config_.password.empty() ? nullptr : config_.password.c_str());
    }

    // Configure reconnect delay
    mosquitto_reconnect_delay_set(mosq_.get(),
                                  static_cast<unsigned int>(config_.reconnect_delay_min.count()),
                                  static_cast<unsigned int>(config_.reconnect_delay_max.count()),
                                  true);  // exponential backoff
}

MqttClient::~MqttClient() {
    Disconnect();
    // mosq_ automatically cleaned up by MosquittoPtr destructor
}

bool MqttClient::Connect() {
    if (!mosq_) {
        std::lock_guard<std::mutex> lock(error_mutex_);
        last_error_ = "Mosquitto client not initialized";
        return false;
    }

    LOG(INFO) << "Connecting to MQTT broker at " << config_.host << ":" << config_.port;

    // Start the network loop thread first
    running_ = true;
    loop_thread_ = std::thread(&MqttClient::LoopThread, this);

    // Attempt initial connection (may fail, but loop thread will retry)
    int rc = mosquitto_connect_async(mosq_.get(), config_.host.c_str(), config_.port, config_.keepalive_seconds);
    if (rc != MOSQ_ERR_SUCCESS) {
        std::lock_guard<std::mutex> lock(error_mutex_);
        last_error_ = mosquitto_strerror(rc);
        LOG(WARNING) << "Initial MQTT connection failed (will retry): " << last_error_;
        // Don't return false - let loop thread handle reconnection
    }

    return true;
}

void MqttClient::Disconnect() {
    if (!running_) return;

    LOG(INFO) << "Disconnecting from MQTT broker";

    running_ = false;
    if (mosq_) {
        mosquitto_disconnect(mosq_.get());
    }

    if (loop_thread_.joinable()) {
        loop_thread_.join();
    }

    connected_ = false;
}

bool MqttClient::Publish(const std::string& topic, const std::vector<uint8_t>& payload, int qos, bool retain) {
    if (!mosq_ || !connected_) {
        std::lock_guard<std::mutex> lock(error_mutex_);
        last_error_ = "Not connected";
        return false;
    }

    int mid = 0;
    int rc = mosquitto_publish(mosq_.get(), &mid, topic.c_str(),
                               static_cast<int>(payload.size()),
                               payload.data(), qos, retain);

    if (rc != MOSQ_ERR_SUCCESS) {
        std::lock_guard<std::mutex> lock(error_mutex_);
        last_error_ = mosquitto_strerror(rc);
        LOG(WARNING) << "Failed to publish to " << topic << ": " << last_error_;
        return false;
    }

    VLOG(2) << "Published " << payload.size() << " bytes to " << topic << " (mid=" << mid << ")";
    return true;
}

bool MqttClient::Subscribe(const std::string& topic_pattern, int qos) {
    if (!mosq_) {
        std::lock_guard<std::mutex> lock(error_mutex_);
        last_error_ = "Not initialized";
        return false;
    }

    int mid = 0;
    int rc = mosquitto_subscribe(mosq_.get(), &mid, topic_pattern.c_str(), qos);

    if (rc != MOSQ_ERR_SUCCESS) {
        std::lock_guard<std::mutex> lock(error_mutex_);
        last_error_ = mosquitto_strerror(rc);
        LOG(WARNING) << "Failed to subscribe to " << topic_pattern << ": " << last_error_;
        return false;
    }

    LOG(INFO) << "Subscribed to " << topic_pattern;
    return true;
}

bool MqttClient::Unsubscribe(const std::string& topic_pattern) {
    if (!mosq_) {
        std::lock_guard<std::mutex> lock(error_mutex_);
        last_error_ = "Not initialized";
        return false;
    }

    int mid = 0;
    int rc = mosquitto_unsubscribe(mosq_.get(), &mid, topic_pattern.c_str());

    if (rc != MOSQ_ERR_SUCCESS) {
        std::lock_guard<std::mutex> lock(error_mutex_);
        last_error_ = mosquitto_strerror(rc);
        LOG(WARNING) << "Failed to unsubscribe from " << topic_pattern << ": " << last_error_;
        return false;
    }

    LOG(INFO) << "Unsubscribed from " << topic_pattern;
    return true;
}

std::string MqttClient::LastError() const {
    std::lock_guard<std::mutex> lock(error_mutex_);
    return last_error_;
}

void MqttClient::LoopThread() {
    LOG(INFO) << "MQTT loop thread started";

    auto last_reconnect_attempt = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    int reconnect_delay_secs = static_cast<int>(config_.reconnect_delay_min.count());

    while (running_) {
        int rc = mosquitto_loop(mosq_.get(), 100, 1);  // 100ms timeout

        if (rc != MOSQ_ERR_SUCCESS && running_) {
            if (rc == MOSQ_ERR_CONN_LOST || rc == MOSQ_ERR_NO_CONN) {
                if (config_.auto_reconnect) {
                    auto now = std::chrono::steady_clock::now();
                    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_reconnect_attempt).count();

                    if (elapsed >= reconnect_delay_secs) {
                        LOG(WARNING) << "MQTT connection lost, attempting reconnect...";
                        int reconnect_rc = mosquitto_reconnect_async(mosq_.get());
                        if (reconnect_rc != MOSQ_ERR_SUCCESS) {
                            // If reconnect fails (e.g., never connected before), try fresh connect
                            mosquitto_connect_async(mosq_.get(), config_.host.c_str(),
                                                   config_.port, config_.keepalive_seconds);
                        }
                        last_reconnect_attempt = now;

                        // Exponential backoff
                        reconnect_delay_secs = std::min(
                            reconnect_delay_secs * 2,
                            static_cast<int>(config_.reconnect_delay_max.count()));
                    }
                }
            } else if (rc != MOSQ_ERR_SUCCESS) {
                VLOG(2) << "MQTT loop: " << mosquitto_strerror(rc);
            }
        } else if (connected_) {
            // Reset backoff on successful connection
            reconnect_delay_secs = static_cast<int>(config_.reconnect_delay_min.count());
        }
    }

    LOG(INFO) << "MQTT loop thread stopped";
}

// Static callbacks - dispatch to instance
void MqttClient::OnConnectStatic(struct mosquitto*, void* userdata, int rc) {
    auto* self = static_cast<MqttClient*>(userdata);
    self->HandleConnect(rc);
}

void MqttClient::OnDisconnectStatic(struct mosquitto*, void* userdata, int rc) {
    auto* self = static_cast<MqttClient*>(userdata);
    self->HandleDisconnect(rc);
}

void MqttClient::OnMessageStatic(struct mosquitto*, void* userdata, const struct mosquitto_message* msg) {
    auto* self = static_cast<MqttClient*>(userdata);
    if (msg && msg->topic && msg->payloadlen > 0) {
        std::vector<uint8_t> payload(static_cast<uint8_t*>(msg->payload),
                                     static_cast<uint8_t*>(msg->payload) + msg->payloadlen);
        self->HandleMessage(msg->topic, payload);
    }
}

void MqttClient::OnPublishStatic(struct mosquitto*, void* userdata, int mid) {
    (void)userdata;
    VLOG(3) << "Message published (mid=" << mid << ")";
}

void MqttClient::HandleConnect(int rc) {
    if (rc == 0) {
        LOG(INFO) << "Connected to MQTT broker";
        connected_ = true;
        if (on_connect_) {
            on_connect_();
        }
    } else {
        std::lock_guard<std::mutex> lock(error_mutex_);
        last_error_ = mosquitto_connack_string(rc);
        LOG(ERROR) << "MQTT connection failed: " << last_error_;
    }
}

void MqttClient::HandleDisconnect(int rc) {
    connected_ = false;

    if (rc == 0) {
        LOG(INFO) << "Disconnected from MQTT broker (clean)";
    } else {
        LOG(WARNING) << "Disconnected from MQTT broker (rc=" << rc << ")";
    }

    if (on_disconnect_) {
        on_disconnect_(rc);
    }
}

void MqttClient::HandleMessage(const std::string& topic, const std::vector<uint8_t>& payload) {
    VLOG(2) << "Received " << payload.size() << " bytes on " << topic;

    if (on_message_) {
        on_message_(topic, payload);
    }
}

}  // namespace ifex::reference
