/**
 * @file backend_transport_client_test.cpp
 * @brief Tests for the C++ client library
 *
 * These tests verify our specific C++ client implementation.
 * Other language bindings (Python, Rust) would have their own client tests.
 */

#include "backend_transport_client.hpp"
#include "backend_transport_server.hpp"
#include "mqtt_test_fixture.hpp"

#include <gtest/gtest.h>
#include <glog/logging.h>
#include <grpcpp/grpcpp.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace ifex::test {

using namespace ifex::client;

class BackendTransportClientTest : public MqttTestFixture {
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
        config.vehicle_id = "test-vehicle";
        config.queue_size_per_content_id = 100;
        config.persistence_dir = "/tmp/ifex-client-test";

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

        LOG(INFO) << "Client test service listening on " << server_address;
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

std::unique_ptr<reference::BackendTransportServer> BackendTransportClientTest::service_;
std::unique_ptr<grpc::Server> BackendTransportClientTest::grpc_server_;
std::shared_ptr<grpc::Channel> BackendTransportClientTest::channel_;
int BackendTransportClientTest::grpc_port_ = 0;

// =============================================================================
// Client Construction and Properties
// =============================================================================

TEST_F(BackendTransportClientTest, ContentIdAccessor) {
    auto client = createClient(42);
    EXPECT_EQ(client.content_id(), 42);
}

TEST_F(BackendTransportClientTest, ContentIdPreservedAcrossCalls) {
    auto client = createClient(123);

    // Multiple operations should all use same content_id
    client.publish({0x01});
    EXPECT_EQ(client.content_id(), 123);

    client.healthy();
    EXPECT_EQ(client.content_id(), 123);

    client.connection_status();
    EXPECT_EQ(client.content_id(), 123);
}

// =============================================================================
// Move Semantics
// =============================================================================

TEST_F(BackendTransportClientTest, MoveConstruction) {
    auto client1 = createClient(42);
    auto result1 = client1.publish({0x01});
    ASSERT_TRUE(result1.ok());

    // Move construct
    auto client2 = std::move(client1);
    EXPECT_EQ(client2.content_id(), 42);

    // Should continue with same sequence
    auto result2 = client2.publish({0x02});
    ASSERT_TRUE(result2.ok());
    EXPECT_EQ(result2.sequence, result1.sequence + 1);
}

TEST_F(BackendTransportClientTest, MoveAssignment) {
    auto client1 = createClient(42);
    auto client2 = createClient(99);

    auto result1 = client1.publish({0x01});
    ASSERT_TRUE(result1.ok());

    // Move assign
    client2 = std::move(client1);
    EXPECT_EQ(client2.content_id(), 42);

    auto result2 = client2.publish({0x02});
    ASSERT_TRUE(result2.ok());
    EXPECT_EQ(result2.sequence, result1.sequence + 1);
}

// =============================================================================
// Multiple Clients
// =============================================================================

TEST_F(BackendTransportClientTest, MultipleClientsShareChannel) {
    // Create multiple clients for different content_ids, sharing channel
    auto client1 = createClient(600);
    auto client2 = createClient(601);
    auto client3 = createClient(602);

    // All should work independently
    EXPECT_TRUE(client1.publish({0x01}).ok());
    EXPECT_TRUE(client2.publish({0x02}).ok());
    EXPECT_TRUE(client3.publish({0x03}).ok());

    // Each has separate sequence space (all get sequence 2 for second message)
    EXPECT_EQ(client1.publish({0x01}).sequence, 2);
    EXPECT_EQ(client2.publish({0x02}).sequence, 2);
    EXPECT_EQ(client3.publish({0x03}).sequence, 2);
}

TEST_F(BackendTransportClientTest, SameContentIdDifferentClients) {
    // Two clients for same content_id share sequence space
    auto client1 = createClient(700);
    auto client2 = createClient(700);

    auto r1 = client1.publish({0x01});
    auto r2 = client2.publish({0x02});
    auto r3 = client1.publish({0x03});

    ASSERT_TRUE(r1.ok());
    ASSERT_TRUE(r2.ok());
    ASSERT_TRUE(r3.ok());

    // Sequences should be unique across both clients
    EXPECT_NE(r1.sequence, r2.sequence);
    EXPECT_NE(r2.sequence, r3.sequence);
    EXPECT_NE(r1.sequence, r3.sequence);
}

// =============================================================================
// Subscription Management
// =============================================================================

TEST_F(BackendTransportClientTest, UnsubscribeAllStopsCallbacks) {
    auto client = createClient(800);

    std::atomic<int> callback_count{0};
    client.on_connection_changed([&](const ConnectionStatus&) {
        callback_count++;
    });

    // Wait for initial callback
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    int count_before = callback_count.load();
    EXPECT_GT(count_before, 0);

    // Unsubscribe
    client.unsubscribe_all();

    // Wait and verify no more callbacks
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    int count_after = callback_count.load();
    EXPECT_EQ(count_before, count_after);
}

TEST_F(BackendTransportClientTest, ResubscribeAfterUnsubscribe) {
    auto client = createClient(801);

    std::atomic<int> callback_count{0};

    // First subscription
    client.on_connection_changed([&](const ConnectionStatus&) {
        callback_count++;
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_GT(callback_count.load(), 0);

    client.unsubscribe_all();
    callback_count = 0;

    // Re-subscribe
    client.on_connection_changed([&](const ConnectionStatus&) {
        callback_count++;
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_GT(callback_count.load(), 0);

    client.unsubscribe_all();
}

// =============================================================================
// PublishResult API
// =============================================================================

TEST_F(BackendTransportClientTest, PublishResultBoolConversion) {
    auto client = createClient(900);

    auto result = client.publish({0x01});

    // Bool conversion should work
    EXPECT_TRUE(static_cast<bool>(result));
    EXPECT_TRUE(result.ok());

    // if (result) should work
    if (result) {
        EXPECT_GT(result.sequence, 0);
    } else {
        FAIL() << "Result should be truthy";
    }
}

TEST_F(BackendTransportClientTest, PublishResultContainsQueueLevel) {
    auto client = createClient(901);

    auto result = client.publish({0x01});

    ASSERT_TRUE(result.ok());
    // Queue level should be set (likely Empty or Low for first message)
    EXPECT_LE(static_cast<int>(result.queue_level), static_cast<int>(QueueLevel::Full));
}

}  // namespace ifex::test

int main(int argc, char** argv) {
    google::InitGoogleLogging(argv[0]);
    FLAGS_logtostderr = true;
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
