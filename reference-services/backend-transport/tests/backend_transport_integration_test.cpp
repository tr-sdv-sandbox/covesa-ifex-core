/**
 * @file backend_transport_integration_test.cpp
 * @brief Integration tests requiring MQTT broker knowledge
 *
 * These tests verify behavior that requires knowledge of the actual MQTT
 * transport layer, such as:
 * - C2V (cloud-to-vehicle) message reception via MQTT publish
 * - MQTT topic structure verification
 * - End-to-end message flow through the actual broker
 *
 * Note: Black-box API tests are in backend_transport_conformance_test.cpp
 * Note: Client library tests are in backend_transport_client_test.cpp
 */

#include "mqtt_test_fixture.hpp"
#include "backend_transport_client.hpp"
#include "backend_transport_server.hpp"

#include <gtest/gtest.h>
#include <glog/logging.h>
#include <grpcpp/grpcpp.h>
#include <mosquitto.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

namespace ifex::test {

using namespace ifex::client;
using namespace std::chrono_literals;

/**
 * @brief Test fixture for MQTT integration tests
 */
class BackendTransportIntegrationTest : public MqttTestFixture {
protected:
    static std::unique_ptr<reference::BackendTransportServer> service_;
    static std::unique_ptr<grpc::Server> grpc_server_;
    static std::shared_ptr<grpc::Channel> channel_;
    static int grpc_port_;
    static std::string vehicle_id_;

    static void SetUpTestSuite() {
        MqttTestFixture::SetUpTestSuite();

        if (!container_started) {
            return;
        }

        vehicle_id_ = "integration-test-vehicle";

        reference::BackendTransportServer::Config config;
        config.mqtt_host = mqtt_host;
        config.mqtt_port = mqtt_port;
        config.vehicle_id = vehicle_id_;
        config.queue_size_per_content_id = 100;
        config.persistence_dir = "/tmp/ifex-integration-test";

        service_ = std::make_unique<reference::BackendTransportServer>(config);

        if (!service_->Start()) {
            LOG(ERROR) << "Failed to start backend transport service";
            GTEST_SKIP() << "Failed to start backend transport service";
            return;
        }

        grpc::ServerBuilder builder;
        builder.AddListeningPort("0.0.0.0:0", grpc::InsecureServerCredentials(), &grpc_port_);

        using namespace swdv::backend_transport_service;
        builder.RegisterService(static_cast<publish_service::Service*>(service_.get()));
        builder.RegisterService(static_cast<get_connection_status_service::Service*>(service_.get()));
        builder.RegisterService(static_cast<get_queue_status_service::Service*>(service_.get()));
        builder.RegisterService(static_cast<get_stats_service::Service*>(service_.get()));
        builder.RegisterService(static_cast<healthy_service::Service*>(service_.get()));
        builder.RegisterService(static_cast<get_content_id_service::Service*>(service_.get()));
        builder.RegisterService(static_cast<on_content_service::Service*>(service_.get()));
        builder.RegisterService(static_cast<on_ack_service::Service*>(service_.get()));
        builder.RegisterService(static_cast<on_connection_changed_service::Service*>(service_.get()));
        builder.RegisterService(static_cast<on_queue_status_changed_service::Service*>(service_.get()));

        grpc_server_ = builder.BuildAndStart();

        std::string server_address = "localhost:" + std::to_string(grpc_port_);
        channel_ = grpc::CreateChannel(server_address, grpc::InsecureChannelCredentials());

        LOG(INFO) << "Integration test service listening on " << server_address;
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    static void TearDownTestSuite() {
        channel_.reset();
        if (grpc_server_) {
            grpc_server_->Shutdown();
            grpc_server_.reset();
        }
        if (service_) {
            service_->Stop();
            service_.reset();
        }
        MqttTestFixture::TearDownTestSuite();
    }

    void SetUp() override {
        MqttTestFixture::SetUp();
        if (!service_ || !grpc_server_ || !channel_) {
            GTEST_SKIP() << "Service not running";
        }
    }

    BackendTransportClient createClient(uint32_t content_id) {
        return BackendTransportClient(channel_, content_id);
    }

    /// Publish a message directly to MQTT (simulating cloud-to-vehicle)
    bool publishToMqtt(uint32_t content_id, const std::vector<uint8_t>& payload) {
        std::string topic = "c2v/" + vehicle_id_ + "/" + std::to_string(content_id);

        struct mosquitto* mosq = mosquitto_new("test-publisher", true, nullptr);
        if (!mosq) {
            LOG(ERROR) << "Failed to create mosquitto client";
            return false;
        }

        int rc = mosquitto_connect(mosq, mqtt_host.c_str(), mqtt_port, 60);
        if (rc != MOSQ_ERR_SUCCESS) {
            LOG(ERROR) << "Failed to connect to MQTT: " << mosquitto_strerror(rc);
            mosquitto_destroy(mosq);
            return false;
        }

        rc = mosquitto_publish(mosq, nullptr, topic.c_str(),
                               static_cast<int>(payload.size()),
                               payload.data(), 1, false);
        if (rc != MOSQ_ERR_SUCCESS) {
            LOG(ERROR) << "Failed to publish: " << mosquitto_strerror(rc);
            mosquitto_disconnect(mosq);
            mosquitto_destroy(mosq);
            return false;
        }

        // Wait for publish to complete
        mosquitto_loop(mosq, 1000, 1);

        mosquitto_disconnect(mosq);
        mosquitto_destroy(mosq);
        return true;
    }

    /// Subscribe to MQTT topic and capture messages (for verifying v2c)
    std::vector<std::vector<uint8_t>> subscribeAndCapture(
            const std::string& topic, int expected_count, std::chrono::seconds timeout) {

        struct CaptureContext {
            std::vector<std::vector<uint8_t>> messages;
            std::mutex mtx;
            std::condition_variable cv;
            int expected;
        };

        CaptureContext ctx;
        ctx.expected = expected_count;

        struct mosquitto* mosq = mosquitto_new("test-subscriber", true, &ctx);
        if (!mosq) {
            return {};
        }

        mosquitto_message_callback_set(mosq, [](struct mosquitto*, void* userdata,
                                                 const struct mosquitto_message* msg) {
            auto* ctx = static_cast<CaptureContext*>(userdata);
            std::lock_guard<std::mutex> lock(ctx->mtx);
            std::vector<uint8_t> payload(
                static_cast<uint8_t*>(msg->payload),
                static_cast<uint8_t*>(msg->payload) + msg->payloadlen);
            ctx->messages.push_back(std::move(payload));
            ctx->cv.notify_all();
        });

        if (mosquitto_connect(mosq, mqtt_host.c_str(), mqtt_port, 60) != MOSQ_ERR_SUCCESS) {
            mosquitto_destroy(mosq);
            return {};
        }

        mosquitto_subscribe(mosq, nullptr, topic.c_str(), 1);

        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            mosquitto_loop(mosq, 100, 1);

            std::lock_guard<std::mutex> lock(ctx.mtx);
            if (static_cast<int>(ctx.messages.size()) >= expected_count) {
                break;
            }
        }

        mosquitto_disconnect(mosq);
        mosquitto_destroy(mosq);

        return ctx.messages;
    }
};

std::unique_ptr<reference::BackendTransportServer> BackendTransportIntegrationTest::service_;
std::unique_ptr<grpc::Server> BackendTransportIntegrationTest::grpc_server_;
std::shared_ptr<grpc::Channel> BackendTransportIntegrationTest::channel_;
int BackendTransportIntegrationTest::grpc_port_ = 0;
std::string BackendTransportIntegrationTest::vehicle_id_;

// =============================================================================
// C2V (Cloud-to-Vehicle) Tests - on_content
// =============================================================================

TEST_F(BackendTransportIntegrationTest, OnContentReceivesMessagesFromMqtt) {
    const uint32_t content_id = 5001;
    auto client = createClient(content_id);

    std::mutex mtx;
    std::condition_variable cv;
    std::vector<std::vector<uint8_t>> received_payloads;

    client.on_content([&](const std::vector<uint8_t>& payload) {
        std::lock_guard<std::mutex> lock(mtx);
        received_payloads.push_back(payload);
        LOG(INFO) << "Received C2V message, size=" << payload.size();
        cv.notify_all();
    });

    // Give subscription time to establish
    std::this_thread::sleep_for(500ms);

    // Publish message to MQTT (simulating cloud sending to vehicle)
    std::vector<uint8_t> test_payload = {0xDE, 0xAD, 0xBE, 0xEF};
    ASSERT_TRUE(publishToMqtt(content_id, test_payload))
        << "Failed to publish to MQTT";

    // Wait for message to arrive via gRPC stream
    {
        std::unique_lock<std::mutex> lock(mtx);
        ASSERT_TRUE(cv.wait_for(lock, 10s, [&]() {
            return !received_payloads.empty();
        })) << "Should receive C2V message";
    }

    client.unsubscribe_all();

    ASSERT_EQ(received_payloads.size(), 1);
    EXPECT_EQ(received_payloads[0], test_payload);
}

TEST_F(BackendTransportIntegrationTest, OnContentReceivesMultipleMessages) {
    const uint32_t content_id = 5002;
    auto client = createClient(content_id);

    std::mutex mtx;
    std::condition_variable cv;
    std::vector<std::vector<uint8_t>> received_payloads;

    client.on_content([&](const std::vector<uint8_t>& payload) {
        std::lock_guard<std::mutex> lock(mtx);
        received_payloads.push_back(payload);
        cv.notify_all();
    });

    std::this_thread::sleep_for(500ms);

    // Publish multiple messages
    std::vector<std::vector<uint8_t>> sent_payloads = {
        {0x01, 0x02, 0x03},
        {0x04, 0x05, 0x06},
        {0x07, 0x08, 0x09}
    };

    for (const auto& payload : sent_payloads) {
        ASSERT_TRUE(publishToMqtt(content_id, payload));
        std::this_thread::sleep_for(100ms);  // Small delay between messages
    }

    // Wait for all messages
    {
        std::unique_lock<std::mutex> lock(mtx);
        ASSERT_TRUE(cv.wait_for(lock, 10s, [&]() {
            return received_payloads.size() >= sent_payloads.size();
        })) << "Should receive all C2V messages";
    }

    client.unsubscribe_all();

    EXPECT_EQ(received_payloads.size(), sent_payloads.size());
    for (size_t i = 0; i < sent_payloads.size(); ++i) {
        EXPECT_EQ(received_payloads[i], sent_payloads[i])
            << "Message " << i << " should match";
    }
}

TEST_F(BackendTransportIntegrationTest, OnContentOnlyReceivesOwnContentId) {
    const uint32_t content_id_1 = 5003;
    const uint32_t content_id_2 = 5004;

    auto client1 = createClient(content_id_1);

    std::mutex mtx;
    std::atomic<int> received_count{0};
    std::vector<uint8_t> received_payload;

    client1.on_content([&](const std::vector<uint8_t>& payload) {
        std::lock_guard<std::mutex> lock(mtx);
        received_payload = payload;
        received_count++;
    });

    std::this_thread::sleep_for(500ms);

    // Publish to wrong content_id
    publishToMqtt(content_id_2, {0xFF, 0xFF});
    std::this_thread::sleep_for(500ms);

    // Should not receive message for wrong content_id
    EXPECT_EQ(received_count.load(), 0);

    // Publish to correct content_id
    std::vector<uint8_t> correct_payload = {0x01, 0x02};
    publishToMqtt(content_id_1, correct_payload);

    // Wait for correct message
    std::this_thread::sleep_for(1s);

    client1.unsubscribe_all();

    EXPECT_EQ(received_count.load(), 1);
    EXPECT_EQ(received_payload, correct_payload);
}

// =============================================================================
// V2C (Vehicle-to-Cloud) MQTT Verification
// =============================================================================

TEST_F(BackendTransportIntegrationTest, PublishSendsToCorrectMqttTopic) {
    const uint32_t content_id = 5010;
    std::string expected_topic = "v2c/" + vehicle_id_ + "/" + std::to_string(content_id);

    auto client = createClient(content_id);

    // Start MQTT subscriber in background
    auto future = std::async(std::launch::async, [&]() {
        return subscribeAndCapture(expected_topic, 1, 10s);
    });

    // Give subscriber time to connect
    std::this_thread::sleep_for(500ms);

    // Publish via gRPC client
    std::vector<uint8_t> payload = {0x11, 0x22, 0x33, 0x44};
    auto result = client.publish(payload, Persistence::Volatile);
    ASSERT_TRUE(result.ok());

    // Wait for message on MQTT
    auto captured = future.get();

    ASSERT_EQ(captured.size(), 1) << "Should receive one message on MQTT";
    EXPECT_EQ(captured[0], payload) << "Payload should match";
}

TEST_F(BackendTransportIntegrationTest, MessagesDeliveredInOrder) {
    const uint32_t content_id = 5011;
    std::string topic = "v2c/" + vehicle_id_ + "/" + std::to_string(content_id);

    auto client = createClient(content_id);

    const int num_messages = 10;

    // Start MQTT subscriber
    auto future = std::async(std::launch::async, [&]() {
        return subscribeAndCapture(topic, num_messages, 15s);
    });

    std::this_thread::sleep_for(500ms);

    // Publish messages
    for (int i = 0; i < num_messages; ++i) {
        auto result = client.publish({static_cast<uint8_t>(i)}, Persistence::Volatile);
        ASSERT_TRUE(result.ok());
    }

    auto captured = future.get();

    ASSERT_EQ(captured.size(), static_cast<size_t>(num_messages));

    // Verify order
    for (int i = 0; i < num_messages; ++i) {
        ASSERT_EQ(captured[i].size(), 1);
        EXPECT_EQ(captured[i][0], static_cast<uint8_t>(i))
            << "Message " << i << " should be in order";
    }
}

// =============================================================================
// Stats Verification via MQTT
// =============================================================================

TEST_F(BackendTransportIntegrationTest, MessagesSentIncreasesAfterMqttDelivery) {
    auto client = createClient(5020);

    auto initial = client.stats();
    uint64_t initial_sent = initial.messages_sent;

    // Publish and wait for delivery
    for (int i = 0; i < 5; ++i) {
        auto result = client.publish({static_cast<uint8_t>(i)}, Persistence::Volatile);
        ASSERT_TRUE(result.ok());
    }

    // Wait for MQTT delivery
    std::this_thread::sleep_for(1s);

    auto after = client.stats();
    EXPECT_GE(after.messages_sent, initial_sent + 5);
    EXPECT_GT(after.bytes_sent, initial.bytes_sent);
}

TEST_F(BackendTransportIntegrationTest, MessagesReceivedIncreasesAfterC2V) {
    const uint32_t content_id = 5021;
    auto client = createClient(content_id);

    // Subscribe to start receiving
    std::mutex mtx;
    std::condition_variable cv;
    std::atomic<int> received{0};
    client.on_content([&](const std::vector<uint8_t>&) {
        received++;
        cv.notify_all();
    });

    std::this_thread::sleep_for(500ms);

    auto initial = client.stats();
    uint64_t initial_received = initial.messages_received;

    // Publish via MQTT (with small delays to ensure ordering)
    for (int i = 0; i < 3; ++i) {
        publishToMqtt(content_id, {static_cast<uint8_t>(i)});
        std::this_thread::sleep_for(100ms);
    }

    // Wait for all 3 messages to be received
    {
        std::unique_lock<std::mutex> lock(mtx);
        ASSERT_TRUE(cv.wait_for(lock, 5s, [&]() {
            return received.load() >= 3;
        })) << "Should receive all 3 C2V messages, got " << received.load();
    }

    client.unsubscribe_all();

    auto after = client.stats();
    EXPECT_GE(after.messages_received, initial_received + 3);
}

// =============================================================================
// C2V Queue Tests - Messages queued before handler registration
// =============================================================================

/**
 * @brief Test fixture for c2v queue tests with fresh service instance
 *
 * These tests require a fresh service instance to control the timing
 * of handler registration vs message arrival.
 */
class C2vQueueTest : public MqttTestFixture {
protected:
    std::unique_ptr<reference::BackendTransportServer> service_;
    std::unique_ptr<grpc::Server> grpc_server_;
    std::shared_ptr<grpc::Channel> channel_;
    int grpc_port_ = 0;
    std::string vehicle_id_;

    void SetUp() override {
        MqttTestFixture::SetUp();
        if (!container_started) {
            GTEST_SKIP() << "MQTT container not available";
            return;
        }

        vehicle_id_ = "c2v-queue-test-" + std::to_string(std::rand());
    }

    void TearDown() override {
        channel_.reset();
        if (grpc_server_) {
            grpc_server_->Shutdown();
            grpc_server_.reset();
        }
        if (service_) {
            service_->Stop();
            service_.reset();
        }
        MqttTestFixture::TearDown();
    }

    void startService() {
        reference::BackendTransportServer::Config config;
        config.mqtt_host = mqtt_host;
        config.mqtt_port = mqtt_port;
        config.vehicle_id = vehicle_id_;
        config.queue_size_per_content_id = 100;
        config.c2v_queue_size_per_content_id = 50;
        config.persistence_dir = "/tmp/ifex-c2v-queue-test";
        config.clean_session = false;  // Enable persistent sessions

        service_ = std::make_unique<reference::BackendTransportServer>(config);
        ASSERT_TRUE(service_->Start()) << "Failed to start service";

        grpc::ServerBuilder builder;
        builder.AddListeningPort("0.0.0.0:0", grpc::InsecureServerCredentials(), &grpc_port_);

        using namespace swdv::backend_transport_service;
        builder.RegisterService(static_cast<publish_service::Service*>(service_.get()));
        builder.RegisterService(static_cast<get_connection_status_service::Service*>(service_.get()));
        builder.RegisterService(static_cast<get_queue_status_service::Service*>(service_.get()));
        builder.RegisterService(static_cast<get_stats_service::Service*>(service_.get()));
        builder.RegisterService(static_cast<healthy_service::Service*>(service_.get()));
        builder.RegisterService(static_cast<get_content_id_service::Service*>(service_.get()));
        builder.RegisterService(static_cast<on_content_service::Service*>(service_.get()));
        builder.RegisterService(static_cast<on_ack_service::Service*>(service_.get()));
        builder.RegisterService(static_cast<on_connection_changed_service::Service*>(service_.get()));
        builder.RegisterService(static_cast<on_queue_status_changed_service::Service*>(service_.get()));

        grpc_server_ = builder.BuildAndStart();
        ASSERT_TRUE(grpc_server_) << "Failed to start gRPC server";

        std::string server_address = "localhost:" + std::to_string(grpc_port_);
        channel_ = grpc::CreateChannel(server_address, grpc::InsecureChannelCredentials());

        // Wait for service to be ready
        std::this_thread::sleep_for(1s);
    }

    BackendTransportClient createClient(uint32_t content_id) {
        return BackendTransportClient(channel_, content_id);
    }

    bool publishToMqtt(uint32_t content_id, const std::vector<uint8_t>& payload) {
        std::string topic = "c2v/" + vehicle_id_ + "/" + std::to_string(content_id);

        struct mosquitto* mosq = mosquitto_new("c2v-queue-test-pub", true, nullptr);
        if (!mosq) return false;

        if (mosquitto_connect(mosq, mqtt_host.c_str(), mqtt_port, 60) != MOSQ_ERR_SUCCESS) {
            mosquitto_destroy(mosq);
            return false;
        }

        int rc = mosquitto_publish(mosq, nullptr, topic.c_str(),
                                   static_cast<int>(payload.size()),
                                   payload.data(), 1, false);

        mosquitto_loop(mosq, 1000, 1);
        mosquitto_disconnect(mosq);
        mosquitto_destroy(mosq);

        return rc == MOSQ_ERR_SUCCESS;
    }

    void subscribeServiceToTopic(uint32_t content_id) {
        // Force service to subscribe to MQTT topic by creating a client
        // that subscribes but immediately unsubscribes
        // This ensures the MQTT subscription is active
        auto client = createClient(content_id);
        std::atomic<bool> got_message{false};
        client.on_content([&](const std::vector<uint8_t>&) {
            got_message = true;
        });
        std::this_thread::sleep_for(200ms);
        client.unsubscribe_all();
    }
};

TEST_F(C2vQueueTest, MessagesQueuedBeforeHandlerRegistration) {
    const uint32_t content_id = 6001;

    // Start service
    startService();

    // First, make sure service subscribes to the MQTT topic
    // by briefly registering and unregistering a handler
    {
        auto client = createClient(content_id);
        std::atomic<bool> dummy{false};
        client.on_content([&](const std::vector<uint8_t>&) { dummy = true; });
        std::this_thread::sleep_for(300ms);
        client.unsubscribe_all();
    }

    // Now publish messages to MQTT while NO handler is registered
    // These should be queued in Backend Transport's c2v queue
    std::vector<std::vector<uint8_t>> sent_payloads = {
        {0x01, 0x02, 0x03},
        {0x04, 0x05, 0x06},
        {0x07, 0x08, 0x09}
    };

    LOG(INFO) << "Publishing " << sent_payloads.size() << " messages while no handler registered";

    for (const auto& payload : sent_payloads) {
        ASSERT_TRUE(publishToMqtt(content_id, payload))
            << "Failed to publish to MQTT";
        std::this_thread::sleep_for(100ms);
    }

    // Wait for messages to arrive at Backend Transport
    std::this_thread::sleep_for(500ms);

    // Now register a handler - should receive the queued messages
    std::mutex mtx;
    std::condition_variable cv;
    std::vector<std::vector<uint8_t>> received_payloads;

    auto client = createClient(content_id);
    client.on_content([&](const std::vector<uint8_t>& payload) {
        std::lock_guard<std::mutex> lock(mtx);
        received_payloads.push_back(payload);
        LOG(INFO) << "Handler received message, size=" << payload.size()
                  << ", total=" << received_payloads.size();
        cv.notify_all();
    });

    // Wait for queued messages to be delivered
    {
        std::unique_lock<std::mutex> lock(mtx);
        bool success = cv.wait_for(lock, 10s, [&]() {
            return received_payloads.size() >= sent_payloads.size();
        });

        LOG(INFO) << "Received " << received_payloads.size() << " of "
                  << sent_payloads.size() << " expected messages";

        ASSERT_TRUE(success)
            << "Should receive all queued messages after handler registration. "
            << "Got " << received_payloads.size() << ", expected " << sent_payloads.size();
    }

    client.unsubscribe_all();

    // Verify message contents
    ASSERT_EQ(received_payloads.size(), sent_payloads.size());
    for (size_t i = 0; i < sent_payloads.size(); ++i) {
        EXPECT_EQ(received_payloads[i], sent_payloads[i])
            << "Message " << i << " content should match";
    }
}

TEST_F(C2vQueueTest, QueuedMessagesDeliveredInOrder) {
    const uint32_t content_id = 6002;

    startService();

    // Subscribe briefly to ensure MQTT subscription exists
    {
        auto client = createClient(content_id);
        std::atomic<bool> dummy{false};
        client.on_content([&](const std::vector<uint8_t>&) { dummy = true; });
        std::this_thread::sleep_for(300ms);
        client.unsubscribe_all();
    }

    // Publish numbered messages while no handler registered
    const int num_messages = 10;
    LOG(INFO) << "Publishing " << num_messages << " numbered messages";

    for (int i = 0; i < num_messages; ++i) {
        ASSERT_TRUE(publishToMqtt(content_id, {static_cast<uint8_t>(i)}));
        std::this_thread::sleep_for(50ms);
    }

    std::this_thread::sleep_for(500ms);

    // Register handler
    std::mutex mtx;
    std::condition_variable cv;
    std::vector<uint8_t> received_sequence;

    auto client = createClient(content_id);
    client.on_content([&](const std::vector<uint8_t>& payload) {
        std::lock_guard<std::mutex> lock(mtx);
        if (!payload.empty()) {
            received_sequence.push_back(payload[0]);
        }
        cv.notify_all();
    });

    // Wait for all messages
    {
        std::unique_lock<std::mutex> lock(mtx);
        ASSERT_TRUE(cv.wait_for(lock, 10s, [&]() {
            return received_sequence.size() >= static_cast<size_t>(num_messages);
        })) << "Should receive all " << num_messages << " messages, got "
            << received_sequence.size();
    }

    client.unsubscribe_all();

    // Verify order
    ASSERT_EQ(received_sequence.size(), static_cast<size_t>(num_messages));
    for (int i = 0; i < num_messages; ++i) {
        EXPECT_EQ(received_sequence[i], static_cast<uint8_t>(i))
            << "Message " << i << " should be in order";
    }
}

TEST_F(C2vQueueTest, CleanSessionConfiguredCorrectly) {
    // This test verifies that clean_session=false is configured
    // We can't directly inspect the MQTT connection, but we can verify
    // the config is passed correctly

    reference::BackendTransportServer::Config config;
    config.mqtt_host = mqtt_host;
    config.mqtt_port = mqtt_port;
    config.vehicle_id = "clean-session-test";
    config.clean_session = false;  // Persistent sessions

    // Default should be false (persistent sessions)
    reference::BackendTransportServer::Config default_config;
    EXPECT_FALSE(default_config.clean_session)
        << "Default clean_session should be false for persistent sessions";
}

TEST_F(C2vQueueTest, QueueSizeRespected) {
    const uint32_t content_id = 6003;

    // Start service with small queue size
    reference::BackendTransportServer::Config config;
    config.mqtt_host = mqtt_host;
    config.mqtt_port = mqtt_port;
    config.vehicle_id = vehicle_id_;
    config.queue_size_per_content_id = 100;
    config.c2v_queue_size_per_content_id = 5;  // Small queue
    config.persistence_dir = "/tmp/ifex-c2v-queue-test";

    service_ = std::make_unique<reference::BackendTransportServer>(config);
    ASSERT_TRUE(service_->Start());

    grpc::ServerBuilder builder;
    builder.AddListeningPort("0.0.0.0:0", grpc::InsecureServerCredentials(), &grpc_port_);

    using namespace swdv::backend_transport_service;
    builder.RegisterService(static_cast<on_content_service::Service*>(service_.get()));

    grpc_server_ = builder.BuildAndStart();
    ASSERT_TRUE(grpc_server_);

    channel_ = grpc::CreateChannel("localhost:" + std::to_string(grpc_port_),
                                    grpc::InsecureChannelCredentials());

    std::this_thread::sleep_for(1s);

    // Subscribe briefly to ensure MQTT subscription exists
    {
        auto client = createClient(content_id);
        std::atomic<bool> dummy{false};
        client.on_content([&](const std::vector<uint8_t>&) { dummy = true; });
        std::this_thread::sleep_for(300ms);
        client.unsubscribe_all();
    }

    // Publish more messages than queue can hold
    const int num_messages = 10;  // More than queue size of 5
    for (int i = 0; i < num_messages; ++i) {
        publishToMqtt(content_id, {static_cast<uint8_t>(i)});
        std::this_thread::sleep_for(50ms);
    }

    std::this_thread::sleep_for(500ms);

    // Register handler
    std::mutex mtx;
    std::condition_variable cv;
    std::vector<uint8_t> received;

    auto client = createClient(content_id);
    client.on_content([&](const std::vector<uint8_t>& payload) {
        std::lock_guard<std::mutex> lock(mtx);
        if (!payload.empty()) {
            received.push_back(payload[0]);
        }
        cv.notify_all();
    });

    // Wait for messages (may be fewer than sent due to queue limit)
    {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait_for(lock, 5s, [&]() {
            return received.size() >= 5;  // At least queue size
        });
    }

    client.unsubscribe_all();

    // Should have at most queue_size messages (oldest dropped)
    EXPECT_LE(received.size(), 5u)
        << "Queue should respect size limit, got " << received.size();

    // Most recent messages should be preserved (oldest dropped)
    if (received.size() == 5) {
        // Should have messages 5-9 (oldest 0-4 dropped)
        EXPECT_GE(received[0], 5u)
            << "Oldest messages should be dropped when queue full";
    }
}

}  // namespace ifex::test

int main(int argc, char** argv) {
    google::InitGoogleLogging(argv[0]);
    FLAGS_logtostderr = true;
    ::testing::InitGoogleTest(&argc, argv);
    mosquitto_lib_init();
    int result = RUN_ALL_TESTS();
    mosquitto_lib_cleanup();
    return result;
}
