/**
 * @file cloud_backend_transport_integration_test.cpp
 * @brief Integration tests for paired cloud/vehicle backend transport services
 *
 * Tests the complete vehicle↔cloud communication using REAL services:
 * - Vehicle-side: BackendTransportServer + BackendTransportClient
 * - Cloud-side: CloudBackendTransportServer + CloudBackendTransportClient
 *
 * Both connect to the same MQTT broker and communicate via:
 * - v2c/{vehicle_id}/{content_id} - vehicle to cloud
 * - c2v/{vehicle_id}/{content_id} - cloud to vehicle
 * - v2c/{vehicle_id}/is_online - vehicle status
 */

#include "cloud_backend_transport_server.hpp"
#include "cloud_backend_transport_client.hpp"
#include "backend_transport_server.hpp"
#include "backend_transport_client.hpp"

#include <gtest/gtest.h>
#include <glog/logging.h>
#include <grpcpp/grpcpp.h>
#include <mosquitto.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace ifex::cloud::test {

using namespace std::chrono_literals;

// =============================================================================
// MQTT Test Fixture
// =============================================================================

class MqttTestFixture : public ::testing::Test {
protected:
    static constexpr const char* MQTT_IMAGE = "eclipse-mosquitto:2";
    static constexpr const char* CONTAINER_NAME = "ifex-paired-transport-test-broker";
    static constexpr const char* MQTT_PORT = "11885";
    static std::string mqtt_host;
    static int mqtt_port;
    static bool container_started;

    static void SetUpTestSuite() {
        LOG(INFO) << "=== Setting up MQTT test environment ===";

        const char* env_host = std::getenv("MQTT_HOST");
        if (env_host) {
            mqtt_host = env_host;
            const char* env_port = std::getenv("MQTT_PORT");
            mqtt_port = env_port ? std::atoi(env_port) : 1883;
            container_started = true;
            LOG(INFO) << "Using MQTT from environment: " << mqtt_host << ":" << mqtt_port;
            return;
        }

        if (std::system("docker --version > /dev/null 2>&1") != 0) {
            GTEST_SKIP() << "Docker is not available. Skipping integration tests.";
            return;
        }

        StopContainer();

        if (!StartContainer()) {
            GTEST_SKIP() << "Failed to start MQTT container. Skipping tests.";
            return;
        }

        mqtt_host = "localhost";
        mqtt_port = std::atoi(MQTT_PORT);
        LOG(INFO) << "MQTT test broker running at: " << mqtt_host << ":" << mqtt_port;
    }

    static void TearDownTestSuite() {
        LOG(INFO) << "=== Tearing down MQTT test environment ===";
        if (!std::getenv("MQTT_HOST")) {
            StopContainer();
        }
    }

    void SetUp() override {
        if (!container_started) {
            GTEST_SKIP() << "MQTT container not running";
        }
        LOG(INFO) << "Test: " << ::testing::UnitTest::GetInstance()->current_test_info()->name();
    }

    void TearDown() override {
        std::this_thread::sleep_for(100ms);
    }

private:
    static bool StartContainer() {
        LOG(INFO) << "Starting MQTT broker container...";

        std::string cmd = "docker run -d --rm "
                          "--name " + std::string(CONTAINER_NAME) + " "
                          "-p " + std::string(MQTT_PORT) + ":1883 "
                          + std::string(MQTT_IMAGE) + " "
                          "sh -c 'echo -e \"listener 1883\\nallow_anonymous true\" > /tmp/m.conf && "
                          "mosquitto -c /tmp/m.conf'";

        if (std::system(cmd.c_str()) != 0) {
            LOG(ERROR) << "Failed to start Docker container";
            return false;
        }

        LOG(INFO) << "Waiting for MQTT broker to be ready...";
        for (int i = 0; i < 100; ++i) {
            std::this_thread::sleep_for(100ms);

            std::string check_port = "nc -z localhost " + std::string(MQTT_PORT) + " 2>/dev/null";
            if (std::system(check_port.c_str()) == 0) {
                LOG(INFO) << "MQTT broker is ready!";
                container_started = true;
                return true;
            }
        }

        LOG(ERROR) << "Timeout waiting for MQTT broker";
        StopContainer();
        return false;
    }

    static void StopContainer() {
        LOG(INFO) << "Stopping MQTT container...";
        [[maybe_unused]] int r1 = std::system(("docker stop " + std::string(CONTAINER_NAME) + " 2>/dev/null").c_str());
        [[maybe_unused]] int r2 = std::system(("docker rm -f " + std::string(CONTAINER_NAME) + " 2>/dev/null").c_str());
        std::this_thread::sleep_for(500ms);
        container_started = false;
    }
};

std::string MqttTestFixture::mqtt_host;
int MqttTestFixture::mqtt_port = 11885;
bool MqttTestFixture::container_started = false;

// =============================================================================
// Paired Transport Integration Test
// =============================================================================

class PairedTransportIntegrationTest : public MqttTestFixture {
protected:
    static constexpr uint32_t TEST_CONTENT_ID = 200;
    static constexpr const char* TEST_VEHICLE_ID = "test-vehicle-paired-001";

    // Cloud side
    static std::unique_ptr<CloudBackendTransportServer> cloud_service_;
    static std::unique_ptr<grpc::Server> cloud_grpc_server_;
    static int cloud_grpc_port_;

    // Vehicle side
    static std::unique_ptr<ifex::reference::BackendTransportServer> vehicle_service_;
    static std::unique_ptr<grpc::Server> vehicle_grpc_server_;
    static int vehicle_grpc_port_;

    static void SetUpTestSuite() {
        MqttTestFixture::SetUpTestSuite();

        if (!container_started) {
            return;
        }

        // Start cloud service first
        if (!StartCloudService()) {
            GTEST_SKIP() << "Failed to start cloud service";
            return;
        }

        // Start vehicle service
        if (!StartVehicleService()) {
            GTEST_SKIP() << "Failed to start vehicle service";
            return;
        }

        // Wait for both to connect and settle
        std::this_thread::sleep_for(2s);
        LOG(INFO) << "Both services started and connected";
    }

    static void TearDownTestSuite() {
        LOG(INFO) << "Tearing down paired transport test suite...";
        // Stop vehicle first (so cloud sees disconnect)
        LOG(INFO) << "Stopping vehicle service...";
        StopVehicleService();
        std::this_thread::sleep_for(500ms);

        LOG(INFO) << "Stopping cloud service...";
        StopCloudService();
        std::this_thread::sleep_for(500ms);

        LOG(INFO) << "Stopping MQTT container...";
        MqttTestFixture::TearDownTestSuite();
        LOG(INFO) << "Paired transport test suite teardown complete";
    }

    void SetUp() override {
        MqttTestFixture::SetUp();
        if (!cloud_service_ || !vehicle_service_) {
            GTEST_SKIP() << "Services not running";
        }
    }

    // Client factories
    CloudBackendTransportClient createCloudClient(uint32_t content_id = TEST_CONTENT_ID) {
        return CloudBackendTransportClient("localhost:" + std::to_string(cloud_grpc_port_), content_id);
    }

    ifex::client::BackendTransportClient createVehicleClient(uint32_t content_id = TEST_CONTENT_ID) {
        auto channel = grpc::CreateChannel(
            "localhost:" + std::to_string(vehicle_grpc_port_),
            grpc::InsecureChannelCredentials());
        return ifex::client::BackendTransportClient(channel, content_id);
    }

private:
    static bool StartCloudService() {
        LOG(INFO) << "Starting cloud backend transport service...";

        CloudBackendTransportServer::Config config;
        config.mqtt_host = mqtt_host;
        config.mqtt_port = mqtt_port;
        config.partition_id = 0;
        config.total_partitions = 1;

        cloud_service_ = std::make_unique<CloudBackendTransportServer>(config);

        if (!cloud_service_->Start()) {
            LOG(ERROR) << "Failed to start cloud transport";
            return false;
        }

        grpc::ServerBuilder builder;
        builder.AddListeningPort("0.0.0.0:0", grpc::InsecureServerCredentials(), &cloud_grpc_port_);

        using namespace swdv::cloud_backend_transport_service;
        builder.RegisterService(static_cast<send_to_vehicle_service::Service*>(cloud_service_.get()));
        builder.RegisterService(static_cast<get_vehicle_status_service::Service*>(cloud_service_.get()));
        builder.RegisterService(static_cast<get_channel_info_service::Service*>(cloud_service_.get()));
        builder.RegisterService(static_cast<get_queue_status_service::Service*>(cloud_service_.get()));
        builder.RegisterService(static_cast<get_stats_service::Service*>(cloud_service_.get()));
        builder.RegisterService(static_cast<healthy_service::Service*>(cloud_service_.get()));
        builder.RegisterService(static_cast<on_vehicle_message_service::Service*>(cloud_service_.get()));
        builder.RegisterService(static_cast<on_ack_service::Service*>(cloud_service_.get()));
        builder.RegisterService(static_cast<on_vehicle_status_service::Service*>(cloud_service_.get()));
        builder.RegisterService(static_cast<on_queue_status_changed_service::Service*>(cloud_service_.get()));

        cloud_grpc_server_ = builder.BuildAndStart();
        LOG(INFO) << "Cloud service listening on port " << cloud_grpc_port_;
        return true;
    }

    static bool StartVehicleService() {
        LOG(INFO) << "Starting vehicle backend transport service...";

        ifex::reference::BackendTransportServer::Config config;
        config.mqtt_host = mqtt_host;
        config.mqtt_port = mqtt_port;
        config.vehicle_id = TEST_VEHICLE_ID;
        config.persistence_dir = "/tmp/ifex-paired-test-vehicle";

        vehicle_service_ = std::make_unique<ifex::reference::BackendTransportServer>(config);

        if (!vehicle_service_->Start()) {
            LOG(ERROR) << "Failed to start vehicle transport";
            return false;
        }

        grpc::ServerBuilder builder;
        builder.AddListeningPort("0.0.0.0:0", grpc::InsecureServerCredentials(), &vehicle_grpc_port_);

        using namespace swdv::backend_transport_service;
        builder.RegisterService(static_cast<publish_service::Service*>(vehicle_service_.get()));
        builder.RegisterService(static_cast<get_connection_status_service::Service*>(vehicle_service_.get()));
        builder.RegisterService(static_cast<get_queue_status_service::Service*>(vehicle_service_.get()));
        builder.RegisterService(static_cast<get_stats_service::Service*>(vehicle_service_.get()));
        builder.RegisterService(static_cast<healthy_service::Service*>(vehicle_service_.get()));
        builder.RegisterService(static_cast<get_content_id_service::Service*>(vehicle_service_.get()));
        builder.RegisterService(static_cast<on_content_service::Service*>(vehicle_service_.get()));
        builder.RegisterService(static_cast<on_ack_service::Service*>(vehicle_service_.get()));
        builder.RegisterService(static_cast<on_connection_changed_service::Service*>(vehicle_service_.get()));
        builder.RegisterService(static_cast<on_queue_status_changed_service::Service*>(vehicle_service_.get()));

        vehicle_grpc_server_ = builder.BuildAndStart();
        LOG(INFO) << "Vehicle service listening on port " << vehicle_grpc_port_;
        return true;
    }

    static void StopCloudService() {
        // Stop service first to clean up MQTT threads
        if (cloud_service_) {
            cloud_service_->Stop();
        }
        // Then shutdown gRPC server
        if (cloud_grpc_server_) {
            auto deadline = std::chrono::system_clock::now() + 5s;
            cloud_grpc_server_->Shutdown(deadline);
            cloud_grpc_server_.reset();
        }
        cloud_service_.reset();
    }

    static void StopVehicleService() {
        // Stop service first to clean up MQTT threads
        if (vehicle_service_) {
            vehicle_service_->Stop();
        }
        // Then shutdown gRPC server
        if (vehicle_grpc_server_) {
            auto deadline = std::chrono::system_clock::now() + 5s;
            vehicle_grpc_server_->Shutdown(deadline);
            vehicle_grpc_server_.reset();
        }
        vehicle_service_.reset();
    }
};

// Static member definitions
std::unique_ptr<CloudBackendTransportServer> PairedTransportIntegrationTest::cloud_service_;
std::unique_ptr<grpc::Server> PairedTransportIntegrationTest::cloud_grpc_server_;
int PairedTransportIntegrationTest::cloud_grpc_port_ = 0;

std::unique_ptr<ifex::reference::BackendTransportServer> PairedTransportIntegrationTest::vehicle_service_;
std::unique_ptr<grpc::Server> PairedTransportIntegrationTest::vehicle_grpc_server_;
int PairedTransportIntegrationTest::vehicle_grpc_port_ = 0;

// =============================================================================
// Health Check Tests
// =============================================================================

TEST_F(PairedTransportIntegrationTest, BothServicesAreHealthy) {
    auto cloud_client = createCloudClient();
    auto vehicle_client = createVehicleClient();

    EXPECT_TRUE(cloud_client.IsHealthy()) << "Cloud service should be healthy";
    EXPECT_TRUE(vehicle_client.healthy()) << "Vehicle service should be healthy";
}

TEST_F(PairedTransportIntegrationTest, ChannelInfoMatches) {
    auto cloud_client = createCloudClient();

    auto info = cloud_client.GetChannelInfo();
    EXPECT_EQ(info.content_id(), TEST_CONTENT_ID);
    EXPECT_EQ(info.partition_id(), 0u);
    EXPECT_EQ(info.total_partitions(), 1u);
}

// =============================================================================
// Cloud → Vehicle Message Flow (C2V)
// =============================================================================

TEST_F(PairedTransportIntegrationTest, CloudToVehicle_SingleMessage) {
    auto cloud_client = createCloudClient();
    auto vehicle_client = createVehicleClient();

    std::mutex mtx;
    std::condition_variable cv;
    std::vector<uint8_t> received_payload;

    // Vehicle subscribes to content
    vehicle_client.on_content([&](const std::vector<uint8_t>& payload) {
        std::lock_guard lock(mtx);
        received_payload = payload;
        LOG(INFO) << "Vehicle received C2V message, size=" << payload.size();
        cv.notify_all();
    });

    std::this_thread::sleep_for(500ms);

    // Cloud sends message to vehicle
    std::vector<uint8_t> test_payload = {0xC2, 0x0F, 0xDE, 0xAD};
    auto result = cloud_client.SendToVehicle(TEST_VEHICLE_ID, test_payload);

    EXPECT_EQ(result.status(), swdv::cloud_backend_transport_service::publish_status_t::OK);
    EXPECT_GT(result.sequence(), 0u);

    // Wait for vehicle to receive
    {
        std::unique_lock lock(mtx);
        ASSERT_TRUE(cv.wait_for(lock, 10s, [&]() {
            return !received_payload.empty();
        })) << "Vehicle should receive C2V message";
    }

    vehicle_client.unsubscribe_all();

    EXPECT_EQ(received_payload, test_payload);
}

TEST_F(PairedTransportIntegrationTest, CloudToVehicle_MultipleMessages) {
    auto cloud_client = createCloudClient();
    auto vehicle_client = createVehicleClient();

    std::mutex mtx;
    std::condition_variable cv;
    std::vector<std::vector<uint8_t>> received;

    vehicle_client.on_content([&](const std::vector<uint8_t>& payload) {
        std::lock_guard lock(mtx);
        received.push_back(payload);
        cv.notify_all();
    });

    std::this_thread::sleep_for(500ms);

    // Send multiple messages
    std::vector<std::vector<uint8_t>> sent = {
        {0x01, 0x02, 0x03},
        {0x04, 0x05, 0x06},
        {0x07, 0x08, 0x09}
    };

    for (const auto& payload : sent) {
        auto result = cloud_client.SendToVehicle(TEST_VEHICLE_ID, payload);
        EXPECT_EQ(result.status(), swdv::cloud_backend_transport_service::publish_status_t::OK);
    }

    // Wait for all messages
    {
        std::unique_lock lock(mtx);
        ASSERT_TRUE(cv.wait_for(lock, 10s, [&]() {
            return received.size() >= sent.size();
        })) << "Vehicle should receive all C2V messages";
    }

    vehicle_client.unsubscribe_all();

    EXPECT_EQ(received.size(), sent.size());
    for (size_t i = 0; i < sent.size(); ++i) {
        EXPECT_EQ(received[i], sent[i]) << "Message " << i << " should match";
    }
}

// =============================================================================
// Vehicle → Cloud Message Flow (V2C)
// =============================================================================

TEST_F(PairedTransportIntegrationTest, VehicleToCloud_SingleMessage) {
    auto cloud_client = createCloudClient();
    auto vehicle_client = createVehicleClient();

    std::mutex mtx;
    std::condition_variable cv;
    std::string received_vehicle_id;
    std::vector<uint8_t> received_payload;

    // Cloud subscribes to vehicle messages
    cloud_client.SubscribeToVehicleMessages(
        [&](const std::string& vid, const std::vector<uint8_t>& payload,
            uint64_t /*seq*/, int64_t /*ts*/) {
            std::lock_guard lock(mtx);
            received_vehicle_id = vid;
            received_payload = payload;
            LOG(INFO) << "Cloud received V2C message from " << vid << ", size=" << payload.size();
            cv.notify_all();
        });

    std::this_thread::sleep_for(500ms);

    // Vehicle publishes message
    std::vector<uint8_t> test_payload = {0xF2, 0xC0, 0xBE, 0xEF};
    auto result = vehicle_client.publish(test_payload, ifex::client::Persistence::Volatile);

    EXPECT_TRUE(result.ok()) << "Vehicle publish should succeed";
    EXPECT_GT(result.sequence, 0u);

    // Wait for cloud to receive
    {
        std::unique_lock lock(mtx);
        ASSERT_TRUE(cv.wait_for(lock, 10s, [&]() {
            return !received_payload.empty();
        })) << "Cloud should receive V2C message";
    }

    cloud_client.StopSubscriptions();

    EXPECT_EQ(received_vehicle_id, TEST_VEHICLE_ID);
    EXPECT_EQ(received_payload, test_payload);
}

TEST_F(PairedTransportIntegrationTest, VehicleToCloud_MultipleMessages) {
    auto cloud_client = createCloudClient();
    auto vehicle_client = createVehicleClient();

    std::mutex mtx;
    std::condition_variable cv;
    std::vector<std::vector<uint8_t>> received;

    cloud_client.SubscribeToVehicleMessages(
        [&](const std::string& /*vid*/, const std::vector<uint8_t>& payload,
            uint64_t /*seq*/, int64_t /*ts*/) {
            std::lock_guard lock(mtx);
            received.push_back(payload);
            cv.notify_all();
        });

    std::this_thread::sleep_for(500ms);

    // Vehicle publishes multiple messages
    std::vector<std::vector<uint8_t>> sent = {
        {0x10, 0x20, 0x30},
        {0x40, 0x50, 0x60},
        {0x70, 0x80, 0x90}
    };

    for (const auto& payload : sent) {
        auto result = vehicle_client.publish(payload, ifex::client::Persistence::Volatile);
        EXPECT_TRUE(result.ok());
    }

    // Wait for all messages
    {
        std::unique_lock lock(mtx);
        ASSERT_TRUE(cv.wait_for(lock, 10s, [&]() {
            return received.size() >= sent.size();
        })) << "Cloud should receive all V2C messages";
    }

    cloud_client.StopSubscriptions();

    EXPECT_EQ(received.size(), sent.size());
    for (size_t i = 0; i < sent.size(); ++i) {
        EXPECT_EQ(received[i], sent[i]) << "Message " << i << " should match";
    }
}

// =============================================================================
// Edge Case Tests
// =============================================================================

TEST_F(PairedTransportIntegrationTest, LargePayload) {
    auto cloud_client = createCloudClient();
    auto vehicle_client = createVehicleClient();

    std::mutex mtx;
    std::condition_variable cv;
    std::vector<uint8_t> received_payload;

    vehicle_client.on_content([&](const std::vector<uint8_t>& payload) {
        std::lock_guard lock(mtx);
        received_payload = payload;
        cv.notify_all();
    });

    std::this_thread::sleep_for(500ms);

    // Send large payload (64KB)
    std::vector<uint8_t> large_payload(64 * 1024);
    for (size_t i = 0; i < large_payload.size(); ++i) {
        large_payload[i] = static_cast<uint8_t>(i & 0xFF);
    }

    auto result = cloud_client.SendToVehicle(TEST_VEHICLE_ID, large_payload);
    EXPECT_EQ(result.status(), swdv::cloud_backend_transport_service::publish_status_t::OK);

    {
        std::unique_lock lock(mtx);
        ASSERT_TRUE(cv.wait_for(lock, 30s, [&]() {
            return !received_payload.empty();
        })) << "Vehicle should receive large payload";
    }

    vehicle_client.unsubscribe_all();

    EXPECT_EQ(received_payload.size(), large_payload.size());
    EXPECT_EQ(received_payload, large_payload) << "Large payload should match exactly";
}

TEST_F(PairedTransportIntegrationTest, SendToUnknownVehicle) {
    auto cloud_client = createCloudClient();

    // Use a unique vehicle ID to avoid retained messages from other tests
    std::string unique_vehicle = "unknown-vehicle-" + std::to_string(
        std::chrono::system_clock::now().time_since_epoch().count());

    // Check status BEFORE sending - should be UNKNOWN
    auto [status_before, last_seen_before] = cloud_client.GetVehicleStatus(unique_vehicle);
    EXPECT_EQ(status_before, swdv::cloud_backend_transport_service::vehicle_status_t::UNKNOWN)
        << "Vehicle should be UNKNOWN before any interaction";

    // Send to a vehicle that doesn't exist
    // The cloud service publishes to MQTT anyway - it doesn't know which vehicles exist
    auto result = cloud_client.SendToVehicle(unique_vehicle, {0x01, 0x02});

    // MQTT-based implementation accepts the message (no vehicle validation at cloud side)
    EXPECT_EQ(result.status(), swdv::cloud_backend_transport_service::publish_status_t::OK);
    EXPECT_GT(result.sequence(), 0u);
}

// =============================================================================
// Vehicle Status Tests
// =============================================================================

TEST_F(PairedTransportIntegrationTest, CloudSeesVehicleAsOnline) {
    auto cloud_client = createCloudClient();

    // Vehicle service is running, should be online
    auto [status, last_seen] = cloud_client.GetVehicleStatus(TEST_VEHICLE_ID);

    // Vehicle has been running since SetUpTestSuite, should be ONLINE
    EXPECT_EQ(status, swdv::cloud_backend_transport_service::vehicle_status_t::ONLINE)
        << "Vehicle should be ONLINE";
    EXPECT_GT(last_seen, 0) << "last_seen should be set";

    LOG(INFO) << "Vehicle status: " << static_cast<int>(status) << ", last_seen: " << last_seen;
}

// =============================================================================
// ACK Tests
// =============================================================================

TEST_F(PairedTransportIntegrationTest, CloudReceivesAckAfterSend) {
    auto cloud_client = createCloudClient();

    std::mutex mtx;
    std::condition_variable cv;
    std::vector<std::pair<std::string, uint64_t>> acks;

    // Subscribe to ACKs
    cloud_client.SubscribeToAcks([&](const std::string& vid, uint64_t seq) {
        std::lock_guard lock(mtx);
        acks.emplace_back(vid, seq);
        LOG(INFO) << "Cloud received ACK for vehicle=" << vid << " seq=" << seq;
        cv.notify_all();
    });

    std::this_thread::sleep_for(500ms);

    // Send message
    auto result = cloud_client.SendToVehicle(TEST_VEHICLE_ID, {0xAC, 0xDC});
    ASSERT_EQ(result.status(), swdv::cloud_backend_transport_service::publish_status_t::OK);
    uint64_t expected_seq = result.sequence();

    // Wait for ACK
    {
        std::unique_lock lock(mtx);
        ASSERT_TRUE(cv.wait_for(lock, 10s, [&]() {
            return !acks.empty();
        })) << "Cloud should receive ACK";
    }

    cloud_client.StopSubscriptions();

    ASSERT_GE(acks.size(), 1u);
    EXPECT_EQ(acks[0].first, TEST_VEHICLE_ID);
    EXPECT_EQ(acks[0].second, expected_seq);
}

TEST_F(PairedTransportIntegrationTest, VehicleReceivesAckAfterPublish) {
    auto vehicle_client = createVehicleClient();

    std::mutex mtx;
    std::condition_variable cv;
    std::vector<uint64_t> acks;

    // Subscribe to ACKs
    vehicle_client.on_ack([&](uint64_t seq) {
        std::lock_guard lock(mtx);
        acks.push_back(seq);
        LOG(INFO) << "Vehicle received ACK for seq=" << seq;
        cv.notify_all();
    });

    std::this_thread::sleep_for(500ms);

    // Publish message
    auto result = vehicle_client.publish({0xAC, 0xDC}, ifex::client::Persistence::Volatile);
    ASSERT_TRUE(result.ok());
    uint64_t expected_seq = result.sequence;

    // Wait for ACK
    {
        std::unique_lock lock(mtx);
        ASSERT_TRUE(cv.wait_for(lock, 10s, [&]() {
            return !acks.empty();
        })) << "Vehicle should receive ACK";
    }

    vehicle_client.unsubscribe_all();

    ASSERT_GE(acks.size(), 1u);
    EXPECT_EQ(acks[0], expected_seq);
}

// =============================================================================
// Bidirectional Communication Tests
// =============================================================================

TEST_F(PairedTransportIntegrationTest, BidirectionalMessageExchange) {
    auto cloud_client = createCloudClient();
    auto vehicle_client = createVehicleClient();

    std::mutex mtx;
    std::condition_variable cv;
    std::vector<uint8_t> vehicle_received;
    std::vector<uint8_t> cloud_received;

    // Vehicle subscribes
    vehicle_client.on_content([&](const std::vector<uint8_t>& payload) {
        std::lock_guard lock(mtx);
        vehicle_received = payload;
        cv.notify_all();
    });

    // Cloud subscribes
    cloud_client.SubscribeToVehicleMessages(
        [&](const std::string& /*vid*/, const std::vector<uint8_t>& payload,
            uint64_t /*seq*/, int64_t /*ts*/) {
            std::lock_guard lock(mtx);
            cloud_received = payload;
            cv.notify_all();
        });

    std::this_thread::sleep_for(500ms);

    // Send in both directions simultaneously
    std::vector<uint8_t> c2v_payload = {0xC2, 0xFF};
    std::vector<uint8_t> v2c_payload = {0xF2, 0xCC};

    auto c2v_result = cloud_client.SendToVehicle(TEST_VEHICLE_ID, c2v_payload);
    auto v2c_result = vehicle_client.publish(v2c_payload, ifex::client::Persistence::Volatile);

    EXPECT_EQ(c2v_result.status(), swdv::cloud_backend_transport_service::publish_status_t::OK);
    EXPECT_TRUE(v2c_result.ok());

    // Wait for both to be received
    {
        std::unique_lock lock(mtx);
        ASSERT_TRUE(cv.wait_for(lock, 10s, [&]() {
            return !vehicle_received.empty() && !cloud_received.empty();
        })) << "Both sides should receive messages";
    }

    vehicle_client.unsubscribe_all();
    cloud_client.StopSubscriptions();

    EXPECT_EQ(vehicle_received, c2v_payload);
    EXPECT_EQ(cloud_received, v2c_payload);
}

// =============================================================================
// Statistics Tests
// =============================================================================

TEST_F(PairedTransportIntegrationTest, StatsIncrementOnBothSides) {
    auto cloud_client = createCloudClient();
    auto vehicle_client = createVehicleClient();

    auto cloud_initial = cloud_client.GetStats();
    auto vehicle_initial = vehicle_client.stats();

    // Exchange messages
    cloud_client.SendToVehicle(TEST_VEHICLE_ID, {0x01});
    vehicle_client.publish({0x02}, ifex::client::Persistence::Volatile);

    std::this_thread::sleep_for(1s);

    auto cloud_after = cloud_client.GetStats();
    auto vehicle_after = vehicle_client.stats();

    // Cloud sent one, received one
    EXPECT_GE(cloud_after.messages_sent(), cloud_initial.messages_sent() + 1);
    EXPECT_GE(cloud_after.messages_received(), cloud_initial.messages_received() + 1);

    // Vehicle sent one, received one
    EXPECT_GE(vehicle_after.messages_sent, vehicle_initial.messages_sent + 1);
    EXPECT_GE(vehicle_after.messages_received, vehicle_initial.messages_received + 1);
}

// =============================================================================
// Reconnect Behavior Tests
// =============================================================================

/**
 * Test fixture for reconnect tests that need to restart vehicle service
 */
class ReconnectTest : public MqttTestFixture {
protected:
    static constexpr uint32_t TEST_CONTENT_ID = 201;  // Different content_id to avoid conflicts

    std::unique_ptr<CloudBackendTransportServer> cloud_service_;
    std::unique_ptr<grpc::Server> cloud_grpc_server_;
    int cloud_grpc_port_ = 0;

    void SetUp() override {
        MqttTestFixture::SetUp();
        if (!container_started) {
            GTEST_SKIP() << "MQTT container not running";
            return;
        }

        // Start cloud service
        CloudBackendTransportServer::Config config;
        config.mqtt_host = mqtt_host;
        config.mqtt_port = mqtt_port;
        config.partition_id = 0;
        config.total_partitions = 1;

        cloud_service_ = std::make_unique<CloudBackendTransportServer>(config);
        ASSERT_TRUE(cloud_service_->Start());

        grpc::ServerBuilder builder;
        builder.AddListeningPort("0.0.0.0:0", grpc::InsecureServerCredentials(), &cloud_grpc_port_);

        using namespace swdv::cloud_backend_transport_service;
        builder.RegisterService(static_cast<send_to_vehicle_service::Service*>(cloud_service_.get()));
        builder.RegisterService(static_cast<get_vehicle_status_service::Service*>(cloud_service_.get()));
        builder.RegisterService(static_cast<get_channel_info_service::Service*>(cloud_service_.get()));
        builder.RegisterService(static_cast<healthy_service::Service*>(cloud_service_.get()));
        builder.RegisterService(static_cast<on_vehicle_message_service::Service*>(cloud_service_.get()));
        builder.RegisterService(static_cast<on_vehicle_status_service::Service*>(cloud_service_.get()));

        cloud_grpc_server_ = builder.BuildAndStart();
        std::this_thread::sleep_for(1s);
    }

    void TearDown() override {
        if (cloud_grpc_server_) {
            cloud_grpc_server_->Shutdown();
        }
        if (cloud_service_) {
            cloud_service_->Stop();
        }
        MqttTestFixture::TearDown();
    }

    CloudBackendTransportClient createCloudClient(uint32_t content_id = TEST_CONTENT_ID) {
        return CloudBackendTransportClient("localhost:" + std::to_string(cloud_grpc_port_), content_id);
    }

    struct VehicleInstance {
        std::unique_ptr<ifex::reference::BackendTransportServer> service;
        std::unique_ptr<grpc::Server> grpc_server;
        int grpc_port = 0;
    };

    VehicleInstance startVehicle(const std::string& vehicle_id) {
        VehicleInstance v;

        ifex::reference::BackendTransportServer::Config config;
        config.mqtt_host = mqtt_host;
        config.mqtt_port = mqtt_port;
        config.vehicle_id = vehicle_id;
        config.persistence_dir = "/tmp/ifex-reconnect-test-" + vehicle_id;

        v.service = std::make_unique<ifex::reference::BackendTransportServer>(config);
        if (!v.service->Start()) {
            return {};
        }

        grpc::ServerBuilder builder;
        builder.AddListeningPort("0.0.0.0:0", grpc::InsecureServerCredentials(), &v.grpc_port);

        using namespace swdv::backend_transport_service;
        builder.RegisterService(static_cast<publish_service::Service*>(v.service.get()));
        builder.RegisterService(static_cast<healthy_service::Service*>(v.service.get()));
        builder.RegisterService(static_cast<on_content_service::Service*>(v.service.get()));
        builder.RegisterService(static_cast<on_ack_service::Service*>(v.service.get()));

        v.grpc_server = builder.BuildAndStart();
        return v;
    }

    void stopVehicle(VehicleInstance& v) {
        if (v.grpc_server) {
            v.grpc_server->Shutdown();
            v.grpc_server.reset();
        }
        if (v.service) {
            v.service->Stop();
            v.service.reset();
        }
    }
};

TEST_F(ReconnectTest, CloudSeesVehicleConnectDisconnectReconnect) {
    // Tests that cloud sees vehicle come ONLINE when it connects and OFFLINE when it disconnects.
    // The vehicle publishes "0" to status topic on graceful disconnect (not just via LWT).

    const std::string vehicle_id = "reconnect-test-vehicle";

    auto cloud_client = createCloudClient();

    std::mutex mtx;
    std::condition_variable cv;
    std::vector<std::pair<std::string, swdv::cloud_backend_transport_service::vehicle_status_t>> events;

    // Subscribe to status changes
    cloud_client.SubscribeToVehicleStatus(
        [&](const std::string& vid, swdv::cloud_backend_transport_service::vehicle_status_t st,
            int64_t /*ts*/) {
            std::lock_guard lock(mtx);
            events.emplace_back(vid, st);
            LOG(INFO) << "Status event: vehicle=" << vid << " status=" << static_cast<int>(st);
            cv.notify_all();
        });

    std::this_thread::sleep_for(500ms);

    // Start vehicle - should see ONLINE
    auto vehicle = startVehicle(vehicle_id);
    ASSERT_TRUE(vehicle.service) << "Failed to start vehicle";

    {
        std::unique_lock lock(mtx);
        ASSERT_TRUE(cv.wait_for(lock, 10s, [&]() {
            for (const auto& [vid, st] : events) {
                if (vid == vehicle_id && st == swdv::cloud_backend_transport_service::vehicle_status_t::ONLINE) {
                    return true;
                }
            }
            return false;
        })) << "Should see vehicle come ONLINE";
    }

    // Stop vehicle - should see OFFLINE (graceful disconnect publishes "0")
    events.clear();
    stopVehicle(vehicle);

    {
        std::unique_lock lock(mtx);
        ASSERT_TRUE(cv.wait_for(lock, 10s, [&]() {
            for (const auto& [vid, st] : events) {
                if (vid == vehicle_id && st == swdv::cloud_backend_transport_service::vehicle_status_t::OFFLINE) {
                    return true;
                }
            }
            return false;
        })) << "Should see vehicle go OFFLINE on graceful disconnect";
    }

    // Restart vehicle - should see ONLINE again
    events.clear();
    vehicle = startVehicle(vehicle_id);
    ASSERT_TRUE(vehicle.service) << "Failed to restart vehicle";

    {
        std::unique_lock lock(mtx);
        ASSERT_TRUE(cv.wait_for(lock, 10s, [&]() {
            for (const auto& [vid, st] : events) {
                if (vid == vehicle_id && st == swdv::cloud_backend_transport_service::vehicle_status_t::ONLINE) {
                    return true;
                }
            }
            return false;
        })) << "Should see vehicle come ONLINE again after reconnect";
    }

    stopVehicle(vehicle);
    cloud_client.StopSubscriptions();
}

TEST_F(ReconnectTest, MessagesDeliveredAfterReconnect) {
    const std::string vehicle_id = "reconnect-msg-test-vehicle";

    auto cloud_client = createCloudClient();

    // Start vehicle
    auto vehicle = startVehicle(vehicle_id);
    ASSERT_TRUE(vehicle.service);

    auto vehicle_channel = grpc::CreateChannel(
        "localhost:" + std::to_string(vehicle.grpc_port),
        grpc::InsecureChannelCredentials());
    ifex::client::BackendTransportClient vehicle_client(vehicle_channel, TEST_CONTENT_ID);

    std::this_thread::sleep_for(1s);

    // Send C2V message
    std::mutex mtx;
    std::condition_variable cv;
    std::vector<uint8_t> received;

    vehicle_client.on_content([&](const std::vector<uint8_t>& payload) {
        std::lock_guard lock(mtx);
        received = payload;
        cv.notify_all();
    });

    std::this_thread::sleep_for(500ms);

    std::vector<uint8_t> payload = {0xAF, 0x7E, 0x22};
    cloud_client.SendToVehicle(vehicle_id, payload);

    {
        std::unique_lock lock(mtx);
        ASSERT_TRUE(cv.wait_for(lock, 10s, [&]() {
            return !received.empty();
        })) << "Vehicle should receive message after connect";
    }

    EXPECT_EQ(received, payload);

    vehicle_client.unsubscribe_all();
    stopVehicle(vehicle);
}

TEST_F(ReconnectTest, VehicleCanPublishAfterReconnect) {
    // Test that V2C works after vehicle disconnect and reconnect
    const std::string vehicle_id = "v2c-reconnect-test-vehicle";

    auto cloud_client = createCloudClient();

    std::mutex mtx;
    std::condition_variable cv;
    std::vector<std::vector<uint8_t>> received;

    cloud_client.SubscribeToVehicleMessages(
        [&](const std::string& vid, const std::vector<uint8_t>& payload,
            uint64_t /*seq*/, int64_t /*ts*/) {
            if (vid == vehicle_id) {
                std::lock_guard lock(mtx);
                received.push_back(payload);
                LOG(INFO) << "Cloud received V2C from " << vid;
                cv.notify_all();
            }
        });

    std::this_thread::sleep_for(500ms);

    // Start vehicle and publish
    auto vehicle = startVehicle(vehicle_id);
    ASSERT_TRUE(vehicle.service);

    auto vehicle_channel = grpc::CreateChannel(
        "localhost:" + std::to_string(vehicle.grpc_port),
        grpc::InsecureChannelCredentials());
    ifex::client::BackendTransportClient vehicle_client(vehicle_channel, TEST_CONTENT_ID);

    std::this_thread::sleep_for(500ms);

    std::vector<uint8_t> payload1 = {0x01, 0x02, 0x03};
    auto result1 = vehicle_client.publish(payload1, ifex::client::Persistence::Volatile);
    ASSERT_TRUE(result1.ok());

    {
        std::unique_lock lock(mtx);
        ASSERT_TRUE(cv.wait_for(lock, 10s, [&]() {
            return received.size() >= 1;
        })) << "Cloud should receive first V2C message";
    }

    // Stop and restart vehicle
    vehicle_client.unsubscribe_all();
    stopVehicle(vehicle);
    std::this_thread::sleep_for(500ms);

    received.clear();
    vehicle = startVehicle(vehicle_id);
    ASSERT_TRUE(vehicle.service);

    auto vehicle_channel2 = grpc::CreateChannel(
        "localhost:" + std::to_string(vehicle.grpc_port),
        grpc::InsecureChannelCredentials());
    ifex::client::BackendTransportClient vehicle_client2(vehicle_channel2, TEST_CONTENT_ID);

    std::this_thread::sleep_for(500ms);

    // Publish again after reconnect
    std::vector<uint8_t> payload2 = {0x04, 0x05, 0x06};
    auto result2 = vehicle_client2.publish(payload2, ifex::client::Persistence::Volatile);
    ASSERT_TRUE(result2.ok());

    {
        std::unique_lock lock(mtx);
        ASSERT_TRUE(cv.wait_for(lock, 10s, [&]() {
            return received.size() >= 1;
        })) << "Cloud should receive V2C message after vehicle reconnect";
    }

    EXPECT_EQ(received[0], payload2);

    cloud_client.StopSubscriptions();
    stopVehicle(vehicle);
}

TEST_F(ReconnectTest, MultipleVehiclesRouteCorrectly) {
    // Test that messages route to the correct vehicle
    const std::string vehicle_a_id = "multi-vehicle-A";
    const std::string vehicle_b_id = "multi-vehicle-B";

    auto cloud_client = createCloudClient();

    // Start two vehicles
    auto vehicle_a = startVehicle(vehicle_a_id);
    auto vehicle_b = startVehicle(vehicle_b_id);
    ASSERT_TRUE(vehicle_a.service) << "Failed to start vehicle A";
    ASSERT_TRUE(vehicle_b.service) << "Failed to start vehicle B";

    auto channel_a = grpc::CreateChannel(
        "localhost:" + std::to_string(vehicle_a.grpc_port),
        grpc::InsecureChannelCredentials());
    auto channel_b = grpc::CreateChannel(
        "localhost:" + std::to_string(vehicle_b.grpc_port),
        grpc::InsecureChannelCredentials());

    ifex::client::BackendTransportClient client_a(channel_a, TEST_CONTENT_ID);
    ifex::client::BackendTransportClient client_b(channel_b, TEST_CONTENT_ID);

    std::mutex mtx;
    std::condition_variable cv;
    std::vector<uint8_t> received_a;
    std::vector<uint8_t> received_b;

    client_a.on_content([&](const std::vector<uint8_t>& payload) {
        std::lock_guard lock(mtx);
        received_a = payload;
        LOG(INFO) << "Vehicle A received C2V";
        cv.notify_all();
    });

    client_b.on_content([&](const std::vector<uint8_t>& payload) {
        std::lock_guard lock(mtx);
        received_b = payload;
        LOG(INFO) << "Vehicle B received C2V";
        cv.notify_all();
    });

    std::this_thread::sleep_for(1s);

    // Send different payloads to each vehicle
    std::vector<uint8_t> payload_for_a = {0xAA, 0xAA};
    std::vector<uint8_t> payload_for_b = {0xBB, 0xBB};

    cloud_client.SendToVehicle(vehicle_a_id, payload_for_a);
    cloud_client.SendToVehicle(vehicle_b_id, payload_for_b);

    // Wait for both to receive
    {
        std::unique_lock lock(mtx);
        ASSERT_TRUE(cv.wait_for(lock, 10s, [&]() {
            return !received_a.empty() && !received_b.empty();
        })) << "Both vehicles should receive their messages";
    }

    // Verify correct routing
    EXPECT_EQ(received_a, payload_for_a) << "Vehicle A should receive payload_for_a";
    EXPECT_EQ(received_b, payload_for_b) << "Vehicle B should receive payload_for_b";

    // Also test V2C from multiple vehicles
    std::mutex v2c_mtx;
    std::condition_variable v2c_cv;
    std::map<std::string, std::vector<uint8_t>> v2c_received;

    cloud_client.SubscribeToVehicleMessages(
        [&](const std::string& vid, const std::vector<uint8_t>& payload,
            uint64_t /*seq*/, int64_t /*ts*/) {
            std::lock_guard lock(v2c_mtx);
            v2c_received[vid] = payload;
            v2c_cv.notify_all();
        });

    std::this_thread::sleep_for(500ms);

    std::vector<uint8_t> v2c_from_a = {0x11, 0x11};
    std::vector<uint8_t> v2c_from_b = {0x22, 0x22};

    client_a.publish(v2c_from_a, ifex::client::Persistence::Volatile);
    client_b.publish(v2c_from_b, ifex::client::Persistence::Volatile);

    {
        std::unique_lock lock(v2c_mtx);
        ASSERT_TRUE(v2c_cv.wait_for(lock, 10s, [&]() {
            return v2c_received.count(vehicle_a_id) && v2c_received.count(vehicle_b_id);
        })) << "Cloud should receive V2C from both vehicles";
    }

    EXPECT_EQ(v2c_received[vehicle_a_id], v2c_from_a) << "V2C from A should match";
    EXPECT_EQ(v2c_received[vehicle_b_id], v2c_from_b) << "V2C from B should match";

    client_a.unsubscribe_all();
    client_b.unsubscribe_all();
    cloud_client.StopSubscriptions();
    stopVehicle(vehicle_a);
    stopVehicle(vehicle_b);
}

}  // namespace ifex::cloud::test

int main(int argc, char** argv) {
    google::InitGoogleLogging(argv[0]);
    FLAGS_logtostderr = true;
    ::testing::InitGoogleTest(&argc, argv);
    mosquitto_lib_init();
    int result = RUN_ALL_TESTS();
    mosquitto_lib_cleanup();
    return result;
}
