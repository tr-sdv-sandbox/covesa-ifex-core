/**
 * @file scheduler_sync_bridge_integration_test.cpp
 * @brief Integration tests for SchedulerSyncBridge with Docker MQTT broker
 *
 * Tests end-to-end sync flow:
 * Scheduler service → SchedulerSyncBridge → Backend Transport → MQTT
 */

#include "../../backend-transport/tests/mqtt_test_fixture.hpp"
#include "backend_transport_client.hpp"
#include "backend_transport_server.hpp"
#include "scheduler_sync_bridge.hpp"
#include "ifex_content_ids.hpp"
#include "scheduler-sync-v2.pb.h"
#include "ifex-scheduler-service.grpc.pb.h"
#include "service-discovery-service.grpc.pb.h"

#include <gtest/gtest.h>
#include <glog/logging.h>
#include <grpcpp/grpcpp.h>
#include <mosquitto.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <filesystem>
#include <future>
#include <map>
#include <mutex>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
// Global pointers for signal handler cleanup
std::atomic<pid_t> g_discovery_pid{0};
std::atomic<pid_t> g_echo_pid{0};
std::atomic<pid_t> g_scheduler_pid{0};

void cleanup_processes() {
    auto kill_process = [](pid_t pid, const char* name) {
        if (pid > 0) {
            LOG(INFO) << "Signal handler: killing " << name << " (PID: " << pid << ")";
            kill(pid, SIGKILL);
            waitpid(pid, nullptr, WNOHANG);
        }
    };
    kill_process(g_scheduler_pid.load(), "scheduler");
    kill_process(g_echo_pid.load(), "echo");
    kill_process(g_discovery_pid.load(), "discovery");
}

void signal_handler(int sig) {
    LOG(WARNING) << "Received signal " << sig << ", cleaning up test processes...";
    cleanup_processes();
    _exit(128 + sig);
}
}  // namespace

namespace fs = std::filesystem;

namespace ifex::test {

using namespace ifex::client;
using namespace ifex::reference;
using namespace std::chrono_literals;
namespace sync_v2 = swdv::scheduler_sync_v2;
namespace scheduler_pb = swdv::ifex_scheduler;
namespace discovery_pb = swdv::service_discovery;

/**
 * @brief Test fixture for SchedulerSyncBridge integration tests
 */
class SchedulerSyncBridgeIntegrationTest : public MqttTestFixture {
protected:
    static constexpr int TEST_DISCOVERY_PORT = 50391;
    static constexpr int TEST_ECHO_PORT = 50392;
    static constexpr int TEST_SCHEDULER_PORT = 50393;
    static constexpr int BACKEND_TRANSPORT_GRPC_PORT = 50390;

    static constexpr const char* TEST_DISCOVERY_ADDRESS = "localhost:50391";
    static constexpr const char* TEST_ECHO_ADDRESS = "localhost:50392";
    static constexpr const char* TEST_SCHEDULER_ADDRESS = "localhost:50393";

    static pid_t discovery_pid_;
    static pid_t echo_pid_;
    static pid_t scheduler_pid_;
    static std::unique_ptr<BackendTransportServer> backend_transport_;
    static std::unique_ptr<grpc::Server> backend_transport_grpc_;
    static std::string vehicle_id_;

    static void SetUpTestSuite() {
        MqttTestFixture::SetUpTestSuite();

        if (!container_started) {
            return;
        }

        vehicle_id_ = "scheduler-sync-test-vehicle";

        // Start Discovery service first (others depend on it)
        discovery_pid_ = start_discovery_service(TEST_DISCOVERY_PORT);
        g_discovery_pid.store(discovery_pid_);
        if (!wait_for_service(TEST_DISCOVERY_ADDRESS)) {
            TearDownTestSuite();
            GTEST_SKIP() << "Discovery service failed to start";
            return;
        }

        // Start Echo service (test target for scheduled jobs)
        echo_pid_ = start_echo_service(TEST_ECHO_PORT, TEST_DISCOVERY_ADDRESS);
        g_echo_pid.store(echo_pid_);
        if (!wait_for_service(TEST_ECHO_ADDRESS)) {
            TearDownTestSuite();
            GTEST_SKIP() << "Echo service failed to start";
            return;
        }

        // Give echo service time to complete registration with Discovery
        std::this_thread::sleep_for(200ms);

        // Start Scheduler service (with discovery endpoint)
        scheduler_pid_ = start_scheduler_service(TEST_SCHEDULER_PORT, TEST_DISCOVERY_ADDRESS);
        g_scheduler_pid.store(scheduler_pid_);
        if (!wait_for_service(TEST_SCHEDULER_ADDRESS)) {
            TearDownTestSuite();
            GTEST_SKIP() << "Scheduler service failed to start";
            return;
        }

        // Start Backend Transport Server (in-process)
        BackendTransportServer::Config bt_config;
        bt_config.mqtt_host = mqtt_host;
        bt_config.mqtt_port = mqtt_port;
        bt_config.vehicle_id = vehicle_id_;
        bt_config.queue_size_per_content_id = 100;
        bt_config.persistence_dir = "/tmp/ifex-scheduler-sync-test";

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
    }

    static void TearDownTestSuite() {
        LOG(INFO) << "Tearing down SchedulerSyncBridge integration tests...";

        if (backend_transport_grpc_) {
            backend_transport_grpc_->Shutdown();
            backend_transport_grpc_.reset();
        }

        if (backend_transport_) {
            backend_transport_->Stop();
            backend_transport_.reset();
        }

        stop_service(scheduler_pid_, "scheduler");
        stop_service(echo_pid_, "echo");
        stop_service(discovery_pid_, "discovery");

        MqttTestFixture::TearDownTestSuite();
    }

    void SetUp() override {
        MqttTestFixture::SetUp();
        if (!backend_transport_) {
            GTEST_SKIP() << "Backend Transport not running";
        }
    }

    /// Subscribe to MQTT and collect v2 sync messages
    std::vector<sync_v2::V2C_SyncMessage> collectSyncMessages(
        int expected_count, std::chrono::seconds timeout) {

        std::string topic = "v2c/" + vehicle_id_ + "/" +
                            std::to_string(content_id::SCHEDULER_SYNC);

        struct Context {
            std::vector<sync_v2::V2C_SyncMessage> messages;
            std::mutex mtx;
            std::condition_variable cv;
            int expected_count;
        };

        Context ctx;
        ctx.expected_count = expected_count;

        struct mosquitto* mosq = mosquitto_new("test-scheduler-sync-subscriber", true, &ctx);
        EXPECT_TRUE(mosq != nullptr);

        mosquitto_message_callback_set(mosq, [](struct mosquitto*, void* userdata,
                                                 const struct mosquitto_message* msg) {
            auto* ctx = static_cast<Context*>(userdata);
            sync_v2::V2C_SyncMessage message;
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

    /// Create a test job in the Scheduler
    std::string createTestJob(const std::string& title, const std::string& service,
                               const std::string& method) {
        auto channel = grpc::CreateChannel(TEST_SCHEDULER_ADDRESS,
                                           grpc::InsecureChannelCredentials());
        auto stub = scheduler_pb::create_job_service::NewStub(channel);

        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + 5s);

        scheduler_pb::create_job_request request;
        request.mutable_job()->set_title(title);
        request.mutable_job()->set_service(service);
        request.mutable_job()->set_method(method);
        request.mutable_job()->set_parameters("{}");
        // Schedule for the future to keep it in PENDING (year 2099 in epoch ms)
        request.mutable_job()->set_scheduled_time_ms(4102444799000ULL);

        scheduler_pb::create_job_response response;
        auto status = stub->create_job(&context, request, &response);

        if (!status.ok()) {
            LOG(ERROR) << "Failed to create job (gRPC error): " << status.error_code()
                       << " - " << status.error_message();
            return "";
        }
        if (!response.success()) {
            LOG(ERROR) << "Failed to create job (service error): " << response.message();
            return "";
        }
        LOG(INFO) << "Created test job: " << title << " (id=" << response.job_id() << ")";
        return response.job_id();
    }

    /// Register a test service with Discovery (needed before creating jobs)
    bool registerTestService(const std::string& name, int port) {
        auto channel = grpc::CreateChannel(TEST_DISCOVERY_ADDRESS,
                                           grpc::InsecureChannelCredentials());
        auto stub = discovery_pb::register_service_service::NewStub(channel);

        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + 5s);

        discovery_pb::register_service_request request;
        request.mutable_service_info()->set_name(name);
        request.mutable_service_info()->set_version("1.0.0");
        request.mutable_service_info()->set_description("Test service for scheduler sync tests");
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
        LOG(ERROR) << "Failed to register test service: " << status.error_message();
        return false;
    }

    /// Delete a job from the Scheduler
    bool deleteJob(const std::string& job_id) {
        auto channel = grpc::CreateChannel(TEST_SCHEDULER_ADDRESS,
                                           grpc::InsecureChannelCredentials());
        auto stub = scheduler_pb::delete_job_service::NewStub(channel);

        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + 5s);

        scheduler_pb::delete_job_request request;
        request.set_job_id(job_id);

        scheduler_pb::delete_job_response response;
        auto status = stub->delete_job(&context, request, &response);

        return status.ok() && response.success();
    }

private:
    static pid_t start_discovery_service(int port) {
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

            std::string log_file = "/tmp/ifex_scheduler_sync_test_discovery_" + port_str + ".log";
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

    static pid_t start_scheduler_service(int port, const char* discovery_addr) {
        std::string build_dir = get_build_dir();
        std::string executable = build_dir + "/reference-services/scheduler/ifex-scheduler-service";

        if (!fs::exists(executable)) {
            LOG(ERROR) << "Scheduler executable not found: " << executable;
            return 0;
        }

        pid_t pid = fork();

        if (pid == 0) {
            std::string port_str = std::to_string(port);
            std::string listen_addr = "0.0.0.0:" + port_str;

            setenv("GLOG_logtostderr", "1", 1);
            // Disable persistence for tests - start fresh each time
            unsetenv("SCHEDULER_PERSISTENCE_DIR");

            std::string log_file = "/tmp/ifex_scheduler_sync_test_scheduler_" + port_str + ".log";
            freopen(log_file.c_str(), "w", stdout);
            freopen(log_file.c_str(), "w", stderr);

            std::string listen_param = "--listen=" + listen_addr;
            std::string discovery_param = std::string("--discovery=") + discovery_addr;
            execl(executable.c_str(), executable.c_str(),
                  listen_param.c_str(), discovery_param.c_str(), nullptr);

            LOG(ERROR) << "Failed to exec " << executable << ": " << strerror(errno);
            _exit(1);
        } else if (pid < 0) {
            LOG(ERROR) << "Failed to fork for scheduler service";
            return 0;
        }

        LOG(INFO) << "Started scheduler service with PID " << pid;
        return pid;
    }

    static pid_t start_echo_service(int port, const char* discovery_addr) {
        std::string build_dir = get_build_dir();
        std::string executable = build_dir + "/test-services/ifex-echo-service";

        if (!fs::exists(executable)) {
            LOG(ERROR) << "Echo executable not found: " << executable;
            return 0;
        }

        pid_t pid = fork();

        if (pid == 0) {
            std::string port_str = std::to_string(port);
            std::string listen_addr = "0.0.0.0:" + port_str;

            setenv("GLOG_logtostderr", "1", 1);
            // Set IFEX schema directory for echo service
            std::string ifex_dir = build_dir + "/ifex";
            setenv("IFEX_SCHEMA_DIR", ifex_dir.c_str(), 1);

            std::string log_file = "/tmp/ifex_scheduler_sync_test_echo_" + port_str + ".log";
            freopen(log_file.c_str(), "w", stdout);
            freopen(log_file.c_str(), "w", stderr);

            std::string listen_param = "--listen=" + listen_addr;
            std::string discovery_param = std::string("--discovery=") + discovery_addr;
            execl(executable.c_str(), executable.c_str(),
                  listen_param.c_str(), discovery_param.c_str(), nullptr);

            LOG(ERROR) << "Failed to exec " << executable << ": " << strerror(errno);
            _exit(1);
        } else if (pid < 0) {
            LOG(ERROR) << "Failed to fork for echo service";
            return 0;
        }

        LOG(INFO) << "Started echo service with PID " << pid;
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
pid_t SchedulerSyncBridgeIntegrationTest::discovery_pid_ = 0;
pid_t SchedulerSyncBridgeIntegrationTest::echo_pid_ = 0;
pid_t SchedulerSyncBridgeIntegrationTest::scheduler_pid_ = 0;
std::unique_ptr<BackendTransportServer> SchedulerSyncBridgeIntegrationTest::backend_transport_;
std::unique_ptr<grpc::Server> SchedulerSyncBridgeIntegrationTest::backend_transport_grpc_;
std::string SchedulerSyncBridgeIntegrationTest::vehicle_id_;

// =============================================================================
// Integration Tests
// =============================================================================

TEST_F(SchedulerSyncBridgeIntegrationTest, FullSyncOnStartup) {
    // Use echo_service (started in fixture) as target for scheduled jobs
    std::string job_id = createTestJob("test-sync-job", "echo_service", "echo");
    ASSERT_FALSE(job_id.empty());

    // Verify job exists in scheduler before proceeding
    {
        auto channel = grpc::CreateChannel(TEST_SCHEDULER_ADDRESS,
                                           grpc::InsecureChannelCredentials());
        auto stub = scheduler_pb::get_jobs_service::NewStub(channel);

        // Retry a few times to handle any race conditions
        bool job_found = false;
        for (int attempt = 0; attempt < 10 && !job_found; ++attempt) {
            if (attempt > 0) {
                std::this_thread::sleep_for(100ms);
            }

            grpc::ClientContext context;
            context.set_deadline(std::chrono::system_clock::now() + 5s);
            scheduler_pb::get_jobs_request request;
            scheduler_pb::get_jobs_response response;

            auto status = stub->get_jobs(&context, request, &response);
            if (status.ok()) {
                for (const auto& job : response.jobs()) {
                    if (job.id() == job_id) {
                        job_found = true;
                        break;
                    }
                }
            }
        }
        ASSERT_TRUE(job_found) << "Job should exist in scheduler before sync bridge starts";
    }

    // Start collecting messages
    auto collect_future = std::async(std::launch::async, [this]() {
        return collectSyncMessages(1, 5s);
    });

    std::this_thread::sleep_for(50ms);

    // Start sync bridge with short init delay
    SchedulerSyncBridgeConfig config;
    config.scheduler_endpoint = TEST_SCHEDULER_ADDRESS;
    config.backend_transport_endpoint = "localhost:" +
        std::to_string(BACKEND_TRANSPORT_GRPC_PORT);
    config.sync_content_id = content_id::SCHEDULER_SYNC;
    config.vehicle_id = vehicle_id_;
    config.initialization_delay_ms = 100;
    config.poll_interval_ms = 100;
    config.batch_window_ms = 0;  // Immediate send
    config.heartbeat_interval_ms = 0;  // Disable heartbeat

    SchedulerSyncBridge bridge(config);
    ASSERT_TRUE(bridge.Start());

    // Wait for full sync message
    auto messages = collect_future.get();

    bridge.Stop();

    ASSERT_GE(messages.size(), 1) << "Should receive at least one sync message";

    // First message should contain job records (v2 protocol)
    const auto& first_msg = messages[0];
    EXPECT_EQ(first_msg.vehicle_id(), vehicle_id_);
    EXPECT_FALSE(first_msg.bridge_instance_id().empty());
    EXPECT_FALSE(first_msg.sync_id().empty()) << "Should have sync_id";
    EXPECT_GT(first_msg.sync_timestamp_ms(), 0) << "Should have timestamp";

    // Full sync includes all jobs - check that our job is present
    bool found_job = false;
    for (const auto& job : first_msg.jobs()) {
        if (job.job_id() == job_id) {
            found_job = true;
            EXPECT_EQ(job.title(), "test-sync-job");
            EXPECT_EQ(job.service(), "echo_service");
            EXPECT_EQ(job.method(), "echo");
            break;
        }
    }
    EXPECT_TRUE(found_job) << "Should have job record in sync message";

    // Cleanup
    deleteJob(job_id);
}

TEST_F(SchedulerSyncBridgeIntegrationTest, DeltaSyncOnJobCreation) {
    // Start collecting messages FIRST before bridge starts
    // This ensures we don't miss any messages including the initial full sync
    auto collect_future = std::async(std::launch::async, [this]() {
        // Collect up to 5 messages to handle any stale messages + initial sync + delta
        return collectSyncMessages(5, 8s);
    });

    // Give the MQTT subscriber time to connect and subscribe
    std::this_thread::sleep_for(300ms);

    SchedulerSyncBridgeConfig config;
    config.scheduler_endpoint = TEST_SCHEDULER_ADDRESS;
    config.backend_transport_endpoint = "localhost:" +
        std::to_string(BACKEND_TRANSPORT_GRPC_PORT);
    config.sync_content_id = content_id::SCHEDULER_SYNC;
    config.vehicle_id = vehicle_id_;
    config.initialization_delay_ms = 100;
    config.poll_interval_ms = 100;
    config.batch_window_ms = 0;
    config.heartbeat_interval_ms = 0;

    SchedulerSyncBridge bridge(config);
    ASSERT_TRUE(bridge.Start());

    // Wait for initialization to complete
    std::this_thread::sleep_for(200ms);
    ASSERT_TRUE(bridge.IsInitialized());

    // Create a NEW job (this triggers the delta sync on next poll)
    std::string job_id = createTestJob("delta-test-job", "echo_service", "echo");
    ASSERT_FALSE(job_id.empty());
    LOG(INFO) << "Created delta-test-job with id: " << job_id;

    // Wait for delta sync - poll_interval_ms=100, so should happen quickly
    // But give extra time in case of slower systems
    std::this_thread::sleep_for(500ms);

    // Stop bridge before getting messages to ensure all syncs are sent
    bridge.Stop();

    // Get collected messages
    auto messages = collect_future.get();

    // Should have received messages - look for our new job in ANY message
    ASSERT_GE(messages.size(), 1) << "Should receive sync messages";

    bool found_job = false;
    for (size_t i = 0; i < messages.size(); ++i) {
        const auto& msg = messages[i];
        LOG(INFO) << "Checking message " << i << ": " << msg.jobs_size() << " jobs";
        for (const auto& job : msg.jobs()) {
            LOG(INFO) << "  Job: " << job.job_id() << " (" << job.title() << ")";
            if (job.job_id() == job_id) {
                found_job = true;
                EXPECT_EQ(job.title(), "delta-test-job");
                EXPECT_EQ(job.service(), "echo_service");
                EXPECT_EQ(job.method(), "echo");
                break;
            }
        }
        if (found_job) break;
    }
    EXPECT_TRUE(found_job) << "Should have job record in sync message, job_id=" << job_id
                           << ", received " << messages.size() << " messages";

    // Cleanup
    deleteJob(job_id);
}

TEST_F(SchedulerSyncBridgeIntegrationTest, StatsAfterSync) {
    SchedulerSyncBridgeConfig config;
    config.scheduler_endpoint = TEST_SCHEDULER_ADDRESS;
    config.backend_transport_endpoint = "localhost:" +
        std::to_string(BACKEND_TRANSPORT_GRPC_PORT);
    config.sync_content_id = content_id::SCHEDULER_SYNC;
    config.vehicle_id = vehicle_id_;
    config.initialization_delay_ms = 100;
    config.poll_interval_ms = 100;
    config.batch_window_ms = 0;
    config.heartbeat_interval_ms = 0;

    SchedulerSyncBridge bridge(config);
    ASSERT_TRUE(bridge.Start());

    // Wait for full sync
    std::this_thread::sleep_for(300ms);

    auto stats = bridge.GetStats();
    EXPECT_TRUE(stats.is_initialized);
    EXPECT_GE(stats.full_syncs_sent, 1);
    EXPECT_GT(stats.bytes_sent, 0);
    EXPECT_GT(stats.current_sequence, 0);

    bridge.Stop();
}

TEST_F(SchedulerSyncBridgeIntegrationTest, ChecksumConsistency) {
    SchedulerSyncBridgeConfig config;
    config.scheduler_endpoint = TEST_SCHEDULER_ADDRESS;
    config.backend_transport_endpoint = "localhost:" +
        std::to_string(BACKEND_TRANSPORT_GRPC_PORT);
    config.sync_content_id = content_id::SCHEDULER_SYNC;
    config.vehicle_id = vehicle_id_;
    config.initialization_delay_ms = 100;
    config.poll_interval_ms = 100;
    config.batch_window_ms = 0;
    config.heartbeat_interval_ms = 0;

    SchedulerSyncBridge bridge(config);
    ASSERT_TRUE(bridge.Start());

    // Wait for initialization
    std::this_thread::sleep_for(300ms);
    ASSERT_TRUE(bridge.IsInitialized());

    // Get checksum multiple times - should be consistent
    uint32_t checksum1 = bridge.GetStateChecksum();
    std::this_thread::sleep_for(200ms);
    uint32_t checksum2 = bridge.GetStateChecksum();

    // Without changes, checksum should be the same
    EXPECT_EQ(checksum1, checksum2);

    bridge.Stop();
}

}  // namespace ifex::test

int main(int argc, char** argv) {
    google::InitGoogleLogging(argv[0]);
    FLAGS_logtostderr = true;

    // Install signal handlers to clean up child processes on interrupt
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    ::testing::InitGoogleTest(&argc, argv);
    mosquitto_lib_init();
    int result = RUN_ALL_TESTS();
    mosquitto_lib_cleanup();

    // Clean up any remaining processes
    cleanup_processes();

    return result;
}
