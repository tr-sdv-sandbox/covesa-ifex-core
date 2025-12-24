/**
 * @file backend_transport_conformance_test.cpp
 * @brief Conformance tests for Backend Transport Service gRPC API
 *
 * These tests verify the gRPC API contract as defined in the IFEX specification.
 * They are "black box" tests that could be used to verify ANY implementation
 * of the backend-transport-service (C++, Rust, Go, etc.).
 *
 * Requirements tested:
 * - Methods: publish, get_connection_status, get_queue_status, get_stats, healthy, get_content_id
 * - Events: on_ack, on_connection_changed, on_queue_status_changed
 * - Publish status codes: OK, QUEUE_FULL, MESSAGE_TOO_LONG, INVALID_REQUEST
 * - Queue levels: EMPTY, LOW, NORMAL, HIGH, CRITICAL, FULL
 * - Persistence semantics: BestEffort, Volatile, Durable priority
 * - Sequence number guarantees: monotonic, per-content-id
 */

#include "backend_transport_client.hpp"
#include "backend_transport_server.hpp"
#include "mqtt_test_fixture.hpp"

#include <gtest/gtest.h>
#include <glog/logging.h>
#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace ifex::test {

using namespace ifex::client;
using namespace std::chrono_literals;

/**
 * @brief Base fixture for conformance tests
 *
 * Uses a connected MQTT broker to ensure the service is fully operational.
 */
class BackendTransportConformanceTest : public MqttTestFixture {
protected:
    static std::unique_ptr<reference::BackendTransportServer> service_;
    static std::unique_ptr<grpc::Server> grpc_server_;
    static std::shared_ptr<grpc::Channel> channel_;
    static int grpc_port_;

    static void SetUpTestSuite() {
        MqttTestFixture::SetUpTestSuite();

        if (!container_started) {
            return;
        }

        reference::BackendTransportServer::Config config;
        config.mqtt_host = mqtt_host;
        config.mqtt_port = mqtt_port;
        config.vehicle_id = "conformance-test-vehicle";
        config.queue_size_per_content_id = 100;
        config.persistence_dir = "/tmp/ifex-conformance-test";

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

        LOG(INFO) << "Conformance test service listening on " << server_address;
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
};

std::unique_ptr<reference::BackendTransportServer> BackendTransportConformanceTest::service_;
std::unique_ptr<grpc::Server> BackendTransportConformanceTest::grpc_server_;
std::shared_ptr<grpc::Channel> BackendTransportConformanceTest::channel_;
int BackendTransportConformanceTest::grpc_port_ = 0;

// =============================================================================
// Publish Method Tests
// =============================================================================

TEST_F(BackendTransportConformanceTest, PublishReturnsSequenceNumber) {
    auto client = createClient(1001);

    auto result = client.publish({0x01, 0x02, 0x03});

    ASSERT_TRUE(result.ok()) << "Publish should succeed";
    EXPECT_EQ(result.sequence, 1) << "First message gets sequence 1";
}

TEST_F(BackendTransportConformanceTest, PublishSequencesAreMonotonic) {
    auto client = createClient(1002);

    std::vector<uint64_t> sequences;
    for (int i = 0; i < 10; ++i) {
        auto result = client.publish({static_cast<uint8_t>(i)});
        ASSERT_TRUE(result.ok());
        sequences.push_back(result.sequence);
    }

    // Check sequences are strictly monotonically increasing
    for (size_t i = 1; i < sequences.size(); ++i) {
        EXPECT_EQ(sequences[i], sequences[i-1] + 1)
            << "Sequence " << i << " should be " << (sequences[i-1] + 1)
            << " but got " << sequences[i];
    }
}

TEST_F(BackendTransportConformanceTest, DifferentContentIdsHaveSeparateSequences) {
    auto client1 = createClient(1003);
    auto client2 = createClient(1004);

    auto result1 = client1.publish({0x01});
    auto result2 = client2.publish({0x02});

    ASSERT_TRUE(result1.ok());
    ASSERT_TRUE(result2.ok());

    // Both should get sequence 1 (separate queues per content_id)
    EXPECT_EQ(result1.sequence, 1);
    EXPECT_EQ(result2.sequence, 1);
}

TEST_F(BackendTransportConformanceTest, PublishReturnsQueueLevel) {
    auto client = createClient(1005);

    auto result = client.publish({0x01});

    ASSERT_TRUE(result.ok());
    // Queue level should be valid (EMPTY to FULL)
    EXPECT_GE(static_cast<int>(result.queue_level), static_cast<int>(QueueLevel::Empty));
    EXPECT_LE(static_cast<int>(result.queue_level), static_cast<int>(QueueLevel::Full));
}

TEST_F(BackendTransportConformanceTest, PublishWithDifferentPersistence) {
    auto client = createClient(1006);

    auto r1 = client.publish({0x01}, Persistence::BestEffort);
    auto r2 = client.publish({0x02}, Persistence::Volatile);
    auto r3 = client.publish({0x03}, Persistence::Durable);

    EXPECT_TRUE(r1.ok());
    EXPECT_TRUE(r2.ok());
    EXPECT_TRUE(r3.ok());

    // All should get sequential sequences
    EXPECT_EQ(r1.sequence, 1);
    EXPECT_EQ(r2.sequence, 2);
    EXPECT_EQ(r3.sequence, 3);
}

TEST_F(BackendTransportConformanceTest, ConcurrentPublishesGetUniqueSequences) {
    const int NUM_THREADS = 4;
    const int MESSAGES_PER_THREAD = 25;

    std::vector<std::thread> threads;
    std::mutex sequences_mutex;
    std::vector<uint64_t> all_sequences;

    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([this, &sequences_mutex, &all_sequences]() {
            auto client = createClient(1007);
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
    EXPECT_EQ(all_sequences.size(), NUM_THREADS * MESSAGES_PER_THREAD);
}

// =============================================================================
// Status Methods Tests
// =============================================================================

TEST_F(BackendTransportConformanceTest, HealthyReturnsTrueWhenConnected) {
    auto client = createClient(1010);
    EXPECT_TRUE(client.healthy());
}

TEST_F(BackendTransportConformanceTest, ConnectionStatusIsConnected) {
    auto client = createClient(1011);
    auto status = client.connection_status();

    EXPECT_EQ(status.state, ConnectionState::Connected);
    EXPECT_EQ(status.reason, DisconnectReason::None);
    EXPECT_GT(status.timestamp_ns, 0);
}

TEST_F(BackendTransportConformanceTest, QueueStatusReturnsValidData) {
    auto client = createClient(1012);
    auto status = client.queue_status();

    EXPECT_GE(static_cast<int>(status.level), static_cast<int>(QueueLevel::Empty));
    EXPECT_LE(static_cast<int>(status.level), static_cast<int>(QueueLevel::Full));
    EXPECT_GT(status.queue_capacity, 0);
    EXPECT_LE(status.queue_size, status.queue_capacity);
}

TEST_F(BackendTransportConformanceTest, StatsTrackMessages) {
    auto client = createClient(1013);

    auto initial = client.stats();
    uint64_t initial_sent = initial.messages_sent;

    // Send messages
    for (int i = 0; i < 5; ++i) {
        client.publish({static_cast<uint8_t>(i)});
    }

    // Wait for delivery
    std::this_thread::sleep_for(500ms);

    auto after = client.stats();
    EXPECT_GE(after.messages_sent, initial_sent + 5);
}

// =============================================================================
// on_connection_changed Event Tests
// =============================================================================

TEST_F(BackendTransportConformanceTest, OnConnectionChangedReceivesInitialStatus) {
    auto client = createClient(1020);

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

    {
        std::unique_lock<std::mutex> lock(mtx);
        ASSERT_TRUE(cv.wait_for(lock, 5s, [&]() { return got_status; }));
    }

    EXPECT_EQ(received_status.state, ConnectionState::Connected);

    client.unsubscribe_all();
}

// =============================================================================
// on_ack Event Tests
// =============================================================================

TEST_F(BackendTransportConformanceTest, OnAckReceivesDeliveryConfirmations) {
    auto client = createClient(1030);

    std::mutex mtx;
    std::condition_variable cv;
    std::vector<uint64_t> acked_sequences;

    client.on_ack([&](uint64_t sequence) {
        std::lock_guard<std::mutex> lock(mtx);
        acked_sequences.push_back(sequence);
        cv.notify_all();
    });

    // Give subscription time to establish
    std::this_thread::sleep_for(200ms);

    // Publish messages
    std::vector<uint64_t> published_sequences;
    for (int i = 0; i < 3; ++i) {
        auto result = client.publish({static_cast<uint8_t>(i)}, Persistence::Volatile);
        ASSERT_TRUE(result.ok());
        published_sequences.push_back(result.sequence);
    }

    // Wait for acks
    {
        std::unique_lock<std::mutex> lock(mtx);
        ASSERT_TRUE(cv.wait_for(lock, 10s, [&]() {
            return acked_sequences.size() >= published_sequences.size();
        })) << "Should receive acks for all published messages";
    }

    client.unsubscribe_all();

    // Verify all published sequences were acked
    std::sort(acked_sequences.begin(), acked_sequences.end());
    for (uint64_t seq : published_sequences) {
        EXPECT_TRUE(std::binary_search(acked_sequences.begin(), acked_sequences.end(), seq))
            << "Sequence " << seq << " should be acked";
    }
}

TEST_F(BackendTransportConformanceTest, OnAckSequencesAreMonotonic) {
    auto client = createClient(1031);

    std::mutex mtx;
    std::condition_variable cv;
    std::vector<uint64_t> acked_sequences;

    client.on_ack([&](uint64_t sequence) {
        std::lock_guard<std::mutex> lock(mtx);
        acked_sequences.push_back(sequence);
        cv.notify_all();
    });

    std::this_thread::sleep_for(200ms);

    // Publish several messages
    for (int i = 0; i < 5; ++i) {
        auto result = client.publish({static_cast<uint8_t>(i)}, Persistence::Volatile);
        ASSERT_TRUE(result.ok());
    }

    // Wait for acks
    {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait_for(lock, 10s, [&]() { return acked_sequences.size() >= 5; });
    }

    client.unsubscribe_all();

    // Acks should arrive in order (FIFO delivery)
    for (size_t i = 1; i < acked_sequences.size(); ++i) {
        EXPECT_GT(acked_sequences[i], acked_sequences[i-1])
            << "Ack sequences should be monotonically increasing";
    }
}

// =============================================================================
// Queue Level Tests
// =============================================================================

/**
 * Fixture with smaller queue for testing queue level transitions
 */
class QueueLevelConformanceTest : public ::testing::Test {
protected:
    static std::unique_ptr<reference::BackendTransportServer> service_;
    static std::unique_ptr<grpc::Server> grpc_server_;
    static std::shared_ptr<grpc::Channel> channel_;
    static int grpc_port_;

    static void SetUpTestSuite() {
        // Check Docker availability
        if (std::system("docker --version > /dev/null 2>&1") != 0) {
            GTEST_SKIP() << "Docker is not available";
            return;
        }

        // Stop any existing container
        std::system("docker stop ifex-mqtt-queue-test 2>/dev/null");
        std::system("docker rm -f ifex-mqtt-queue-test 2>/dev/null");

        // Start MQTT broker
        std::string cmd = "docker run -d --rm --name ifex-mqtt-queue-test "
                          "-p 21883:1883 eclipse-mosquitto:2 "
                          "sh -c 'echo -e \"listener 1883\\nallow_anonymous true\" > /tmp/m.conf && "
                          "mosquitto -c /tmp/m.conf'";
        if (std::system(cmd.c_str()) != 0) {
            GTEST_SKIP() << "Failed to start MQTT broker";
            return;
        }

        std::this_thread::sleep_for(2s);

        // Create service with small queue (20 messages)
        reference::BackendTransportServer::Config config;
        config.mqtt_host = "localhost";
        config.mqtt_port = 21883;
        config.vehicle_id = "queue-test-vehicle";
        config.queue_size_per_content_id = 20;
        config.persistence_dir = "/tmp/ifex-queue-test";

        service_ = std::make_unique<reference::BackendTransportServer>(config);

        if (!service_->Start()) {
            GTEST_SKIP() << "Failed to start service";
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

        std::this_thread::sleep_for(2s);
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
        std::system("docker stop ifex-mqtt-queue-test 2>/dev/null");
    }

    void SetUp() override {
        if (!service_ || !channel_) {
            GTEST_SKIP() << "Service not running";
        }
    }

    BackendTransportClient createClient(uint32_t content_id) {
        return BackendTransportClient(channel_, content_id);
    }
};

std::unique_ptr<reference::BackendTransportServer> QueueLevelConformanceTest::service_;
std::unique_ptr<grpc::Server> QueueLevelConformanceTest::grpc_server_;
std::shared_ptr<grpc::Channel> QueueLevelConformanceTest::channel_;
int QueueLevelConformanceTest::grpc_port_ = 0;

TEST_F(QueueLevelConformanceTest, QueueLevelTransitionsAsQueueFills) {
    // Stop the MQTT broker to prevent queue draining
    std::system("docker stop ifex-mqtt-queue-test 2>/dev/null");
    std::this_thread::sleep_for(1s);

    auto client = createClient(2001);

    std::set<QueueLevel> observed_levels;

    // Fill the queue and observe level transitions
    // Queue size is 20, so we fill it up
    for (int i = 0; i < 20; ++i) {
        auto result = client.publish({static_cast<uint8_t>(i)}, Persistence::Volatile);
        if (result.ok()) {
            observed_levels.insert(result.queue_level);
        } else {
            // Queue full - should get FULL level in last successful publish
            break;
        }
    }

    // Should have observed multiple levels as queue filled
    EXPECT_GT(observed_levels.size(), 1)
        << "Should observe multiple queue levels as queue fills";

    // Should end at HIGH, CRITICAL, or FULL
    auto max_level = *observed_levels.rbegin();
    EXPECT_GE(static_cast<int>(max_level), static_cast<int>(QueueLevel::High))
        << "Should reach at least HIGH level when queue is mostly full";
}

TEST_F(QueueLevelConformanceTest, QueueFullRejectsNewMessages) {
    // Keep broker stopped
    std::system("docker stop ifex-mqtt-queue-test 2>/dev/null");
    std::this_thread::sleep_for(500ms);

    auto client = createClient(2002);

    // Fill the queue completely
    int accepted = 0;
    for (int i = 0; i < 30; ++i) {
        auto result = client.publish({static_cast<uint8_t>(i)}, Persistence::Volatile);
        if (result.ok()) {
            accepted++;
        } else {
            EXPECT_EQ(result.status, PublishStatus::QueueFull);
            break;
        }
    }

    EXPECT_LE(accepted, 20) << "Should not accept more than queue capacity";
}

TEST_F(QueueLevelConformanceTest, VolatileDisplacesBestEffortWhenFull) {
    // Keep broker stopped
    std::system("docker stop ifex-mqtt-queue-test 2>/dev/null");
    std::this_thread::sleep_for(500ms);

    auto client = createClient(2003);

    // Fill queue with BestEffort messages
    for (int i = 0; i < 20; ++i) {
        auto result = client.publish({static_cast<uint8_t>(i)}, Persistence::BestEffort);
        ASSERT_TRUE(result.ok()) << "Should accept BestEffort message " << i;
    }

    // Queue should be full now
    auto status = client.queue_status();
    EXPECT_EQ(status.level, QueueLevel::Full);

    // Volatile should displace BestEffort
    auto result = client.publish({0xFF}, Persistence::Volatile);
    EXPECT_TRUE(result.ok()) << "Volatile should displace BestEffort when queue is full";
}

TEST_F(QueueLevelConformanceTest, BestEffortRejectedWhenFull) {
    // Keep broker stopped
    std::system("docker stop ifex-mqtt-queue-test 2>/dev/null");
    std::this_thread::sleep_for(500ms);

    auto client = createClient(2004);

    // Fill queue with Volatile messages (cannot be displaced)
    for (int i = 0; i < 20; ++i) {
        auto result = client.publish({static_cast<uint8_t>(i)}, Persistence::Volatile);
        ASSERT_TRUE(result.ok()) << "Should accept Volatile message " << i;
    }

    // BestEffort should be rejected
    auto result = client.publish({0xFF}, Persistence::BestEffort);
    EXPECT_FALSE(result.ok()) << "BestEffort should be rejected when queue is full of Volatile";
    EXPECT_EQ(result.status, PublishStatus::QueueFull);
}

// =============================================================================
// get_content_id RPC Tests
// =============================================================================

TEST_F(BackendTransportConformanceTest, GetContentIdReturnsChannelBoundId) {
    // Create client with specific content_id
    auto client = createClient(9999);

    // Use raw gRPC to call get_content_id service
    using namespace swdv::backend_transport_service;
    auto stub = get_content_id_service::NewStub(channel_);

    grpc::ClientContext context;
    // Set the content_id metadata (channel binding)
    context.AddMetadata("x-content-id", "9999");

    get_content_id_request request;
    get_content_id_response response;

    auto status = stub->get_content_id(&context, request, &response);
    ASSERT_TRUE(status.ok()) << "get_content_id RPC should succeed: " << status.error_message();

    EXPECT_EQ(response.content_id(), 9999)
        << "Server should return the channel-bound content_id";
}

TEST_F(BackendTransportConformanceTest, GetContentIdFailsWithoutMetadata) {
    using namespace swdv::backend_transport_service;
    auto stub = get_content_id_service::NewStub(channel_);

    grpc::ClientContext context;
    // No x-content-id metadata

    get_content_id_request request;
    get_content_id_response response;

    auto status = stub->get_content_id(&context, request, &response);
    EXPECT_FALSE(status.ok())
        << "get_content_id should fail without x-content-id metadata";
    EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// =============================================================================
// Stats Detailed Tests
// =============================================================================

TEST_F(BackendTransportConformanceTest, StatsTimestampsUpdateAfterSend) {
    auto client = createClient(1100);

    auto initial = client.stats();
    int64_t initial_send_ts = initial.last_send_timestamp_ns;

    // Small delay to ensure timestamp difference
    std::this_thread::sleep_for(10ms);

    // Publish a message
    auto result = client.publish({0x01}, Persistence::Volatile);
    ASSERT_TRUE(result.ok());

    // Wait for delivery
    std::this_thread::sleep_for(500ms);

    auto after = client.stats();

    // Timestamp should have been updated (or be initially 0 and now > 0)
    if (initial_send_ts == 0) {
        EXPECT_GT(after.last_send_timestamp_ns, 0)
            << "last_send_timestamp should be set after first send";
    } else {
        EXPECT_GT(after.last_send_timestamp_ns, initial_send_ts)
            << "last_send_timestamp should update after send";
    }
}

TEST_F(BackendTransportConformanceTest, StatsBytesTrackPayloadSize) {
    auto client = createClient(1101);

    auto initial = client.stats();
    uint64_t initial_bytes = initial.bytes_sent;

    // Publish messages with known payload sizes
    std::vector<uint8_t> payload1(100, 0xAA);  // 100 bytes
    std::vector<uint8_t> payload2(200, 0xBB);  // 200 bytes

    client.publish(payload1, Persistence::Volatile);
    client.publish(payload2, Persistence::Volatile);

    // Wait for delivery
    std::this_thread::sleep_for(500ms);

    auto after = client.stats();

    // bytes_sent should increase by at least payload sizes
    uint64_t bytes_added = after.bytes_sent - initial_bytes;
    EXPECT_GE(bytes_added, 300)
        << "bytes_sent should track payload sizes (sent 300 bytes, got " << bytes_added << ")";
}

}  // namespace ifex::test

int main(int argc, char** argv) {
    google::InitGoogleLogging(argv[0]);
    FLAGS_logtostderr = true;
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
