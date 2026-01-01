/**
 * @file discovery_sync_bridge_integration_test.cpp
 * @brief Integration tests for DiscoverySyncBridge with Docker MQTT broker
 *
 * Tests end-to-end sync flow:
 * Discovery service → DiscoverySyncBridge → Backend Transport → MQTT
 */

#include "../../backend-transport/tests/mqtt_test_fixture.hpp"
#include "backend_transport_client.hpp"
#include "backend_transport_server.hpp"
#include "discovery_sync_bridge.hpp"
#include "ifex_content_ids.hpp"
#include "discovery-sync-envelope.pb.h"
#include "service-discovery-service.grpc.pb.h"

#include <gtest/gtest.h>
#include <glog/logging.h>
#include <grpcpp/grpcpp.h>
#include <mosquitto.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <map>
#include <mutex>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace ifex::test {

using namespace ifex::client;
using namespace ifex::reference;
using namespace std::chrono_literals;
namespace sync_pb = swdv::discovery_sync_envelope;
namespace discovery_pb = swdv::service_discovery;

/**
 * @brief Test fixture for DiscoverySyncBridge integration tests
 */
class DiscoverySyncBridgeIntegrationTest : public MqttTestFixture {
protected:
    static constexpr int TEST_DISCOVERY_PORT = 50291;
    static constexpr int BACKEND_TRANSPORT_GRPC_PORT = 50290;

    static constexpr const char* TEST_DISCOVERY_ADDRESS = "localhost:50291";

    static pid_t discovery_pid_;
    static std::unique_ptr<BackendTransportServer> backend_transport_;
    static std::unique_ptr<grpc::Server> backend_transport_grpc_;
    static std::string vehicle_id_;

    static void SetUpTestSuite() {
        MqttTestFixture::SetUpTestSuite();

        if (!container_started) {
            return;
        }

        vehicle_id_ = "discovery-sync-test-vehicle";

        // Start Discovery Service
        discovery_pid_ = start_service("discovery", TEST_DISCOVERY_PORT);
        if (!wait_for_service(TEST_DISCOVERY_ADDRESS)) {
            TearDownTestSuite();
            GTEST_SKIP() << "Discovery service failed to start";
            return;
        }

        // Start Backend Transport Server (in-process)
        BackendTransportServer::Config bt_config;
        bt_config.mqtt_host = mqtt_host;
        bt_config.mqtt_port = mqtt_port;
        bt_config.vehicle_id = vehicle_id_;
        bt_config.queue_size_per_content_id = 100;
        bt_config.persistence_dir = "/tmp/ifex-discovery-sync-test";

        backend_transport_ = std::make_unique<BackendTransportServer>(bt_config);

        if (!backend_transport_->Start()) {
            TearDownTestSuite();
            GTEST_SKIP() << "Failed to start Backend Transport";
            return;
        }

        // Start Backend Transport gRPC server
        grpc::ServerBuilder builder;
        int grpc_port = BACKEND_TRANSPORT_GRPC_PORT;
        builder.AddListeningPort("0.0.0.0:" + std::to_string(grpc_port),
                                  grpc::InsecureServerCredentials());

        using namespace swdv::backend_transport_service;
        builder.RegisterService(static_cast<publish_service::Service*>(backend_transport_.get()));
        builder.RegisterService(static_cast<get_connection_status_service::Service*>(backend_transport_.get()));
        builder.RegisterService(static_cast<get_queue_status_service::Service*>(backend_transport_.get()));
        builder.RegisterService(static_cast<get_stats_service::Service*>(backend_transport_.get()));
        builder.RegisterService(static_cast<healthy_service::Service*>(backend_transport_.get()));
        builder.RegisterService(static_cast<get_content_id_service::Service*>(backend_transport_.get()));
        builder.RegisterService(static_cast<on_content_service::Service*>(backend_transport_.get()));
        builder.RegisterService(static_cast<on_ack_service::Service*>(backend_transport_.get()));
        builder.RegisterService(static_cast<on_connection_changed_service::Service*>(backend_transport_.get()));
        builder.RegisterService(static_cast<on_queue_status_changed_service::Service*>(backend_transport_.get()));

        backend_transport_grpc_ = builder.BuildAndStart();

        LOG(INFO) << "Backend Transport listening on port " << grpc_port;
        std::this_thread::sleep_for(1s);
    }

    static void TearDownTestSuite() {
        LOG(INFO) << "Tearing down DiscoverySyncBridge integration tests...";

        if (backend_transport_grpc_) {
            backend_transport_grpc_->Shutdown();
            backend_transport_grpc_.reset();
        }

        if (backend_transport_) {
            backend_transport_->Stop();
            backend_transport_.reset();
        }

        stop_service(discovery_pid_, "discovery");

        MqttTestFixture::TearDownTestSuite();
    }

    void SetUp() override {
        MqttTestFixture::SetUp();
        if (!backend_transport_) {
            GTEST_SKIP() << "Backend Transport not running";
        }
    }

    /// Subscribe to MQTT and collect sync messages
    std::vector<sync_pb::sync_message_t> collectSyncMessages(
        int expected_count, std::chrono::seconds timeout) {

        std::string topic = "v2c/" + vehicle_id_ + "/" +
                            std::to_string(content_id::DISCOVERY_SYNC);

        struct Context {
            std::vector<sync_pb::sync_message_t> messages;
            std::mutex mtx;
            std::condition_variable cv;
            int expected_count;
        };

        Context ctx;
        ctx.expected_count = expected_count;

        struct mosquitto* mosq = mosquitto_new("test-sync-subscriber", true, &ctx);
        EXPECT_TRUE(mosq != nullptr);

        mosquitto_message_callback_set(mosq, [](struct mosquitto*, void* userdata,
                                                 const struct mosquitto_message* msg) {
            auto* ctx = static_cast<Context*>(userdata);
            sync_pb::sync_message_t message;
            if (message.ParseFromArray(msg->payload, msg->payloadlen)) {
                std::lock_guard<std::mutex> lock(ctx->mtx);
                ctx->messages.push_back(std::move(message));
                ctx->cv.notify_all();
            }
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

        std::lock_guard<std::mutex> lock(ctx.mtx);
        return ctx.messages;
    }

    /// Register a test service with Discovery
    bool registerTestService(const std::string& name, int port) {
        auto channel = grpc::CreateChannel(TEST_DISCOVERY_ADDRESS,
                                           grpc::InsecureChannelCredentials());
        auto stub = discovery_pb::register_service_service::NewStub(channel);

        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + 5s);

        discovery_pb::register_service_request request;
        request.mutable_service_info()->set_name(name);
        request.mutable_service_info()->set_version("1.0.0");
        request.mutable_service_info()->set_description("Test service");
        request.mutable_service_info()->mutable_endpoint()->set_address(
            "localhost:" + std::to_string(port));
        request.mutable_service_info()->mutable_endpoint()->set_transport(
            discovery_pb::GRPC);

        discovery_pb::register_service_response response;
        auto status = stub->register_service(&context, request, &response);

        if (status.ok()) {
            LOG(INFO) << "Registered test service: " << name
                      << " (id=" << response.registration_id() << ")";
            return true;
        }
        LOG(ERROR) << "Failed to register: " << status.error_message();
        return false;
    }

    /// Unregister a service from Discovery
    bool unregisterService(const std::string& registration_id) {
        auto channel = grpc::CreateChannel(TEST_DISCOVERY_ADDRESS,
                                           grpc::InsecureChannelCredentials());
        auto stub = discovery_pb::unregister_service_service::NewStub(channel);

        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + 5s);

        discovery_pb::unregister_service_request request;
        request.set_registration_id(registration_id);

        discovery_pb::unregister_service_response response;
        auto status = stub->unregister_service(&context, request, &response);

        return status.ok();
    }

private:
    static pid_t start_service(const std::string& name, int port) {
        std::string build_dir = get_build_dir();
        std::string executable = build_dir + "/reference-services/discovery/ifex-discovery-service";

        if (!fs::exists(executable)) {
            LOG(ERROR) << "Discovery executable not found: " << executable;
            return 0;
        }

        pid_t pid = fork();

        if (pid == 0) {
            std::string port_str = std::to_string(port);
            std::string listen_addr = "0.0.0.0:" + port_str;

            setenv("GLOG_logtostderr", "1", 1);

            std::string log_file = "/tmp/ifex_sync_test_discovery_" + port_str + ".log";
            freopen(log_file.c_str(), "w", stdout);
            freopen(log_file.c_str(), "w", stderr);

            std::string listen_param = "--listen=" + listen_addr;
            execl(executable.c_str(), executable.c_str(), listen_param.c_str(), nullptr);

            LOG(ERROR) << "Failed to exec " << executable << ": " << strerror(errno);
            _exit(1);
        } else if (pid < 0) {
            LOG(ERROR) << "Failed to fork for discovery service";
            return 0;
        }

        LOG(INFO) << "Started discovery service with PID " << pid;
        return pid;
    }

    static void stop_service(pid_t& pid, const std::string& name) {
        if (pid > 0) {
            LOG(INFO) << "Stopping " << name << " service (PID: " << pid << ")";

            kill(pid, SIGTERM);

            int status;
            int wait_count = 0;
            while (waitpid(pid, &status, WNOHANG) == 0 && wait_count < 50) {
                std::this_thread::sleep_for(100ms);
                wait_count++;
            }

            if (waitpid(pid, &status, WNOHANG) == 0) {
                LOG(WARNING) << "Force killing " << name << " service";
                kill(pid, SIGKILL);
                waitpid(pid, &status, 0);
            }

            pid = 0;
        }
    }

    static bool wait_for_service(const std::string& address, int timeout_seconds = 10) {
        auto channel = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
        auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(timeout_seconds);

        while (std::chrono::system_clock::now() < deadline) {
            auto state = channel->GetState(true);
            if (state == GRPC_CHANNEL_READY) {
                LOG(INFO) << "Service " << address << " is ready";
                return true;
            }
            channel->WaitForStateChange(state,
                std::chrono::system_clock::now() + std::chrono::milliseconds(200));
        }

        LOG(ERROR) << "Service at " << address << " failed to become ready";
        return false;
    }

    static std::string get_build_dir() {
        fs::path current = fs::current_path();

        while (!current.empty() && current != current.root_path()) {
            if (fs::exists(current / "CMakeCache.txt")) {
                fs::path ifex_subdir = current / "covesa-ifex-core" / "reference-services";
                if (fs::exists(ifex_subdir)) {
                    return (current / "covesa-ifex-core").string();
                }
                return current.string();
            }
            current = current.parent_path();
        }

        return ".";
    }
};

// Static member definitions
pid_t DiscoverySyncBridgeIntegrationTest::discovery_pid_ = 0;
std::unique_ptr<BackendTransportServer> DiscoverySyncBridgeIntegrationTest::backend_transport_;
std::unique_ptr<grpc::Server> DiscoverySyncBridgeIntegrationTest::backend_transport_grpc_;
std::string DiscoverySyncBridgeIntegrationTest::vehicle_id_;

// =============================================================================
// Integration Tests
// =============================================================================

TEST_F(DiscoverySyncBridgeIntegrationTest, FullSyncOnStartup) {
    // Register a test service first
    ASSERT_TRUE(registerTestService("test-sync-service", 59001));

    // Start sync bridge with short init delay
    DiscoverySyncBridgeConfig config;
    config.discovery_endpoint = TEST_DISCOVERY_ADDRESS;
    config.backend_transport_endpoint = "localhost:" +
        std::to_string(BACKEND_TRANSPORT_GRPC_PORT);
    config.sync_content_id = content_id::DISCOVERY_SYNC;
    config.vehicle_id = vehicle_id_;
    config.initialization_delay_ms = 500;
    config.poll_interval_ms = 500;
    config.batch_window_ms = 0;  // Immediate send
    config.heartbeat_interval_ms = 0;  // Disable heartbeat

    // Start collecting messages
    auto collect_future = std::async(std::launch::async, [this]() {
        return collectSyncMessages(1, 15s);
    });

    std::this_thread::sleep_for(500ms);

    DiscoverySyncBridge bridge(config);
    ASSERT_TRUE(bridge.Start());

    // Wait for full sync message
    auto messages = collect_future.get();

    bridge.Stop();

    ASSERT_GE(messages.size(), 1) << "Should receive at least one sync message";

    // First message should contain FULL_SYNC event
    const auto& first_msg = messages[0];
    EXPECT_EQ(first_msg.vehicle_id(), vehicle_id_);
    EXPECT_FALSE(first_msg.bridge_instance_id().empty());
    EXPECT_GT(first_msg.events_size(), 0);

    bool found_full_sync = false;
    for (const auto& event : first_msg.events()) {
        if (event.event_type() == sync_pb::FULL_SYNC) {
            found_full_sync = true;
            break;
        }
    }
    EXPECT_TRUE(found_full_sync) << "Should have FULL_SYNC event";
}

TEST_F(DiscoverySyncBridgeIntegrationTest, DeltaSyncOnServiceRegistration) {
    DiscoverySyncBridgeConfig config;
    config.discovery_endpoint = TEST_DISCOVERY_ADDRESS;
    config.backend_transport_endpoint = "localhost:" +
        std::to_string(BACKEND_TRANSPORT_GRPC_PORT);
    config.sync_content_id = content_id::DISCOVERY_SYNC;
    config.vehicle_id = vehicle_id_;
    config.initialization_delay_ms = 500;
    config.poll_interval_ms = 500;
    config.batch_window_ms = 0;
    config.heartbeat_interval_ms = 0;

    DiscoverySyncBridge bridge(config);
    ASSERT_TRUE(bridge.Start());

    // Wait for initialization
    std::this_thread::sleep_for(1s);
    ASSERT_TRUE(bridge.IsInitialized());

    // Start collecting delta messages
    auto collect_future = std::async(std::launch::async, [this]() {
        return collectSyncMessages(1, 10s);
    });

    std::this_thread::sleep_for(500ms);

    // Register a new service - should trigger delta sync
    ASSERT_TRUE(registerTestService("delta-test-service", 59002));

    // Wait for delta message
    auto messages = collect_future.get();

    bridge.Stop();

    ASSERT_GE(messages.size(), 1) << "Should receive delta sync message";

    // Look for SERVICE_REGISTERED event
    bool found_registered = false;
    for (const auto& msg : messages) {
        for (const auto& event : msg.events()) {
            if (event.event_type() == sync_pb::SERVICE_REGISTERED &&
                event.service_info().name() == "delta-test-service") {
                found_registered = true;
                break;
            }
        }
    }
    EXPECT_TRUE(found_registered) << "Should have SERVICE_REGISTERED event for delta-test-service";
}

TEST_F(DiscoverySyncBridgeIntegrationTest, StatsAfterSync) {
    DiscoverySyncBridgeConfig config;
    config.discovery_endpoint = TEST_DISCOVERY_ADDRESS;
    config.backend_transport_endpoint = "localhost:" +
        std::to_string(BACKEND_TRANSPORT_GRPC_PORT);
    config.sync_content_id = content_id::DISCOVERY_SYNC;
    config.vehicle_id = vehicle_id_;
    config.initialization_delay_ms = 200;
    config.poll_interval_ms = 500;
    config.batch_window_ms = 0;
    config.heartbeat_interval_ms = 0;

    DiscoverySyncBridge bridge(config);

    auto initial_stats = bridge.GetStats();
    EXPECT_EQ(initial_stats.events_sent, 0);
    EXPECT_FALSE(initial_stats.is_initialized);

    ASSERT_TRUE(bridge.Start());

    // Wait for initialization and full sync
    std::this_thread::sleep_for(1s);

    auto final_stats = bridge.GetStats();

    bridge.Stop();

    EXPECT_TRUE(final_stats.is_initialized);
    EXPECT_GT(final_stats.events_sent, 0);
    EXPECT_GT(final_stats.full_syncs_sent, 0);
    EXPECT_GT(final_stats.bytes_sent, 0);
    EXPECT_GT(final_stats.current_sequence, 0);
}

TEST_F(DiscoverySyncBridgeIntegrationTest, ChecksumConsistency) {
    DiscoverySyncBridgeConfig config;
    config.discovery_endpoint = TEST_DISCOVERY_ADDRESS;
    config.backend_transport_endpoint = "localhost:" +
        std::to_string(BACKEND_TRANSPORT_GRPC_PORT);
    config.sync_content_id = content_id::DISCOVERY_SYNC;
    config.vehicle_id = vehicle_id_;
    config.initialization_delay_ms = 200;
    config.poll_interval_ms = 500;
    config.batch_window_ms = 0;

    DiscoverySyncBridge bridge(config);
    ASSERT_TRUE(bridge.Start());

    std::this_thread::sleep_for(1s);

    // Get checksum multiple times - should be consistent if no changes
    uint32_t checksum1 = bridge.GetStateChecksum();
    std::this_thread::sleep_for(100ms);
    uint32_t checksum2 = bridge.GetStateChecksum();

    bridge.Stop();

    EXPECT_EQ(checksum1, checksum2) << "Checksum should be stable when state unchanged";
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
