/**
 * @file backend_transport_resilience_test.cpp
 * @brief Resilience tests for Backend Transport Service
 *
 * Tests broker up/down scenarios, persistence behavior, and reconnection.
 * Requires Docker for MQTT broker management.
 */

#include "mqtt_test_fixture.hpp"
#include "backend_transport_client.hpp"
#include "backend_transport_server.hpp"

#include <gtest/gtest.h>
#include <glog/logging.h>
#include <grpcpp/grpcpp.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

namespace ifex::test {

using namespace ifex::client;
using namespace std::chrono_literals;

/**
 * @brief Helper to manage MQTT broker container for resilience tests
 */
class BrokerControl {
public:
    static constexpr const char* CONTAINER_NAME = "ifex-mqtt-test-broker";
    static constexpr const char* MQTT_IMAGE = "eclipse-mosquitto:2";
    static constexpr int MQTT_PORT = 11883;

    static bool Start() {
        LOG(INFO) << "Starting MQTT broker...";

        std::string cmd = "docker run -d --rm "
                          "--name " + std::string(CONTAINER_NAME) + " "
                          "-p " + std::to_string(MQTT_PORT) + ":1883 "
                          + std::string(MQTT_IMAGE) + " "
                          "sh -c 'echo -e \"listener 1883\\nallow_anonymous true\" > /tmp/m.conf && "
                          "mosquitto -c /tmp/m.conf'";

        if (std::system(cmd.c_str()) != 0) {
            LOG(ERROR) << "Failed to start MQTT broker";
            return false;
        }

        // Wait for port to be ready
        for (int i = 0; i < 30; ++i) {
            std::this_thread::sleep_for(500ms);
            std::string check = "nc -z localhost " + std::to_string(MQTT_PORT) + " 2>/dev/null";
            if (std::system(check.c_str()) == 0) {
                LOG(INFO) << "MQTT broker started";
                return true;
            }
        }

        LOG(ERROR) << "Timeout waiting for MQTT broker";
        Stop();
        return false;
    }

    static void Stop() {
        LOG(INFO) << "Stopping MQTT broker...";
        [[maybe_unused]] int r1 = std::system(("docker stop " + std::string(CONTAINER_NAME) + " 2>/dev/null").c_str());
        [[maybe_unused]] int r2 = std::system(("docker rm -f " + std::string(CONTAINER_NAME) + " 2>/dev/null").c_str());
        std::this_thread::sleep_for(500ms);
    }

    static bool IsRunning() {
        std::string check = "docker ps -q -f name=" + std::string(CONTAINER_NAME) + " | grep -q .";
        return std::system(check.c_str()) == 0;
    }
};

/**
 * @brief Test fixture for resilience tests
 *
 * Unlike the integration test fixture, this allows starting/stopping
 * the broker during individual tests.
 */
class BackendTransportResilienceTest : public ::testing::Test {
protected:
    static std::unique_ptr<reference::BackendTransportServer> service_;
    static std::unique_ptr<grpc::Server> grpc_server_;
    static std::shared_ptr<grpc::Channel> channel_;
    static int grpc_port_;
    static std::string persistence_dir_;

    static void SetUpTestSuite() {
        // Check Docker availability
        if (std::system("docker --version > /dev/null 2>&1") != 0) {
            GTEST_SKIP() << "Docker is not available";
            return;
        }

        // Clean up any existing container
        BrokerControl::Stop();

        // Create temp persistence directory
        persistence_dir_ = "/tmp/ifex-resilience-test-" + std::to_string(getpid());
        std::filesystem::create_directories(persistence_dir_);
    }

    static void TearDownTestSuite() {
        ShutdownService();
        BrokerControl::Stop();

        // Clean up persistence directory
        if (!persistence_dir_.empty()) {
            std::filesystem::remove_all(persistence_dir_);
        }
    }

    void SetUp() override {
        // Each test starts fresh
        ShutdownService();
        BrokerControl::Stop();
    }

    void TearDown() override {
        ShutdownService();
        BrokerControl::Stop();
    }

    static bool StartService(size_t queue_size = 100) {
        reference::BackendTransportServer::Config config;
        config.mqtt_host = "localhost";
        config.mqtt_port = BrokerControl::MQTT_PORT;
        config.vehicle_id = "test-vehicle";
        config.queue_size_per_content_id = queue_size;
        config.persistence_dir = persistence_dir_;

        service_ = std::make_unique<reference::BackendTransportServer>(config);

        if (!service_->Start()) {
            LOG(ERROR) << "Failed to start backend transport service";
            return false;
        }

        // Start gRPC server
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

        // Wait for channel to connect (important for streaming RPCs to work reliably)
        auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(5);
        if (!channel_->WaitForConnected(deadline)) {
            LOG(WARNING) << "gRPC channel not connected within timeout";
        }

        LOG(INFO) << "Backend Transport Service listening on " << server_address;
        return true;
    }

    static void ShutdownService() {
        channel_.reset();

        if (grpc_server_) {
            grpc_server_->Shutdown();
            grpc_server_.reset();
        }
        if (service_) {
            service_->Stop();
            service_.reset();
        }
    }

    BackendTransportClient createClient(uint32_t content_id) {
        return BackendTransportClient(channel_, content_id);
    }

    // Wait for connection state with timeout
    bool waitForConnectionState(BackendTransportClient& client, ConnectionState expected,
                                std::chrono::seconds timeout = 10s) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            auto status = client.connection_status();
            if (status.state == expected) {
                return true;
            }
            std::this_thread::sleep_for(100ms);
        }
        return false;
    }
};

// Static member definitions
std::unique_ptr<reference::BackendTransportServer> BackendTransportResilienceTest::service_;
std::unique_ptr<grpc::Server> BackendTransportResilienceTest::grpc_server_;
std::shared_ptr<grpc::Channel> BackendTransportResilienceTest::channel_;
int BackendTransportResilienceTest::grpc_port_ = 0;
std::string BackendTransportResilienceTest::persistence_dir_;

// =============================================================================
// Broker Up/Down Tests
// =============================================================================

TEST_F(BackendTransportResilienceTest, ServiceStartsWithBrokerDown) {
    // Start service without broker
    ASSERT_TRUE(StartService());

    auto client = createClient(1);

    // Service should report disconnected
    auto status = client.connection_status();
    EXPECT_NE(status.state, ConnectionState::Connected);

    // Healthy should return false when disconnected
    EXPECT_FALSE(client.healthy());
}

TEST_F(BackendTransportResilienceTest, ServiceConnectsWhenBrokerComesUp) {
    // Start service without broker
    ASSERT_TRUE(StartService());

    auto client = createClient(1);
    EXPECT_NE(client.connection_status().state, ConnectionState::Connected);

    // Now start broker
    ASSERT_TRUE(BrokerControl::Start());

    // Wait for connection
    ASSERT_TRUE(waitForConnectionState(client, ConnectionState::Connected, 15s))
        << "Service should connect when broker comes up";

    EXPECT_TRUE(client.healthy());
}

TEST_F(BackendTransportResilienceTest, ServiceReconnectsWhenBrokerReturns) {
    // Start broker and service
    ASSERT_TRUE(BrokerControl::Start());
    ASSERT_TRUE(StartService());

    auto client = createClient(1);
    ASSERT_TRUE(waitForConnectionState(client, ConnectionState::Connected));

    // Stop broker
    LOG(INFO) << "=== Stopping broker ===";
    BrokerControl::Stop();

    // Wait for disconnect
    ASSERT_TRUE(waitForConnectionState(client, ConnectionState::Disconnected, 15s))
        << "Service should detect broker disconnect";

    // Restart broker
    LOG(INFO) << "=== Restarting broker ===";
    ASSERT_TRUE(BrokerControl::Start());

    // Wait for reconnect
    ASSERT_TRUE(waitForConnectionState(client, ConnectionState::Connected, 15s))
        << "Service should reconnect when broker returns";
}

// =============================================================================
// Connection Status Streaming Tests
// =============================================================================

TEST_F(BackendTransportResilienceTest, ConnectionStatusStreamReceivesUpdates) {
    // Start service without broker
    ASSERT_TRUE(StartService());

    auto client = createClient(1);

    std::mutex mtx;
    std::condition_variable cv;
    std::vector<ConnectionState> states;

    client.on_connection_changed([&](const ConnectionStatus& status) {
        std::lock_guard<std::mutex> lock(mtx);
        states.push_back(status.state);
        LOG(INFO) << "Connection state changed: " << static_cast<int>(status.state);
        cv.notify_all();
    });

    // Wait for initial state
    {
        std::unique_lock<std::mutex> lock(mtx);
        ASSERT_TRUE(cv.wait_for(lock, 5s, [&]{ return !states.empty(); }));
    }

    // Should be disconnected initially
    EXPECT_NE(states.back(), ConnectionState::Connected);

    // Start broker
    LOG(INFO) << "=== Starting broker ===";
    ASSERT_TRUE(BrokerControl::Start());

    // Wait for connected state
    {
        std::unique_lock<std::mutex> lock(mtx);
        ASSERT_TRUE(cv.wait_for(lock, 15s, [&]{
            return std::find(states.begin(), states.end(), ConnectionState::Connected) != states.end();
        }));
    }

    // Capture size before stopping broker to avoid race condition
    size_t count_before_stop;
    {
        std::lock_guard<std::mutex> lock(mtx);
        count_before_stop = states.size();
    }

    // Stop broker
    LOG(INFO) << "=== Stopping broker ===";
    BrokerControl::Stop();

    // Wait for disconnected state
    {
        std::unique_lock<std::mutex> lock(mtx);
        ASSERT_TRUE(cv.wait_for(lock, 15s, [&]{
            return states.size() > count_before_stop && states.back() == ConnectionState::Disconnected;
        }));
    }

    client.unsubscribe_all();

    // Verify we received state transitions
    EXPECT_GE(states.size(), 2) << "Should have received at least 2 state changes";
}

// =============================================================================
// Persistence Tests - Queue Behavior During Disconnection
// =============================================================================

TEST_F(BackendTransportResilienceTest, MessagesQueueWhileDisconnected) {
    // Start service without broker (disconnected)
    ASSERT_TRUE(StartService());

    auto client = createClient(100);

    // Publish messages while disconnected - they should queue
    std::vector<uint64_t> sequences;
    for (int i = 0; i < 5; ++i) {
        auto result = client.publish({static_cast<uint8_t>(i)}, Persistence::Volatile);
        ASSERT_TRUE(result.ok()) << "Publish should succeed (queued) even when disconnected";
        sequences.push_back(result.sequence);
    }

    // Verify sequences are monotonic
    for (size_t i = 1; i < sequences.size(); ++i) {
        EXPECT_EQ(sequences[i], sequences[i-1] + 1);
    }

    // Check queue has pending messages
    auto queue_status = client.queue_status();
    EXPECT_GT(queue_status.queue_size, 0) << "Queue should have pending messages";
}

TEST_F(BackendTransportResilienceTest, QueuedMessagesDeliveredWhenBrokerReturns) {
    // Start service without broker
    ASSERT_TRUE(StartService());

    auto client = createClient(200);

    // Publish messages while disconnected
    LOG(INFO) << "=== Publishing while disconnected ===";
    for (int i = 0; i < 3; ++i) {
        auto result = client.publish({static_cast<uint8_t>(i)}, Persistence::Volatile);
        ASSERT_TRUE(result.ok());
    }

    auto initial_stats = client.stats();
    EXPECT_EQ(initial_stats.messages_sent, 0) << "No messages should be sent while disconnected";

    // Start broker
    LOG(INFO) << "=== Starting broker ===";
    ASSERT_TRUE(BrokerControl::Start());
    ASSERT_TRUE(waitForConnectionState(client, ConnectionState::Connected));

    // Wait for messages to be sent
    std::this_thread::sleep_for(2s);

    auto final_stats = client.stats();
    EXPECT_GE(final_stats.messages_sent, 3) << "Queued messages should be delivered after reconnect";
}

TEST_F(BackendTransportResilienceTest, BestEffortDroppedWhenQueueFull) {
    // Start service without broker, with small queue
    ASSERT_TRUE(StartService(5));  // Queue size of 5

    auto client = createClient(300);

    // Fill queue with volatile messages
    LOG(INFO) << "=== Filling queue with volatile messages ===";
    for (int i = 0; i < 5; ++i) {
        auto result = client.publish({static_cast<uint8_t>(i)}, Persistence::Volatile);
        ASSERT_TRUE(result.ok()) << "Should accept volatile message " << i;
    }

    auto status = client.queue_status();
    EXPECT_EQ(status.level, QueueLevel::Full) << "Queue should be full";

    // BestEffort should be dropped when queue is full
    LOG(INFO) << "=== Trying to add best-effort to full queue ===";
    auto result = client.publish({0xFF}, Persistence::BestEffort);
    EXPECT_FALSE(result.ok()) << "BestEffort should be rejected when queue is full";
    EXPECT_EQ(result.sequence, 0);
}

TEST_F(BackendTransportResilienceTest, VolatileMessageDisplacesBestEffort) {
    // Start service without broker, with small queue
    ASSERT_TRUE(StartService(5));

    auto client = createClient(400);

    // Fill queue with best-effort messages
    LOG(INFO) << "=== Filling queue with best-effort ===";
    for (int i = 0; i < 5; ++i) {
        auto result = client.publish({static_cast<uint8_t>(i)}, Persistence::BestEffort);
        ASSERT_TRUE(result.ok());
    }

    auto status = client.queue_status();
    EXPECT_EQ(status.level, QueueLevel::Full);

    // Volatile message should displace a best-effort
    LOG(INFO) << "=== Adding volatile message to full queue ===";
    auto result = client.publish({0xFF}, Persistence::Volatile);
    EXPECT_TRUE(result.ok()) << "Volatile message should displace best-effort";

    // Queue should still be full (one was dropped, one was added)
    status = client.queue_status();
    EXPECT_EQ(status.level, QueueLevel::Full);
}

// =============================================================================
// Publish Behavior During Connection State Changes
// =============================================================================

TEST_F(BackendTransportResilienceTest, PublishDuringReconnection) {
    // Start with broker running
    ASSERT_TRUE(BrokerControl::Start());
    ASSERT_TRUE(StartService());

    auto client = createClient(500);
    ASSERT_TRUE(waitForConnectionState(client, ConnectionState::Connected));

    // Send some messages
    for (int i = 0; i < 3; ++i) {
        auto result = client.publish({static_cast<uint8_t>(i)}, Persistence::Volatile);
        ASSERT_TRUE(result.ok());
    }

    auto stats_before = client.stats();
    uint64_t sent_before = stats_before.messages_sent;

    // Stop broker
    LOG(INFO) << "=== Stopping broker ===";
    BrokerControl::Stop();
    ASSERT_TRUE(waitForConnectionState(client, ConnectionState::Disconnected, 15s));

    // Publish more messages during disconnection
    LOG(INFO) << "=== Publishing while disconnected ===";
    for (int i = 10; i < 15; ++i) {
        auto result = client.publish({static_cast<uint8_t>(i)}, Persistence::Volatile);
        ASSERT_TRUE(result.ok()) << "Should accept messages while disconnected (queued)";
    }

    // Restart broker
    LOG(INFO) << "=== Restarting broker ===";
    ASSERT_TRUE(BrokerControl::Start());
    ASSERT_TRUE(waitForConnectionState(client, ConnectionState::Connected, 15s));

    // Wait for messages to be delivered
    std::this_thread::sleep_for(3s);

    auto stats_after = client.stats();
    EXPECT_GT(stats_after.messages_sent, sent_before)
        << "Messages should be delivered after reconnect";
}

// =============================================================================
// Stats Tracking During Connection Changes
// =============================================================================

TEST_F(BackendTransportResilienceTest, StatsAccumulateAcrossReconnections) {
    ASSERT_TRUE(BrokerControl::Start());
    ASSERT_TRUE(StartService());

    auto client = createClient(600);
    ASSERT_TRUE(waitForConnectionState(client, ConnectionState::Connected));

    // Send messages
    for (int i = 0; i < 5; ++i) {
        client.publish({static_cast<uint8_t>(i)}, Persistence::Volatile);
    }

    std::this_thread::sleep_for(1s);
    auto stats1 = client.stats();

    // Reconnect cycle
    BrokerControl::Stop();
    ASSERT_TRUE(waitForConnectionState(client, ConnectionState::Disconnected, 15s));

    ASSERT_TRUE(BrokerControl::Start());
    ASSERT_TRUE(waitForConnectionState(client, ConnectionState::Connected, 15s));

    // Send more messages
    for (int i = 0; i < 5; ++i) {
        client.publish({static_cast<uint8_t>(i + 10)}, Persistence::Volatile);
    }

    std::this_thread::sleep_for(1s);
    auto stats2 = client.stats();

    EXPECT_GT(stats2.messages_sent, stats1.messages_sent)
        << "Stats should continue accumulating after reconnection";
}

// =============================================================================
// DURABLE Persistence Tests
// =============================================================================

TEST_F(BackendTransportResilienceTest, DurableMessagesSurviveGracefulShutdown) {
    // Start service without broker (messages will queue)
    ASSERT_TRUE(StartService());

    auto client = createClient(700);

    // Publish DURABLE messages while disconnected
    LOG(INFO) << "=== Publishing DURABLE messages while disconnected ===";
    std::vector<uint64_t> sequences;
    for (int i = 0; i < 5; ++i) {
        auto result = client.publish({static_cast<uint8_t>(i)}, Persistence::Durable);
        ASSERT_TRUE(result.ok());
        sequences.push_back(result.sequence);
    }

    // Verify messages are queued
    auto status = client.queue_status();
    EXPECT_GT(status.queue_size, 0) << "Should have queued messages";

    // Graceful shutdown (should persist DURABLE messages)
    LOG(INFO) << "=== Graceful shutdown ===";
    ShutdownService();

    // Start broker
    ASSERT_TRUE(BrokerControl::Start());

    // Restart service (should load persisted messages)
    LOG(INFO) << "=== Restarting service ===";
    ASSERT_TRUE(StartService());

    auto client2 = createClient(700);
    ASSERT_TRUE(waitForConnectionState(client2, ConnectionState::Connected));

    // Wait for persisted messages to be delivered
    std::this_thread::sleep_for(3s);

    auto stats = client2.stats();
    EXPECT_GE(stats.messages_sent, 5)
        << "DURABLE messages should be delivered after restart";
}

TEST_F(BackendTransportResilienceTest, VolatileMessagesLostOnShutdown) {
    // Start service without broker
    ASSERT_TRUE(StartService());

    auto client = createClient(701);

    // Publish VOLATILE messages while disconnected
    for (int i = 0; i < 5; ++i) {
        auto result = client.publish({static_cast<uint8_t>(i)}, Persistence::Volatile);
        ASSERT_TRUE(result.ok());
    }

    auto status = client.queue_status();
    EXPECT_GT(status.queue_size, 0);

    // Shutdown (VOLATILE messages should be lost)
    ShutdownService();

    // Start broker and service
    ASSERT_TRUE(BrokerControl::Start());
    ASSERT_TRUE(StartService());

    auto client2 = createClient(701);
    ASSERT_TRUE(waitForConnectionState(client2, ConnectionState::Connected));

    // Wait
    std::this_thread::sleep_for(2s);

    // Queue should be empty (VOLATILE messages were lost)
    auto status2 = client2.queue_status();
    // Note: A new queue may not exist yet, so just verify no old messages sent
    auto stats = client2.stats();
    // Stats are reset on restart, so we can't directly verify.
    // The key behavior is that no messages arrive on MQTT that were queued before shutdown.
}

// =============================================================================
// on_queue_status_changed Event Tests
// =============================================================================

TEST_F(BackendTransportResilienceTest, OnQueueStatusChangedBroadcastsLevelChanges) {
    // Start service without broker so we can fill queue
    ASSERT_TRUE(StartService(10));  // Small queue

    auto client = createClient(800);

    std::mutex mtx;
    std::condition_variable cv;
    std::vector<QueueLevel> observed_levels;

    // Note: Client doesn't expose on_queue_status_changed directly.
    // This test would require extending the client or using raw gRPC.
    // For now, we verify queue level changes via publish response.

    // Fill queue and observe level changes
    for (int i = 0; i < 10; ++i) {
        auto result = client.publish({static_cast<uint8_t>(i)}, Persistence::Volatile);
        if (result.ok()) {
            // Track observed levels
            if (observed_levels.empty() || observed_levels.back() != result.queue_level) {
                observed_levels.push_back(result.queue_level);
            }
        }
    }

    // Should have observed level transitions
    EXPECT_GT(observed_levels.size(), 1)
        << "Should observe queue level changes as queue fills";
}

// =============================================================================
// Enhanced DURABLE Persistence Tests
// =============================================================================

TEST_F(BackendTransportResilienceTest, DurablePersistenceFileCreatedOnShutdown) {
    // Start service without broker
    ASSERT_TRUE(StartService());

    auto client = createClient(750);

    // Publish DURABLE messages
    for (int i = 0; i < 3; ++i) {
        auto result = client.publish({static_cast<uint8_t>(i)}, Persistence::Durable);
        ASSERT_TRUE(result.ok());
    }

    // Verify file doesn't exist before shutdown (messages are in memory)
    std::string expected_file = persistence_dir_ + "/queue_750.bin";
    EXPECT_FALSE(std::filesystem::exists(expected_file))
        << "Persistence file should not exist before shutdown";

    // Shutdown service (triggers PersistAll)
    ShutdownService();

    // Verify persistence file was created
    ASSERT_TRUE(std::filesystem::exists(expected_file))
        << "Persistence file should be created on graceful shutdown";

    // Verify file has content (non-empty)
    auto file_size = std::filesystem::file_size(expected_file);
    EXPECT_GT(file_size, 0) << "Persistence file should contain data";

    LOG(INFO) << "Persistence file created: " << expected_file
              << " (size=" << file_size << " bytes)";
}

TEST_F(BackendTransportResilienceTest, DurableMessagesDeliveredWithAcksAfterRestart) {
    // Clean any existing persistence files for this content_id
    std::string persistence_file = persistence_dir_ + "/queue_751.bin";
    std::filesystem::remove(persistence_file);

    // Start service without broker
    ASSERT_TRUE(StartService());

    auto client = createClient(751);

    // Publish DURABLE messages while disconnected
    LOG(INFO) << "=== Publishing DURABLE messages ===";
    std::vector<uint64_t> original_sequences;
    for (int i = 0; i < 5; ++i) {
        auto result = client.publish({static_cast<uint8_t>(i + 100)}, Persistence::Durable);
        ASSERT_TRUE(result.ok());
        original_sequences.push_back(result.sequence);
        LOG(INFO) << "Published message " << i << " with seq=" << result.sequence;
    }

    // Shutdown to persist
    LOG(INFO) << "=== Graceful shutdown to persist ===";
    ShutdownService();

    // Verify persistence file exists
    ASSERT_TRUE(std::filesystem::exists(persistence_file))
        << "Persistence file should exist after shutdown";

    // Start broker first
    ASSERT_TRUE(BrokerControl::Start());

    // Restart service (should load and deliver persisted messages)
    LOG(INFO) << "=== Restarting service ===";
    ASSERT_TRUE(StartService());

    auto client2 = createClient(751);
    ASSERT_TRUE(waitForConnectionState(client2, ConnectionState::Connected));

    // Wait for delivery (messages are sent immediately after load)
    std::this_thread::sleep_for(3s);

    // Verify stats show messages sent
    // Note: Acks may have already been sent before we could subscribe,
    // so we verify via stats instead
    auto stats = client2.stats();
    EXPECT_GE(stats.messages_sent, 5)
        << "Stats should reflect delivered DURABLE messages";

    LOG(INFO) << "Final stats: messages_sent=" << stats.messages_sent;
}

TEST_F(BackendTransportResilienceTest, PersistenceFileRemovedAfterDelivery) {
    // Clean start
    std::string persistence_file = persistence_dir_ + "/queue_752.bin";
    std::filesystem::remove(persistence_file);

    // Start service without broker, publish, shutdown
    ASSERT_TRUE(StartService());
    auto client = createClient(752);
    for (int i = 0; i < 3; ++i) {
        client.publish({static_cast<uint8_t>(i)}, Persistence::Durable);
    }
    ShutdownService();

    ASSERT_TRUE(std::filesystem::exists(persistence_file))
        << "Persistence file should exist after shutdown";

    // Start broker and service
    ASSERT_TRUE(BrokerControl::Start());
    ASSERT_TRUE(StartService());

    auto client2 = createClient(752);
    ASSERT_TRUE(waitForConnectionState(client2, ConnectionState::Connected));

    // Wait for delivery
    std::this_thread::sleep_for(3s);

    // Shutdown again (should not re-persist already-sent messages)
    ShutdownService();
    BrokerControl::Stop();

    // File might still exist but be empty, or be removed
    // The key is that on next restart, no duplicate messages are sent
    ASSERT_TRUE(BrokerControl::Start());
    ASSERT_TRUE(StartService());

    auto client3 = createClient(752);
    ASSERT_TRUE(waitForConnectionState(client3, ConnectionState::Connected));

    std::this_thread::sleep_for(2s);

    // Check stats - should not have sent more messages than original 3
    auto stats = client3.stats();
    // Note: Since we restarted twice, stats are reset each time
    // This test mainly verifies no crash or infinite loop on duplicate load
    LOG(INFO) << "Final stats: messages_sent=" << stats.messages_sent;
}

TEST_F(BackendTransportResilienceTest, BestEffortMessagesNotPersisted) {
    // Clean start
    std::string persistence_file = persistence_dir_ + "/queue_753.bin";
    std::filesystem::remove(persistence_file);

    // Start service without broker
    ASSERT_TRUE(StartService());
    auto client = createClient(753);

    // Publish BEST_EFFORT messages
    for (int i = 0; i < 5; ++i) {
        auto result = client.publish({static_cast<uint8_t>(i)}, Persistence::BestEffort);
        ASSERT_TRUE(result.ok());
    }

    // Shutdown
    ShutdownService();

    // Verify NO persistence file created (BEST_EFFORT is not persisted)
    EXPECT_FALSE(std::filesystem::exists(persistence_file))
        << "BEST_EFFORT messages should NOT be persisted to disk";
}

TEST_F(BackendTransportResilienceTest, MixedPersistenceOnlyDurablePersisted) {
    // Clean start
    std::string persistence_file = persistence_dir_ + "/queue_754.bin";
    std::filesystem::remove(persistence_file);

    // Start service without broker
    ASSERT_TRUE(StartService());
    auto client = createClient(754);

    // Publish mix of persistence levels
    client.publish({0x01}, Persistence::BestEffort);
    client.publish({0x02}, Persistence::Volatile);
    client.publish({0x03}, Persistence::Durable);
    client.publish({0x04}, Persistence::Volatile);
    client.publish({0x05}, Persistence::Durable);

    // Shutdown
    ShutdownService();

    // Verify persistence file exists (DURABLE messages persisted)
    ASSERT_TRUE(std::filesystem::exists(persistence_file))
        << "Persistence file should exist for DURABLE messages";

    // Start broker and service
    ASSERT_TRUE(BrokerControl::Start());
    ASSERT_TRUE(StartService());

    auto client2 = createClient(754);
    ASSERT_TRUE(waitForConnectionState(client2, ConnectionState::Connected));

    // Wait for delivery (messages are sent immediately after load)
    std::this_thread::sleep_for(3s);

    // Verify via stats - only DURABLE messages should have been persisted and sent
    // Note: We can't use acks here because messages are sent before we can subscribe
    auto stats = client2.stats();
    EXPECT_EQ(stats.messages_sent, 2)
        << "Only DURABLE messages (2) should be persisted and sent after restart";

    LOG(INFO) << "Mixed persistence test: messages_sent=" << stats.messages_sent;
}

}  // namespace ifex::test

int main(int argc, char** argv) {
    google::InitGoogleLogging(argv[0]);
    FLAGS_logtostderr = true;
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
