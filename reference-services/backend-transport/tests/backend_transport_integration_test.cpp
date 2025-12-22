/**
 * @file backend_transport_integration_test.cpp
 * @brief Integration tests for Backend Transport Service using client library
 *
 * Tests require Docker for MQTT broker. Use MQTT_HOST/MQTT_PORT env vars for external broker.
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
#include <mutex>
#include <thread>
#include <vector>

namespace ifex::test {

using namespace ifex::client;

class BackendTransportIntegrationTest : public MqttTestFixture {
protected:
    static std::unique_ptr<reference::BackendTransportServer> service_;
    static std::unique_ptr<grpc::Server> grpc_server_;
    static std::shared_ptr<grpc::Channel> channel_;
    static int grpc_port_;

    static void SetUpTestSuite() {
        // First set up MQTT
        MqttTestFixture::SetUpTestSuite();

        if (!container_started) {
            return;  // MQTT not available, tests will be skipped
        }

        // Create and start the backend transport service
        reference::BackendTransportServer::Config config;
        config.mqtt_host = mqtt_host;
        config.mqtt_port = mqtt_port;
        config.vehicle_id = "test-vehicle";
        config.queue_size_per_content_id = 100;
        config.persistence_dir = "/tmp/ifex-test-persistence";

        service_ = std::make_unique<reference::BackendTransportServer>(config);

        if (!service_->Start()) {
            LOG(ERROR) << "Failed to start backend transport service";
            GTEST_SKIP() << "Failed to start backend transport service";
            return;
        }

        // Start gRPC server
        grpc::ServerBuilder builder;
        builder.AddListeningPort("0.0.0.0:0", grpc::InsecureServerCredentials(), &grpc_port_);

        // Register all services
        using namespace swdv::backend_transport_service;
        builder.RegisterService(static_cast<publish_service::Service*>(service_.get()));
        builder.RegisterService(static_cast<get_connection_status_service::Service*>(service_.get()));
        builder.RegisterService(static_cast<get_queue_status_service::Service*>(service_.get()));
        builder.RegisterService(static_cast<get_stats_service::Service*>(service_.get()));
        builder.RegisterService(static_cast<healthy_service::Service*>(service_.get()));
        builder.RegisterService(static_cast<on_content_service::Service*>(service_.get()));
        builder.RegisterService(static_cast<on_ack_service::Service*>(service_.get()));
        builder.RegisterService(static_cast<on_connection_changed_service::Service*>(service_.get()));
        builder.RegisterService(static_cast<on_queue_status_changed_service::Service*>(service_.get()));

        grpc_server_ = builder.BuildAndStart();

        std::string server_address = "localhost:" + std::to_string(grpc_port_);
        channel_ = grpc::CreateChannel(server_address, grpc::InsecureChannelCredentials());

        LOG(INFO) << "Backend Transport Service listening on " << server_address;

        // Give services time to connect
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

    // Helper to create a client for a content_id
    BackendTransportClient createClient(uint32_t content_id) {
        return BackendTransportClient(channel_, content_id);
    }
};

// Static member definitions
std::unique_ptr<reference::BackendTransportServer> BackendTransportIntegrationTest::service_;
std::unique_ptr<grpc::Server> BackendTransportIntegrationTest::grpc_server_;
std::shared_ptr<grpc::Channel> BackendTransportIntegrationTest::channel_;
int BackendTransportIntegrationTest::grpc_port_ = 0;

// =============================================================================
// Publish Tests
// =============================================================================

TEST_F(BackendTransportIntegrationTest, PublishReturnsSequenceNumber) {
    auto client = createClient(42);

    auto result = client.publish({0x01, 0x02, 0x03});

    ASSERT_TRUE(result.ok()) << "Publish should succeed";
    EXPECT_EQ(result.sequence, 1) << "First message gets sequence 1";
}

TEST_F(BackendTransportIntegrationTest, PublishSequencesAreMonotonic) {
    auto client = createClient(100);

    std::vector<uint64_t> sequences;
    for (int i = 0; i < 5; ++i) {
        auto result = client.publish({static_cast<uint8_t>(i)});
        ASSERT_TRUE(result.ok());
        sequences.push_back(result.sequence);
    }

    // Check sequences are monotonically increasing
    for (size_t i = 1; i < sequences.size(); ++i) {
        EXPECT_EQ(sequences[i], sequences[i-1] + 1)
            << "Sequence " << i << " should be " << (sequences[i-1] + 1)
            << " but got " << sequences[i];
    }
}

TEST_F(BackendTransportIntegrationTest, DifferentContentIdsHaveSeparateSequences) {
    auto client1 = createClient(200);
    auto client2 = createClient(201);

    auto result1 = client1.publish({0x01});
    auto result2 = client2.publish({0x02});

    ASSERT_TRUE(result1.ok());
    ASSERT_TRUE(result2.ok());

    // Both should get sequence 1 (separate queues)
    EXPECT_EQ(result1.sequence, 1);
    EXPECT_EQ(result2.sequence, 1);
}

TEST_F(BackendTransportIntegrationTest, PublishWithDifferentPersistence) {
    auto client = createClient(300);

    auto r1 = client.publish({0x01}, Persistence::None);
    auto r2 = client.publish({0x02}, Persistence::UntilDelivered);
    auto r3 = client.publish({0x03}, Persistence::Persistent);

    EXPECT_TRUE(r1.ok());
    EXPECT_TRUE(r2.ok());
    EXPECT_TRUE(r3.ok());

    // All should get sequential sequences
    EXPECT_EQ(r1.sequence, 1);
    EXPECT_EQ(r2.sequence, 2);
    EXPECT_EQ(r3.sequence, 3);
}

// =============================================================================
// Connection Status Tests
// =============================================================================

TEST_F(BackendTransportIntegrationTest, HealthyReturnsTrue) {
    auto client = createClient(1);
    EXPECT_TRUE(client.healthy());
}

TEST_F(BackendTransportIntegrationTest, ConnectionStatusIsConnected) {
    auto client = createClient(1);
    auto status = client.connection_status();
    EXPECT_EQ(status.state, ConnectionState::Connected);
}

// =============================================================================
// Queue Status Tests
// =============================================================================

TEST_F(BackendTransportIntegrationTest, QueueStatusShowsCapacity) {
    auto client = createClient(1);
    auto status = client.queue_status();

    EXPECT_FALSE(status.is_full);
    EXPECT_EQ(status.queue_capacity, 100);  // From config
}

// =============================================================================
// Stats Tests
// =============================================================================

TEST_F(BackendTransportIntegrationTest, StatsTrackMessages) {
    auto client = createClient(400);

    auto initial = client.stats();
    uint64_t initial_sent = initial.messages_sent;

    // Send some messages
    client.publish({0x01});
    client.publish({0x02});

    auto after = client.stats();
    EXPECT_GE(after.messages_sent, initial_sent + 2);
}

// =============================================================================
// Streaming Subscription Tests
// =============================================================================

TEST_F(BackendTransportIntegrationTest, OnConnectionChangedReceivesInitialStatus) {
    auto client = createClient(1);

    std::mutex mtx;
    std::condition_variable cv;
    ConnectionStatus received_status;
    bool got_status = false;

    client.on_connection_changed([&](const ConnectionStatus& status) {
        std::lock_guard<std::mutex> lock(mtx);
        received_status = status;
        got_status = true;
        cv.notify_all();
    });

    // Wait for initial status
    {
        std::unique_lock<std::mutex> lock(mtx);
        ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(5), [&]() { return got_status; }));
    }

    EXPECT_EQ(received_status.state, ConnectionState::Connected);

    client.unsubscribe_all();
}

// =============================================================================
// Concurrent Publish Tests
// =============================================================================

TEST_F(BackendTransportIntegrationTest, ConcurrentPublishesGetUniqueSequences) {
    const int NUM_THREADS = 4;
    const int MESSAGES_PER_THREAD = 10;

    std::vector<std::thread> threads;
    std::mutex sequences_mutex;
    std::vector<uint64_t> all_sequences;

    // All threads publish to same content_id
    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([this, &sequences_mutex, &all_sequences]() {
            auto client = createClient(500);

            for (int i = 0; i < MESSAGES_PER_THREAD; ++i) {
                auto result = client.publish({0x01, 0x02, 0x03});
                if (result.ok()) {
                    std::lock_guard<std::mutex> lock(sequences_mutex);
                    all_sequences.push_back(result.sequence);
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // All sequences should be unique
    std::sort(all_sequences.begin(), all_sequences.end());
    auto last = std::unique(all_sequences.begin(), all_sequences.end());
    EXPECT_EQ(last, all_sequences.end()) << "All sequences should be unique";

    // Should have gotten all expected messages
    EXPECT_EQ(all_sequences.size(), NUM_THREADS * MESSAGES_PER_THREAD);
}

// =============================================================================
// Client API Tests
// =============================================================================

TEST_F(BackendTransportIntegrationTest, ClientContentIdAccessor) {
    auto client = createClient(42);
    EXPECT_EQ(client.content_id(), 42);
}

TEST_F(BackendTransportIntegrationTest, ClientIsMovable) {
    auto client1 = createClient(42);
    auto result1 = client1.publish({0x01});
    ASSERT_TRUE(result1.ok());

    // Move client
    auto client2 = std::move(client1);
    EXPECT_EQ(client2.content_id(), 42);

    auto result2 = client2.publish({0x02});
    ASSERT_TRUE(result2.ok());
    EXPECT_EQ(result2.sequence, result1.sequence + 1);
}

TEST_F(BackendTransportIntegrationTest, MultipleClientsShareChannel) {
    // Create multiple clients for different content_ids, sharing channel
    auto client1 = createClient(600);
    auto client2 = createClient(601);
    auto client3 = createClient(602);

    // All should work independently
    EXPECT_TRUE(client1.publish({0x01}).ok());
    EXPECT_TRUE(client2.publish({0x02}).ok());
    EXPECT_TRUE(client3.publish({0x03}).ok());

    // Each should have sequence 1 (separate content_ids)
    EXPECT_EQ(client1.publish({0x01}).sequence, 2);
    EXPECT_EQ(client2.publish({0x02}).sequence, 2);
    EXPECT_EQ(client3.publish({0x03}).sequence, 2);
}

}  // namespace ifex::test

int main(int argc, char** argv) {
    google::InitGoogleLogging(argv[0]);
    FLAGS_logtostderr = true;
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
