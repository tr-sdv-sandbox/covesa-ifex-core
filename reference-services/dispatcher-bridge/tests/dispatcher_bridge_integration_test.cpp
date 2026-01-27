/**
 * @file dispatcher_bridge_integration_test.cpp
 * @brief Integration tests for DispatcherBridge with Docker MQTT broker
 *
 * Tests end-to-end RPC flow:
 * Cloud MQTT publish → Backend Transport → DispatcherBridge → Dispatcher → Echo Service → response via MQTT
 */

#include "../../backend-transport/tests/mqtt_test_fixture.hpp"
#include "backend_transport_client.hpp"
#include "backend_transport_server.hpp"
#include "dispatcher_bridge.hpp"
#include "ifex_content_ids.hpp"
#include "dispatcher-rpc-envelope.pb.h"

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

/**
 * @brief Combined test fixture for DispatcherBridge integration tests
 *
 * Manages:
 * - Docker MQTT broker (inherited from MqttTestFixture)
 * - Backend Transport Service (in-process)
 * - Discovery Service (forked process)
 * - Dispatcher Service (forked process)
 * - Echo Service (forked process)
 * - DispatcherBridge (in-process)
 */
class DispatcherBridgeIntegrationTest : public MqttTestFixture {
protected:
    // Test ports (different from production to avoid conflicts)
    static constexpr int TEST_DISCOVERY_PORT = 50199;
    static constexpr int TEST_DISPATCHER_PORT = 50198;
    static constexpr int TEST_ECHO_PORT = 50197;
    static constexpr int BACKEND_TRANSPORT_GRPC_PORT = 50196;

    static constexpr const char* TEST_DISCOVERY_ADDRESS = "localhost:50199";
    static constexpr const char* TEST_DISPATCHER_ADDRESS = "localhost:50198";
    static constexpr const char* TEST_ECHO_ADDRESS = "localhost:50197";

    // Process IDs for forked services
    static pid_t discovery_pid_;
    static pid_t dispatcher_pid_;
    static pid_t echo_pid_;

    // In-process components
    static std::unique_ptr<BackendTransportServer> backend_transport_;
    static std::unique_ptr<grpc::Server> backend_transport_grpc_;
    static std::unique_ptr<DispatcherBridge> dispatcher_bridge_;
    static std::string vehicle_id_;

    static void SetUpTestSuite() {
        MqttTestFixture::SetUpTestSuite();

        if (!container_started) {
            return;
        }

        vehicle_id_ = "dispatcher-bridge-test-vehicle";

        // Start Discovery Service first
        discovery_pid_ = start_service("discovery", TEST_DISCOVERY_PORT);
        if (!wait_for_service(TEST_DISCOVERY_ADDRESS)) {
            TearDownTestSuite();
            GTEST_SKIP() << "Discovery service failed to start";
            return;
        }

        // Start Dispatcher and Echo in parallel
        dispatcher_pid_ = start_service("dispatcher", TEST_DISPATCHER_PORT);
        echo_pid_ = start_service("echo", TEST_ECHO_PORT);

        if (!wait_for_service(TEST_DISPATCHER_ADDRESS) ||
            !wait_for_service(TEST_ECHO_ADDRESS)) {
            TearDownTestSuite();
            GTEST_SKIP() << "Dispatcher or Echo service failed to start";
            return;
        }

        // Start Backend Transport Server (in-process)
        BackendTransportServer::Config bt_config;
        bt_config.mqtt_host = mqtt_host;
        bt_config.mqtt_port = mqtt_port;
        bt_config.vehicle_id = vehicle_id_;
        bt_config.queue_size_per_content_id = 100;
        bt_config.persistence_dir = "/tmp/ifex-dispatcher-bridge-test";

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

        // Start DispatcherBridge (in-process)
        DispatcherBridgeConfig bridge_config;
        bridge_config.dispatcher_endpoint = TEST_DISPATCHER_ADDRESS;
        bridge_config.backend_transport_endpoint = "localhost:" + std::to_string(grpc_port);
        bridge_config.rpc_content_id = content_id::DISPATCHER_RPC;
        bridge_config.num_workers = 2;
        bridge_config.default_timeout_ms = 10000;

        dispatcher_bridge_ = std::make_unique<DispatcherBridge>(bridge_config);

        if (!dispatcher_bridge_->Start()) {
            TearDownTestSuite();
            GTEST_SKIP() << "Failed to start DispatcherBridge";
            return;
        }

        LOG(INFO) << "DispatcherBridge started successfully";
        std::this_thread::sleep_for(1s);
    }

    static void TearDownTestSuite() {
        LOG(INFO) << "Tearing down DispatcherBridge integration tests...";

        if (dispatcher_bridge_) {
            dispatcher_bridge_->Stop();
            dispatcher_bridge_.reset();
        }

        if (backend_transport_grpc_) {
            backend_transport_grpc_->Shutdown();
            backend_transport_grpc_.reset();
        }

        if (backend_transport_) {
            backend_transport_->Stop();
            backend_transport_.reset();
        }

        stop_service(echo_pid_, "echo");
        stop_service(dispatcher_pid_, "dispatcher");
        stop_service(discovery_pid_, "discovery");

        MqttTestFixture::TearDownTestSuite();
    }

    void SetUp() override {
        MqttTestFixture::SetUp();
        if (!dispatcher_bridge_ || !dispatcher_bridge_->IsRunning()) {
            GTEST_SKIP() << "DispatcherBridge not running";
        }
    }

    /// Publish RPC request directly to MQTT (simulating cloud)
    bool publishRpcRequest(const swdv::dispatcher_rpc_envelope::rpc_request_t& request) {
        std::string serialized;
        if (!request.SerializeToString(&serialized)) {
            LOG(ERROR) << "Failed to serialize RPC request";
            return false;
        }

        std::string topic = "c2v/" + vehicle_id_ + "/" + std::to_string(content_id::DISPATCHER_RPC);

        struct mosquitto* mosq = mosquitto_new("test-rpc-publisher", true, nullptr);
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
                               static_cast<int>(serialized.size()),
                               serialized.data(), 1, false);
        if (rc != MOSQ_ERR_SUCCESS) {
            LOG(ERROR) << "Failed to publish RPC request: " << mosquitto_strerror(rc);
            mosquitto_disconnect(mosq);
            mosquitto_destroy(mosq);
            return false;
        }

        mosquitto_loop(mosq, 1000, 1);
        mosquitto_disconnect(mosq);
        mosquitto_destroy(mosq);
        return true;
    }

    /// Subscribe to MQTT and wait for RPC response
    std::optional<swdv::dispatcher_rpc_envelope::rpc_response_t>
    waitForRpcResponse(const std::string& correlation_id, std::chrono::seconds timeout) {
        std::string topic = "v2c/" + vehicle_id_ + "/" + std::to_string(content_id::DISPATCHER_RPC);

        struct Context {
            std::string expected_correlation_id;
            std::optional<swdv::dispatcher_rpc_envelope::rpc_response_t> response;
            std::mutex mtx;
            std::condition_variable cv;
            bool done = false;
        };

        Context ctx;
        ctx.expected_correlation_id = correlation_id;

        struct mosquitto* mosq = mosquitto_new("test-rpc-subscriber", true, &ctx);
        if (!mosq) {
            return std::nullopt;
        }

        mosquitto_message_callback_set(mosq, [](struct mosquitto*, void* userdata,
                                                 const struct mosquitto_message* msg) {
            auto* ctx = static_cast<Context*>(userdata);

            swdv::dispatcher_rpc_envelope::rpc_response_t response;
            if (response.ParseFromArray(msg->payload, msg->payloadlen)) {
                std::lock_guard<std::mutex> lock(ctx->mtx);
                if (response.correlation_id() == ctx->expected_correlation_id) {
                    ctx->response = std::move(response);
                    ctx->done = true;
                    ctx->cv.notify_all();
                }
            }
        });

        if (mosquitto_connect(mosq, mqtt_host.c_str(), mqtt_port, 60) != MOSQ_ERR_SUCCESS) {
            mosquitto_destroy(mosq);
            return std::nullopt;
        }

        mosquitto_subscribe(mosq, nullptr, topic.c_str(), 1);

        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            mosquitto_loop(mosq, 100, 1);

            std::unique_lock<std::mutex> lock(ctx.mtx);
            if (ctx.done) {
                break;
            }
        }

        mosquitto_disconnect(mosq);
        mosquitto_destroy(mosq);

        std::lock_guard<std::mutex> lock(ctx.mtx);
        return ctx.response;
    }

private:
    static pid_t start_service(const std::string& name, int port) {
        std::string build_dir = get_build_dir();
        std::string executable;

        if (name == "discovery") {
            executable = build_dir + "/reference-services/discovery/ifex-discovery-service";
        } else if (name == "dispatcher") {
            executable = build_dir + "/reference-services/dispatcher/ifex-dispatcher-service";
        } else if (name == "echo") {
            executable = build_dir + "/test-services/ifex-echo-service";
        }

        if (!fs::exists(executable)) {
            LOG(ERROR) << "Service executable not found: " << executable;
            return 0;
        }

        pid_t pid = fork();

        if (pid == 0) {
            // Child process
            std::string port_str = std::to_string(port);
            std::string listen_addr = "0.0.0.0:" + port_str;

            setenv("GLOG_logtostderr", "1", 1);
            setenv("IFEX_SCHEMA_DIR", get_schema_dir().c_str(), 1);

            // Redirect output to log files
            std::string log_file = "/tmp/ifex_bridge_test_" + name + "_" + port_str + ".log";
            freopen(log_file.c_str(), "w", stdout);
            freopen(log_file.c_str(), "w", stderr);

            if (name == "discovery") {
                std::string listen_param = "--listen=" + listen_addr;
                execl(executable.c_str(), executable.c_str(), listen_param.c_str(), nullptr);
            } else if (name == "dispatcher") {
                std::string listen_param = "--listen=" + listen_addr;
                std::string discovery_param = "--discovery=" + std::string(TEST_DISCOVERY_ADDRESS);
                // Schema is now embedded at compile time, no --ifex-schema needed
                execl(executable.c_str(), executable.c_str(),
                      listen_param.c_str(),
                      discovery_param.c_str(),
                      nullptr);
            } else if (name == "echo") {
                std::string listen_param = "--listen=" + listen_addr;
                std::string discovery_param = "--discovery=" + std::string(TEST_DISCOVERY_ADDRESS);
                // Schema is now embedded at compile time, no --ifex-schema needed
                execl(executable.c_str(), executable.c_str(),
                      listen_param.c_str(),
                      discovery_param.c_str(),
                      nullptr);
            }

            LOG(ERROR) << "Failed to exec " << executable << ": " << strerror(errno);
            _exit(1);
        } else if (pid < 0) {
            LOG(ERROR) << "Failed to fork for " << name << " service";
            return 0;
        }

        LOG(INFO) << "Started " << name << " service with PID " << pid;
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

    static std::string get_schema_dir() {
        // IFEX schema files are installed to <build>/ifex/
        std::string build_dir = get_build_dir();
        fs::path schema_dir = fs::path(build_dir) / "ifex";
        if (fs::exists(schema_dir)) {
            return schema_dir.string();
        }

        // Fallback: try the parent build directory
        fs::path current = fs::current_path();
        while (!current.empty() && current != current.root_path()) {
            if (fs::exists(current / "CMakeCache.txt")) {
                if (fs::exists(current / "ifex")) {
                    return (current / "ifex").string();
                }
            }
            current = current.parent_path();
        }

        return "./ifex";
    }
};

// Static member definitions
pid_t DispatcherBridgeIntegrationTest::discovery_pid_ = 0;
pid_t DispatcherBridgeIntegrationTest::dispatcher_pid_ = 0;
pid_t DispatcherBridgeIntegrationTest::echo_pid_ = 0;
std::unique_ptr<BackendTransportServer> DispatcherBridgeIntegrationTest::backend_transport_;
std::unique_ptr<grpc::Server> DispatcherBridgeIntegrationTest::backend_transport_grpc_;
std::unique_ptr<DispatcherBridge> DispatcherBridgeIntegrationTest::dispatcher_bridge_;
std::string DispatcherBridgeIntegrationTest::vehicle_id_;

// =============================================================================
// End-to-End RPC Tests
// =============================================================================

TEST_F(DispatcherBridgeIntegrationTest, EchoServiceRoundTrip) {
    // Create RPC request for echo service
    swdv::dispatcher_rpc_envelope::rpc_request_t request;
    request.set_correlation_id("test-echo-001");
    request.set_service_name("echo_service");
    request.set_method_name("echo");
    request.set_parameters_json(R"({"message": "Hello from cloud!"})");
    request.set_timeout_ms(5000);
    request.set_request_timestamp_ms(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    // Start listening for response before sending request
    auto response_future = std::async(std::launch::async, [&]() {
        return waitForRpcResponse("test-echo-001", 15s);
    });

    // Give subscriber time to connect
    std::this_thread::sleep_for(500ms);

    // Publish RPC request via MQTT
    ASSERT_TRUE(publishRpcRequest(request)) << "Failed to publish RPC request";

    // Wait for response
    auto response = response_future.get();
    ASSERT_TRUE(response.has_value()) << "Should receive RPC response";

    EXPECT_EQ(response->correlation_id(), "test-echo-001");
    EXPECT_EQ(response->status(), swdv::dispatcher_rpc_envelope::SUCCESS);
    EXPECT_FALSE(response->result_json().empty());
    EXPECT_GT(response->duration_ms(), 0);

    LOG(INFO) << "Echo response: " << response->result_json();
}

TEST_F(DispatcherBridgeIntegrationTest, ServiceNotFoundReturnsError) {
    swdv::dispatcher_rpc_envelope::rpc_request_t request;
    request.set_correlation_id("test-notfound-001");
    request.set_service_name("nonexistent_service");
    request.set_method_name("some_method");
    request.set_parameters_json("{}");
    request.set_timeout_ms(5000);

    auto response_future = std::async(std::launch::async, [&]() {
        return waitForRpcResponse("test-notfound-001", 15s);
    });

    std::this_thread::sleep_for(500ms);

    ASSERT_TRUE(publishRpcRequest(request));

    auto response = response_future.get();
    ASSERT_TRUE(response.has_value()) << "Should receive error response";

    EXPECT_EQ(response->correlation_id(), "test-notfound-001");
    EXPECT_EQ(response->status(), swdv::dispatcher_rpc_envelope::SERVICE_UNAVAILABLE);
    EXPECT_FALSE(response->error_message().empty());
}

TEST_F(DispatcherBridgeIntegrationTest, MethodNotFoundReturnsError) {
    swdv::dispatcher_rpc_envelope::rpc_request_t request;
    request.set_correlation_id("test-method-notfound-001");
    request.set_service_name("echo_service");
    request.set_method_name("nonexistent_method");
    request.set_parameters_json("{}");
    request.set_timeout_ms(5000);

    auto response_future = std::async(std::launch::async, [&]() {
        return waitForRpcResponse("test-method-notfound-001", 15s);
    });

    std::this_thread::sleep_for(500ms);

    ASSERT_TRUE(publishRpcRequest(request));

    auto response = response_future.get();
    ASSERT_TRUE(response.has_value()) << "Should receive error response";

    EXPECT_EQ(response->correlation_id(), "test-method-notfound-001");
    EXPECT_EQ(response->status(), swdv::dispatcher_rpc_envelope::METHOD_NOT_FOUND);
}

TEST_F(DispatcherBridgeIntegrationTest, MultipleConcurrentRequests) {
    // Send multiple requests sequentially but quickly, verify all responses come back
    const int NUM_REQUESTS = 3;
    std::vector<std::string> correlation_ids;

    // Collect all responses in one subscriber
    std::string topic = "v2c/" + vehicle_id_ + "/" + std::to_string(content_id::DISPATCHER_RPC);

    struct Context {
        std::map<std::string, swdv::dispatcher_rpc_envelope::rpc_response_t> responses;
        std::mutex mtx;
        std::condition_variable cv;
        int expected_count;
    };

    Context ctx;
    ctx.expected_count = NUM_REQUESTS;

    struct mosquitto* mosq = mosquitto_new("test-concurrent-subscriber", true, &ctx);
    ASSERT_TRUE(mosq != nullptr);

    mosquitto_message_callback_set(mosq, [](struct mosquitto*, void* userdata,
                                             const struct mosquitto_message* msg) {
        auto* ctx = static_cast<Context*>(userdata);
        swdv::dispatcher_rpc_envelope::rpc_response_t response;
        if (response.ParseFromArray(msg->payload, msg->payloadlen)) {
            std::lock_guard<std::mutex> lock(ctx->mtx);
            ctx->responses[response.correlation_id()] = std::move(response);
            ctx->cv.notify_all();
        }
    });

    ASSERT_EQ(mosquitto_connect(mosq, mqtt_host.c_str(), mqtt_port, 60), MOSQ_ERR_SUCCESS);
    ASSERT_EQ(mosquitto_subscribe(mosq, nullptr, topic.c_str(), 1), MOSQ_ERR_SUCCESS);

    // Start polling loop in background
    auto poller = std::async(std::launch::async, [&]() {
        auto deadline = std::chrono::steady_clock::now() + 20s;
        while (std::chrono::steady_clock::now() < deadline) {
            mosquitto_loop(mosq, 100, 1);
            std::lock_guard<std::mutex> lock(ctx.mtx);
            if (static_cast<int>(ctx.responses.size()) >= NUM_REQUESTS) {
                break;
            }
        }
    });

    std::this_thread::sleep_for(500ms);

    // Send all requests
    for (int i = 0; i < NUM_REQUESTS; ++i) {
        std::string correlation_id = "test-concurrent-" + std::to_string(i);
        correlation_ids.push_back(correlation_id);

        swdv::dispatcher_rpc_envelope::rpc_request_t request;
        request.set_correlation_id(correlation_id);
        request.set_service_name("echo_service");
        request.set_method_name("echo");
        request.set_parameters_json(R"({"message": "Request )" + std::to_string(i) + R"("})");
        request.set_timeout_ms(10000);

        ASSERT_TRUE(publishRpcRequest(request)) << "Failed to publish request " << i;
        std::this_thread::sleep_for(100ms);  // Small delay between requests
    }

    // Wait for all responses
    poller.get();

    mosquitto_disconnect(mosq);
    mosquitto_destroy(mosq);

    // Verify all responses received
    std::lock_guard<std::mutex> lock(ctx.mtx);
    int success_count = 0;
    for (const auto& cid : correlation_ids) {
        auto it = ctx.responses.find(cid);
        if (it != ctx.responses.end()) {
            EXPECT_EQ(it->second.correlation_id(), cid);
            if (it->second.status() == swdv::dispatcher_rpc_envelope::SUCCESS) {
                success_count++;
            }
        }
    }

    EXPECT_EQ(success_count, NUM_REQUESTS) << "All concurrent requests should succeed";
}

TEST_F(DispatcherBridgeIntegrationTest, StatsIncrementAfterRequests) {
    auto initial_stats = dispatcher_bridge_->GetStats();

    // Send a request
    swdv::dispatcher_rpc_envelope::rpc_request_t request;
    request.set_correlation_id("test-stats-001");
    request.set_service_name("echo_service");
    request.set_method_name("echo");
    request.set_parameters_json(R"({"message": "test"})");
    request.set_timeout_ms(5000);

    auto response_future = std::async(std::launch::async, [&]() {
        return waitForRpcResponse("test-stats-001", 15s);
    });

    std::this_thread::sleep_for(500ms);
    ASSERT_TRUE(publishRpcRequest(request));

    auto response = response_future.get();
    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(response->status(), swdv::dispatcher_rpc_envelope::SUCCESS);

    // Wait for stats to update
    std::this_thread::sleep_for(500ms);

    auto final_stats = dispatcher_bridge_->GetStats();
    EXPECT_GT(final_stats.requests_received, initial_stats.requests_received);
    EXPECT_GT(final_stats.requests_completed, initial_stats.requests_completed);
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
