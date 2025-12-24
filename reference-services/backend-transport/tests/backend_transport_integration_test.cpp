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
