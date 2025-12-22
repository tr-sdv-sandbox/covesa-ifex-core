#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Forward declarations for mosquitto C library types
struct mosquitto;
struct mosquitto_message;

namespace ifex::reference {

/// RAII wrapper for mosquitto library initialization
/// Ensures mosquitto_lib_init/cleanup are called exactly once
class MosquittoLib {
public:
    static MosquittoLib& Instance() {
        static MosquittoLib instance;
        return instance;
    }

    MosquittoLib(const MosquittoLib&) = delete;
    MosquittoLib& operator=(const MosquittoLib&) = delete;

private:
    MosquittoLib();
    ~MosquittoLib();
};

/// Custom deleter for mosquitto handle
struct MosquittoDeleter {
    void operator()(struct mosquitto* mosq) const;
};

/// RAII wrapper for mosquitto handle
using MosquittoPtr = std::unique_ptr<struct mosquitto, MosquittoDeleter>;

/// Create a new mosquitto client instance
/// @param client_id Client identifier (empty for auto-generated)
/// @param clean_session Clean session flag
/// @param userdata User data pointer passed to callbacks
/// @return Managed mosquitto pointer (nullptr on failure)
MosquittoPtr MakeMosquitto(const std::string& client_id, bool clean_session, void* userdata);

/// Simple MQTT client wrapper around libmosquitto
///
/// Thread-safe, handles reconnection automatically.
/// Single instance shared by all gRPC clients.
class MqttClient {
public:
    using ConnectCallback = std::function<void()>;
    using DisconnectCallback = std::function<void(int reason_code)>;
    using MessageCallback = std::function<void(const std::string& topic, const std::vector<uint8_t>& payload)>;

    struct Config {
        std::string host = "localhost";
        int port = 1883;
        std::string client_id;
        std::string username;
        std::string password;

        // Reconnection
        bool auto_reconnect = true;
        std::chrono::seconds reconnect_delay_min{1};
        std::chrono::seconds reconnect_delay_max{30};

        // Keep alive
        int keepalive_seconds = 60;

        // Clean session
        bool clean_session = true;
    };

    explicit MqttClient(const Config& config);
    ~MqttClient();

    // Non-copyable, non-movable
    MqttClient(const MqttClient&) = delete;
    MqttClient& operator=(const MqttClient&) = delete;
    MqttClient(MqttClient&&) = delete;
    MqttClient& operator=(MqttClient&&) = delete;

    /// Connect to broker (async, returns immediately)
    bool Connect();

    /// Disconnect from broker
    void Disconnect();

    /// Check if connected
    bool IsConnected() const { return connected_.load(); }

    /// Publish message to topic
    /// @param topic MQTT topic
    /// @param payload Binary payload
    /// @param qos QoS level (0, 1, or 2)
    /// @param retain Retain flag
    /// @return true if publish initiated successfully
    bool Publish(const std::string& topic,
                 const std::vector<uint8_t>& payload,
                 int qos = 1,
                 bool retain = false);

    /// Subscribe to topic pattern
    /// @param topic_pattern Topic pattern (may include wildcards)
    /// @param qos QoS level
    /// @return true if subscribe initiated successfully
    bool Subscribe(const std::string& topic_pattern, int qos = 1);

    /// Unsubscribe from topic pattern
    bool Unsubscribe(const std::string& topic_pattern);

    /// Set callbacks
    void OnConnect(ConnectCallback cb) { on_connect_ = std::move(cb); }
    void OnDisconnect(DisconnectCallback cb) { on_disconnect_ = std::move(cb); }
    void OnMessage(MessageCallback cb) { on_message_ = std::move(cb); }

    /// Get last error message
    std::string LastError() const;

private:
    // Mosquitto callbacks (static, dispatch to instance)
    static void OnConnectStatic(struct mosquitto* mosq, void* userdata, int rc);
    static void OnDisconnectStatic(struct mosquitto* mosq, void* userdata, int rc);
    static void OnMessageStatic(struct mosquitto* mosq, void* userdata, const struct mosquitto_message* msg);
    static void OnPublishStatic(struct mosquitto* mosq, void* userdata, int mid);

    void HandleConnect(int rc);
    void HandleDisconnect(int rc);
    void HandleMessage(const std::string& topic, const std::vector<uint8_t>& payload);

    void LoopThread();

    Config config_;
    MosquittoPtr mosq_;

    std::atomic<bool> connected_{false};
    std::atomic<bool> running_{false};
    std::thread loop_thread_;

    ConnectCallback on_connect_;
    DisconnectCallback on_disconnect_;
    MessageCallback on_message_;

    mutable std::mutex error_mutex_;
    std::string last_error_;
};

}  // namespace ifex::reference
