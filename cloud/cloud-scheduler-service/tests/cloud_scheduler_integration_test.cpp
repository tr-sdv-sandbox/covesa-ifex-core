/**
 * @file cloud_scheduler_integration_test.cpp
 * @brief Integration tests for CloudSchedulerService bidirectional sync
 *
 * Tests end-to-end flow with REAL services:
 *
 * Cloud side:
 *   CloudSchedulerService → CloudBackendTransportClient → MQTT
 *
 * Vehicle side:
 *   BackendTransportServer ← MQTT
 *        ↓
 *   SchedulerSyncBridge → Scheduler service (forked process)
 *        ↓
 *   BackendTransportClient → MQTT → Cloud
 *
 * Key verification:
 * - Epoch milliseconds format works correctly (scheduled_time_ms, end_time_ms)
 * - Commands flow c2v (cloud-to-vehicle)
 * - Sync messages flow v2c (vehicle-to-cloud)
 */

#include "cloud_scheduler_service.hpp"
#include "cloud_backend_transport_server.hpp"
#include "cloud_backend_transport_client.hpp"
#include "backend_transport_server.hpp"
#include "backend_transport_client.hpp"
#include "scheduler_sync_bridge.hpp"
#include "ifex_content_ids.hpp"

#include <gtest/gtest.h>
#include <glog/logging.h>
#include <grpcpp/grpcpp.h>
#include <mosquitto.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <filesystem>
#include <mutex>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
// Global pointers for signal handler cleanup
std::atomic<pid_t> g_discovery_pid{0};
std::atomic<pid_t> g_echo_pid{0};
std::atomic<pid_t> g_dispatcher_pid{0};
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
    kill_process(g_dispatcher_pid.load(), "dispatcher");
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

namespace ifex::cloud::test {

using namespace std::chrono_literals;

// =============================================================================
// MQTT Test Fixture
// =============================================================================

class MqttTestFixture : public ::testing::Test {
protected:
    static constexpr const char* MQTT_IMAGE = "eclipse-mosquitto:2";
    static constexpr const char* CONTAINER_NAME = "ifex-cloud-scheduler-test-broker";
    static constexpr const char* MQTT_PORT = "11886";
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
int MqttTestFixture::mqtt_port = 11886;
bool MqttTestFixture::container_started = false;

// =============================================================================
// Basic Unit Tests (no MQTT required)
// =============================================================================

class CloudSchedulerServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
    }
};

TEST_F(CloudSchedulerServiceTest, CanInstantiate) {
    CloudSchedulerServiceConfig config;
    config.backend_transport_address = "localhost:50201";
    config.scheduler_content_id = 202;

    CloudSchedulerService service(config);
    EXPECT_FALSE(service.IsRunning());
}

TEST_F(CloudSchedulerServiceTest, StartWithoutTransportFails) {
    CloudSchedulerServiceConfig config;
    config.backend_transport_address = "localhost:59999";
    config.scheduler_content_id = 202;

    CloudSchedulerService service(config);
    EXPECT_TRUE(service.Start());
    EXPECT_TRUE(service.IsRunning());
    service.Stop();
    EXPECT_FALSE(service.IsRunning());
}

TEST_F(CloudSchedulerServiceTest, JobCountStartsAtZero) {
    CloudSchedulerServiceConfig config;
    CloudSchedulerService service(config);
    EXPECT_EQ(service.GetTotalJobCount(), 0);
    EXPECT_EQ(service.GetJobCount("any-vehicle"), 0);
}

// =============================================================================
// Full Stack Integration Test
// =============================================================================

class SchedulerBidirectionalSyncTest : public MqttTestFixture {
protected:
    static constexpr uint32_t SCHEDULER_CONTENT_ID = 202;
    static constexpr const char* TEST_VEHICLE_ID = "test-vehicle-scheduler-001";

    // Service ports
    static constexpr int TEST_DISCOVERY_PORT = 50491;
    static constexpr int TEST_ECHO_PORT = 50492;
    static constexpr int TEST_DISPATCHER_PORT = 50494;
    static constexpr int TEST_SCHEDULER_PORT = 50493;
    static constexpr int VEHICLE_TRANSPORT_GRPC_PORT = 50490;

    static constexpr const char* TEST_DISCOVERY_ADDRESS = "localhost:50491";
    static constexpr const char* TEST_ECHO_ADDRESS = "localhost:50492";
    static constexpr const char* TEST_DISPATCHER_ADDRESS = "localhost:50494";
    static constexpr const char* TEST_SCHEDULER_ADDRESS = "localhost:50493";

    // Process IDs
    static pid_t discovery_pid_;
    static pid_t echo_pid_;
    static pid_t dispatcher_pid_;
    static pid_t scheduler_pid_;

    // Cloud side
    static std::unique_ptr<CloudBackendTransportServer> cloud_transport_service_;
    static std::unique_ptr<grpc::Server> cloud_transport_grpc_server_;
    static int cloud_transport_grpc_port_;

    static std::unique_ptr<CloudSchedulerService> cloud_scheduler_service_;
    static std::unique_ptr<grpc::Server> cloud_scheduler_grpc_server_;
    static int cloud_scheduler_grpc_port_;

    // Vehicle side
    static std::unique_ptr<ifex::reference::BackendTransportServer> vehicle_transport_service_;
    static std::unique_ptr<grpc::Server> vehicle_transport_grpc_server_;

    static std::unique_ptr<ifex::reference::SchedulerSyncBridge> vehicle_sync_bridge_;

    static void SetUpTestSuite() {
        MqttTestFixture::SetUpTestSuite();

        if (!container_started) {
            return;
        }

        // 1. Start Discovery service
        discovery_pid_ = start_discovery_service(TEST_DISCOVERY_PORT);
        g_discovery_pid.store(discovery_pid_);
        if (!wait_for_service(TEST_DISCOVERY_ADDRESS)) {
            TearDownTestSuite();
            ADD_FAILURE() << "Discovery service failed to start - check /tmp/ifex_cloud_scheduler_test_discovery_" << TEST_DISCOVERY_PORT << ".log";
            return;
        }

        // 2. Start Echo service (test target for scheduled jobs)
        echo_pid_ = start_echo_service(TEST_ECHO_PORT, TEST_DISCOVERY_ADDRESS);
        g_echo_pid.store(echo_pid_);
        if (!wait_for_service(TEST_ECHO_ADDRESS)) {
            TearDownTestSuite();
            ADD_FAILURE() << "Echo service failed to start - check /tmp/ifex_cloud_scheduler_test_echo_" << TEST_ECHO_PORT << ".log";
            return;
        }

        // Give echo service time to register with Discovery
        std::this_thread::sleep_for(200ms);

        // 3. Start Dispatcher service
        dispatcher_pid_ = start_dispatcher_service(TEST_DISPATCHER_PORT, TEST_DISCOVERY_ADDRESS);
        g_dispatcher_pid.store(dispatcher_pid_);
        if (!wait_for_service(TEST_DISPATCHER_ADDRESS)) {
            TearDownTestSuite();
            ADD_FAILURE() << "Dispatcher service failed to start - check /tmp/ifex_cloud_scheduler_test_dispatcher_" << TEST_DISPATCHER_PORT << ".log";
            return;
        }

        // 4. Start Scheduler service
        scheduler_pid_ = start_scheduler_service(TEST_SCHEDULER_PORT, TEST_DISCOVERY_ADDRESS);
        g_scheduler_pid.store(scheduler_pid_);
        if (!wait_for_service(TEST_SCHEDULER_ADDRESS)) {
            TearDownTestSuite();
            ADD_FAILURE() << "Scheduler service failed to start - check /tmp/ifex_cloud_scheduler_test_scheduler_" << TEST_SCHEDULER_PORT << ".log";
            return;
        }

        // 5. Start cloud transport service
        if (!StartCloudTransportService()) {
            TearDownTestSuite();
            ADD_FAILURE() << "Failed to start cloud transport service";
            return;
        }

        // 6. Start vehicle transport service
        if (!StartVehicleTransportService()) {
            TearDownTestSuite();
            ADD_FAILURE() << "Failed to start vehicle transport service";
            return;
        }

        // 7. Start vehicle scheduler sync bridge
        if (!StartVehicleSyncBridge()) {
            TearDownTestSuite();
            ADD_FAILURE() << "Failed to start vehicle sync bridge";
            return;
        }

        // 8. Start cloud scheduler service
        if (!StartCloudSchedulerService()) {
            TearDownTestSuite();
            ADD_FAILURE() << "Failed to start cloud scheduler service";
            return;
        }

        // Wait for all to connect and settle
        std::this_thread::sleep_for(2s);
        LOG(INFO) << "=== All services started and connected ===";
    }

    static void TearDownTestSuite() {
        // Stop in reverse order
        StopCloudSchedulerService();
        StopVehicleSyncBridge();
        StopVehicleTransportService();
        StopCloudTransportService();

        stop_service(scheduler_pid_, "scheduler");
        stop_service(dispatcher_pid_, "dispatcher");
        stop_service(echo_pid_, "echo");
        stop_service(discovery_pid_, "discovery");

        MqttTestFixture::TearDownTestSuite();
    }

    void SetUp() override {
        MqttTestFixture::SetUp();
        if (!cloud_scheduler_service_ || !vehicle_sync_bridge_) {
            FAIL() << "Services not running - SetUpTestSuite failed";
        }
    }

    // Client factories
    std::unique_ptr<::ifex::cloud::scheduler::CloudSchedulerService::Stub>
    createCloudSchedulerStub() {
        auto channel = grpc::CreateChannel(
            "localhost:" + std::to_string(cloud_scheduler_grpc_port_),
            grpc::InsecureChannelCredentials());
        return ::ifex::cloud::scheduler::CloudSchedulerService::NewStub(channel);
    }

private:
    static std::string get_build_dir() {
        // First check for build directory relative to source
        fs::path source_dir = fs::path(__FILE__).parent_path().parent_path().parent_path().parent_path();
        fs::path build_dir = source_dir / "build";

        if (fs::exists(build_dir / "reference-services" / "discovery" / "ifex-discovery-service")) {
            LOG(INFO) << "Found build directory: " << build_dir.string();
            return build_dir.string();
        }

        // Try current working directory / build
        fs::path cwd_build = fs::current_path() / "build";
        if (fs::exists(cwd_build / "reference-services" / "discovery" / "ifex-discovery-service")) {
            LOG(INFO) << "Found build directory from cwd: " << cwd_build.string();
            return cwd_build.string();
        }

        // Try parent directories
        fs::path current = fs::current_path();
        while (!current.empty() && current != current.root_path()) {
            fs::path try_build = current / "build";
            if (fs::exists(try_build / "reference-services" / "discovery" / "ifex-discovery-service")) {
                LOG(INFO) << "Found build directory: " << try_build.string();
                return try_build.string();
            }
            current = current.parent_path();
        }

        LOG(WARNING) << "Could not find build directory, returning '.'";
        return ".";
    }

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

            std::string log_file = "/tmp/ifex_cloud_scheduler_test_discovery_" + port_str + ".log";
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

    static pid_t start_echo_service(int port, const char* discovery_addr) {
        std::string build_dir = get_build_dir();
        std::string executable = build_dir + "/test-services/ifex-echo-service";

        if (!fs::exists(executable)) {
            LOG(ERROR) << "Echo executable not found: " << executable;
            return 0;
        }

        // Set IFEX schema directory for the child process
        std::string ifex_schema_dir = build_dir + "/test-services/ifex";
        if (!fs::exists(ifex_schema_dir + "/echo_service.ifex.yml")) {
            LOG(ERROR) << "IFEX schema not found: " << ifex_schema_dir << "/echo_service.ifex.yml";
            return 0;
        }

        pid_t pid = fork();

        if (pid == 0) {
            std::string port_str = std::to_string(port);
            std::string listen_addr = "0.0.0.0:" + port_str;

            setenv("GLOG_logtostderr", "1", 1);
            setenv("IFEX_SCHEMA_DIR", ifex_schema_dir.c_str(), 1);

            std::string log_file = "/tmp/ifex_cloud_scheduler_test_echo_" + port_str + ".log";
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

        LOG(INFO) << "Started echo service with PID " << pid << " with IFEX_SCHEMA_DIR=" << ifex_schema_dir;
        return pid;
    }

    static pid_t start_dispatcher_service(int port, const char* discovery_addr) {
        std::string build_dir = get_build_dir();
        std::string executable = build_dir + "/reference-services/dispatcher/ifex-dispatcher-service";

        if (!fs::exists(executable)) {
            LOG(ERROR) << "Dispatcher executable not found: " << executable;
            return 0;
        }

        // Set IFEX schema directory for the child process
        std::string ifex_schema_dir = build_dir + "/reference-services/ifex";
        if (!fs::exists(ifex_schema_dir + "/ifex-dispatcher-service.yml")) {
            LOG(ERROR) << "IFEX schema not found: " << ifex_schema_dir << "/ifex-dispatcher-service.yml";
            return 0;
        }

        pid_t pid = fork();

        if (pid == 0) {
            std::string port_str = std::to_string(port);
            std::string listen_addr = "0.0.0.0:" + port_str;

            setenv("GLOG_logtostderr", "1", 1);
            setenv("IFEX_SCHEMA_DIR", ifex_schema_dir.c_str(), 1);

            std::string log_file = "/tmp/ifex_cloud_scheduler_test_dispatcher_" + port_str + ".log";
            freopen(log_file.c_str(), "w", stdout);
            freopen(log_file.c_str(), "w", stderr);

            std::string listen_param = "--listen=" + listen_addr;
            std::string discovery_param = std::string("--discovery=") + discovery_addr;
            execl(executable.c_str(), executable.c_str(),
                  listen_param.c_str(), discovery_param.c_str(), nullptr);

            LOG(ERROR) << "Failed to exec " << executable << ": " << strerror(errno);
            _exit(1);
        } else if (pid < 0) {
            LOG(ERROR) << "Failed to fork for dispatcher service";
            return 0;
        }

        LOG(INFO) << "Started dispatcher service with PID " << pid << " with IFEX_SCHEMA_DIR=" << ifex_schema_dir;
        return pid;
    }

    static pid_t start_scheduler_service(int port, const char* discovery_addr) {
        std::string build_dir = get_build_dir();
        std::string executable = build_dir + "/reference-services/scheduler/ifex-scheduler-service";

        if (!fs::exists(executable)) {
            LOG(ERROR) << "Scheduler executable not found: " << executable;
            return 0;
        }

        // Set IFEX schema directory for the child process
        std::string ifex_schema_dir = build_dir + "/reference-services/ifex";
        if (!fs::exists(ifex_schema_dir + "/ifex-scheduler-service.yml")) {
            LOG(ERROR) << "IFEX schema not found: " << ifex_schema_dir << "/ifex-scheduler-service.yml";
            return 0;
        }

        pid_t pid = fork();

        if (pid == 0) {
            std::string port_str = std::to_string(port);
            std::string listen_addr = "0.0.0.0:" + port_str;

            setenv("GLOG_logtostderr", "1", 1);
            setenv("IFEX_SCHEMA_DIR", ifex_schema_dir.c_str(), 1);

            std::string log_file = "/tmp/ifex_cloud_scheduler_test_scheduler_" + port_str + ".log";
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

        LOG(INFO) << "Started scheduler service with PID " << pid << " with IFEX_SCHEMA_DIR=" << ifex_schema_dir;
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

    static bool StartCloudTransportService() {
        LOG(INFO) << "Starting cloud backend transport service...";

        CloudBackendTransportServer::Config config;
        config.mqtt_host = mqtt_host;
        config.mqtt_port = mqtt_port;
        config.content_id = SCHEDULER_CONTENT_ID;
        config.partition_id = 0;
        config.total_partitions = 1;

        cloud_transport_service_ = std::make_unique<CloudBackendTransportServer>(config);

        if (!cloud_transport_service_->Start()) {
            LOG(ERROR) << "Failed to start cloud transport";
            return false;
        }

        grpc::ServerBuilder builder;
        builder.AddListeningPort("0.0.0.0:0", grpc::InsecureServerCredentials(), &cloud_transport_grpc_port_);

        using namespace swdv::cloud_backend_transport_service;
        builder.RegisterService(static_cast<send_to_vehicle_service::Service*>(cloud_transport_service_.get()));
        builder.RegisterService(static_cast<get_vehicle_status_service::Service*>(cloud_transport_service_.get()));
        builder.RegisterService(static_cast<get_channel_info_service::Service*>(cloud_transport_service_.get()));
        builder.RegisterService(static_cast<get_queue_status_service::Service*>(cloud_transport_service_.get()));
        builder.RegisterService(static_cast<get_stats_service::Service*>(cloud_transport_service_.get()));
        builder.RegisterService(static_cast<healthy_service::Service*>(cloud_transport_service_.get()));
        builder.RegisterService(static_cast<on_vehicle_message_service::Service*>(cloud_transport_service_.get()));
        builder.RegisterService(static_cast<on_ack_service::Service*>(cloud_transport_service_.get()));
        builder.RegisterService(static_cast<on_vehicle_status_service::Service*>(cloud_transport_service_.get()));
        builder.RegisterService(static_cast<on_queue_status_changed_service::Service*>(cloud_transport_service_.get()));

        cloud_transport_grpc_server_ = builder.BuildAndStart();
        LOG(INFO) << "Cloud transport service listening on port " << cloud_transport_grpc_port_;
        return true;
    }

    static bool StartVehicleTransportService() {
        LOG(INFO) << "Starting vehicle backend transport service...";

        ifex::reference::BackendTransportServer::Config config;
        config.mqtt_host = mqtt_host;
        config.mqtt_port = mqtt_port;
        config.vehicle_id = TEST_VEHICLE_ID;
        config.persistence_dir = "/tmp/ifex-cloud-scheduler-test-vehicle";

        vehicle_transport_service_ = std::make_unique<ifex::reference::BackendTransportServer>(config);

        if (!vehicle_transport_service_->Start()) {
            LOG(ERROR) << "Failed to start vehicle transport";
            return false;
        }

        grpc::ServerBuilder builder;
        int grpc_port = VEHICLE_TRANSPORT_GRPC_PORT;
        builder.AddListeningPort("0.0.0.0:" + std::to_string(grpc_port),
                                  grpc::InsecureServerCredentials());

        using namespace swdv::backend_transport_service;
        builder.RegisterService(static_cast<publish_service::Service*>(vehicle_transport_service_.get()));
        builder.RegisterService(static_cast<get_connection_status_service::Service*>(vehicle_transport_service_.get()));
        builder.RegisterService(static_cast<get_queue_status_service::Service*>(vehicle_transport_service_.get()));
        builder.RegisterService(static_cast<get_stats_service::Service*>(vehicle_transport_service_.get()));
        builder.RegisterService(static_cast<healthy_service::Service*>(vehicle_transport_service_.get()));
        builder.RegisterService(static_cast<get_content_id_service::Service*>(vehicle_transport_service_.get()));
        builder.RegisterService(static_cast<on_content_service::Service*>(vehicle_transport_service_.get()));
        builder.RegisterService(static_cast<on_ack_service::Service*>(vehicle_transport_service_.get()));
        builder.RegisterService(static_cast<on_connection_changed_service::Service*>(vehicle_transport_service_.get()));
        builder.RegisterService(static_cast<on_queue_status_changed_service::Service*>(vehicle_transport_service_.get()));

        vehicle_transport_grpc_server_ = builder.BuildAndStart();
        LOG(INFO) << "Vehicle transport service listening on port " << grpc_port;
        return true;
    }

    static bool StartVehicleSyncBridge() {
        LOG(INFO) << "Starting vehicle scheduler sync bridge...";

        ifex::reference::SchedulerSyncBridgeConfig bridge_config;
        bridge_config.scheduler_endpoint = TEST_SCHEDULER_ADDRESS;
        bridge_config.backend_transport_endpoint = "localhost:" + std::to_string(VEHICLE_TRANSPORT_GRPC_PORT);
        bridge_config.poll_interval_ms = 500;
        bridge_config.sync_content_id = SCHEDULER_CONTENT_ID;
        bridge_config.vehicle_id = TEST_VEHICLE_ID;
        bridge_config.initialization_delay_ms = 500;
        bridge_config.enable_cloud_commands = true;
        bridge_config.batch_window_ms = 0;  // Immediate send
        bridge_config.heartbeat_interval_ms = 0;  // Disable heartbeat

        vehicle_sync_bridge_ = std::make_unique<ifex::reference::SchedulerSyncBridge>(bridge_config);

        if (!vehicle_sync_bridge_->Start()) {
            LOG(ERROR) << "Failed to start sync bridge";
            return false;
        }

        LOG(INFO) << "Vehicle sync bridge started";
        return true;
    }

    static bool StartCloudSchedulerService() {
        LOG(INFO) << "Starting cloud scheduler service...";

        CloudSchedulerServiceConfig config;
        config.backend_transport_address = "localhost:" + std::to_string(cloud_transport_grpc_port_);
        config.scheduler_content_id = SCHEDULER_CONTENT_ID;

        cloud_scheduler_service_ = std::make_unique<CloudSchedulerService>(config);

        if (!cloud_scheduler_service_->Start()) {
            LOG(ERROR) << "Failed to start cloud scheduler";
            return false;
        }

        grpc::ServerBuilder builder;
        builder.AddListeningPort("0.0.0.0:0", grpc::InsecureServerCredentials(), &cloud_scheduler_grpc_port_);
        builder.RegisterService(cloud_scheduler_service_.get());

        cloud_scheduler_grpc_server_ = builder.BuildAndStart();
        LOG(INFO) << "Cloud scheduler service listening on port " << cloud_scheduler_grpc_port_;
        return true;
    }

    static void StopCloudSchedulerService() {
        if (cloud_scheduler_grpc_server_) {
            cloud_scheduler_grpc_server_->Shutdown();
            cloud_scheduler_grpc_server_.reset();
        }
        if (cloud_scheduler_service_) {
            cloud_scheduler_service_->Stop();
            cloud_scheduler_service_.reset();
        }
    }

    static void StopVehicleSyncBridge() {
        if (vehicle_sync_bridge_) {
            vehicle_sync_bridge_->Stop();
            vehicle_sync_bridge_.reset();
        }
    }

    static void StopVehicleTransportService() {
        if (vehicle_transport_grpc_server_) {
            vehicle_transport_grpc_server_->Shutdown();
            vehicle_transport_grpc_server_.reset();
        }
        if (vehicle_transport_service_) {
            vehicle_transport_service_->Stop();
            vehicle_transport_service_.reset();
        }
    }

    static void StopCloudTransportService() {
        if (cloud_transport_grpc_server_) {
            cloud_transport_grpc_server_->Shutdown();
            cloud_transport_grpc_server_.reset();
        }
        if (cloud_transport_service_) {
            cloud_transport_service_->Stop();
            cloud_transport_service_.reset();
        }
    }
};

// Static member definitions
pid_t SchedulerBidirectionalSyncTest::discovery_pid_ = 0;
pid_t SchedulerBidirectionalSyncTest::echo_pid_ = 0;
pid_t SchedulerBidirectionalSyncTest::dispatcher_pid_ = 0;
pid_t SchedulerBidirectionalSyncTest::scheduler_pid_ = 0;

std::unique_ptr<CloudBackendTransportServer> SchedulerBidirectionalSyncTest::cloud_transport_service_;
std::unique_ptr<grpc::Server> SchedulerBidirectionalSyncTest::cloud_transport_grpc_server_;
int SchedulerBidirectionalSyncTest::cloud_transport_grpc_port_ = 0;

std::unique_ptr<CloudSchedulerService> SchedulerBidirectionalSyncTest::cloud_scheduler_service_;
std::unique_ptr<grpc::Server> SchedulerBidirectionalSyncTest::cloud_scheduler_grpc_server_;
int SchedulerBidirectionalSyncTest::cloud_scheduler_grpc_port_ = 0;

std::unique_ptr<ifex::reference::BackendTransportServer> SchedulerBidirectionalSyncTest::vehicle_transport_service_;
std::unique_ptr<grpc::Server> SchedulerBidirectionalSyncTest::vehicle_transport_grpc_server_;

std::unique_ptr<ifex::reference::SchedulerSyncBridge> SchedulerBidirectionalSyncTest::vehicle_sync_bridge_;

// =============================================================================
// Cloud-to-Vehicle Command Tests
// =============================================================================

TEST_F(SchedulerBidirectionalSyncTest, CreateJobSendsCommandToVehicle) {
    auto stub = createCloudSchedulerStub();

    // Record initial job count (may have jobs synced from vehicle on startup)
    size_t initial_count = cloud_scheduler_service_->GetJobCount(TEST_VEHICLE_ID);
    LOG(INFO) << "Initial job count: " << initial_count;

    ::ifex::cloud::scheduler::CreateJobRequest request;
    request.set_vehicle_id(TEST_VEHICLE_ID);
    request.set_title("Integration Test Job");
    request.set_service("echo_service");
    request.set_method("echo");
    request.set_parameters_json(R"({"message": "Hello from scheduled job"})");
    request.set_scheduled_time("2099-01-15T10:30:00Z");  // Far future to keep PENDING
    request.set_recurrence_rule("FREQ=DAILY;BYHOUR=10;BYMINUTE=30");

    ::ifex::cloud::scheduler::CreateJobResponse response;
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + 10s);

    auto status = stub->CreateJob(&context, request, &response);

    ASSERT_TRUE(status.ok()) << "gRPC call failed: " << status.error_message();
    EXPECT_TRUE(response.success()) << "CreateJob failed: " << response.error_message();
    EXPECT_FALSE(response.job_id().empty()) << "job_id should be set";

    LOG(INFO) << "Created job: " << response.job_id();

    // Verify job count increased by 1
    size_t new_count = cloud_scheduler_service_->GetJobCount(TEST_VEHICLE_ID);
    EXPECT_EQ(new_count, initial_count + 1) << "Job count should increase by 1";

    // Wait for command to propagate to vehicle and sync back
    std::this_thread::sleep_for(3s);

    // Check sync bridge received and processed the command SUCCESSFULLY
    auto stats = vehicle_sync_bridge_->GetStats();
    LOG(INFO) << "Sync bridge stats: commands_received=" << stats.commands_received
              << " commands_succeeded=" << stats.commands_succeeded
              << " commands_failed=" << stats.commands_failed;

    EXPECT_GE(stats.commands_received, 1u) << "Vehicle should have received at least 1 command";
    EXPECT_EQ(stats.commands_succeeded, stats.commands_received)
        << "All commands should succeed - check if echo_service is registered with Discovery";
    EXPECT_EQ(stats.commands_failed, 0u) << "No commands should fail";
}

TEST_F(SchedulerBidirectionalSyncTest, EpochMillisecondsFlowCorrectly) {
    auto stub = createCloudSchedulerStub();

    // Record initial stats
    auto initial_stats = vehicle_sync_bridge_->GetStats();

    // Create a job with specific timestamp
    ::ifex::cloud::scheduler::CreateJobRequest request;
    request.set_vehicle_id(TEST_VEHICLE_ID);
    request.set_title("Epoch Ms Test Job");
    request.set_service("echo_service");
    request.set_method("echo");
    request.set_parameters_json(R"({"message": "Epoch test"})");
    request.set_scheduled_time("2099-01-15T10:30:00Z");
    request.set_end_time("2099-12-31T23:59:59Z");

    ::ifex::cloud::scheduler::CreateJobResponse response;
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + 10s);

    auto status = stub->CreateJob(&context, request, &response);

    ASSERT_TRUE(status.ok());
    EXPECT_TRUE(response.success());

    // Wait for sync
    std::this_thread::sleep_for(3s);

    // Verify command was processed successfully on vehicle
    auto final_stats = vehicle_sync_bridge_->GetStats();
    EXPECT_GT(final_stats.commands_succeeded, initial_stats.commands_succeeded)
        << "Command should have succeeded on vehicle";
    EXPECT_EQ(final_stats.commands_failed, initial_stats.commands_failed)
        << "No new command failures expected";

    // Query the job back
    ::ifex::cloud::scheduler::GetJobRequest get_request;
    get_request.set_vehicle_id(TEST_VEHICLE_ID);
    get_request.set_job_id(response.job_id());

    ::ifex::cloud::scheduler::GetJobResponse get_response;
    grpc::ClientContext get_context;
    get_context.set_deadline(std::chrono::system_clock::now() + 10s);

    auto get_status = stub->GetJob(&get_context, get_request, &get_response);

    ASSERT_TRUE(get_status.ok());
    EXPECT_TRUE(get_response.found());
    EXPECT_EQ(get_response.job().title(), "Epoch Ms Test Job");
    EXPECT_EQ(get_response.job().service(), "echo_service");
}

TEST_F(SchedulerBidirectionalSyncTest, ListJobsReturnsCreatedJobs) {
    auto stub = createCloudSchedulerStub();

    size_t initial_count = cloud_scheduler_service_->GetJobCount(TEST_VEHICLE_ID);

    // Create multiple jobs
    for (int i = 0; i < 3; i++) {
        ::ifex::cloud::scheduler::CreateJobRequest request;
        request.set_vehicle_id(TEST_VEHICLE_ID);
        request.set_title("List Test Job " + std::to_string(i));
        request.set_service("echo_service");
        request.set_method("echo");
        request.set_parameters_json(R"({"message": "List test"})");
        request.set_scheduled_time("2099-01-20T00:00:00Z");

        ::ifex::cloud::scheduler::CreateJobResponse response;
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + 10s);

        stub->CreateJob(&context, request, &response);
        EXPECT_TRUE(response.success());
    }

    // List jobs
    ::ifex::cloud::scheduler::ListJobsRequest list_request;
    list_request.set_vehicle_id_filter(TEST_VEHICLE_ID);
    list_request.set_service_filter("echo_service");

    ::ifex::cloud::scheduler::ListJobsResponse list_response;
    grpc::ClientContext list_context;
    list_context.set_deadline(std::chrono::system_clock::now() + 10s);

    auto status = stub->ListJobs(&list_context, list_request, &list_response);

    ASSERT_TRUE(status.ok());
    EXPECT_GE(list_response.jobs_size(), 3);
    LOG(INFO) << "Listed " << list_response.jobs_size() << " jobs";
}

TEST_F(SchedulerBidirectionalSyncTest, DeleteJobRemovesFromCloud) {
    auto stub = createCloudSchedulerStub();

    // Create a job first
    ::ifex::cloud::scheduler::CreateJobRequest create_request;
    create_request.set_vehicle_id(TEST_VEHICLE_ID);
    create_request.set_title("Job To Delete");
    create_request.set_service("echo_service");
    create_request.set_method("echo");
    create_request.set_parameters_json(R"({"message": "Delete test"})");
    create_request.set_scheduled_time("2099-02-01T12:00:00Z");

    ::ifex::cloud::scheduler::CreateJobResponse create_response;
    grpc::ClientContext create_context;
    create_context.set_deadline(std::chrono::system_clock::now() + 10s);

    stub->CreateJob(&create_context, create_request, &create_response);
    ASSERT_TRUE(create_response.success());
    std::string job_id = create_response.job_id();

    size_t count_before = cloud_scheduler_service_->GetJobCount(TEST_VEHICLE_ID);

    // Delete the job
    ::ifex::cloud::scheduler::DeleteJobRequest delete_request;
    delete_request.set_vehicle_id(TEST_VEHICLE_ID);
    delete_request.set_job_id(job_id);

    ::ifex::cloud::scheduler::DeleteJobResponse delete_response;
    grpc::ClientContext delete_context;
    delete_context.set_deadline(std::chrono::system_clock::now() + 10s);

    auto status = stub->DeleteJob(&delete_context, delete_request, &delete_response);

    ASSERT_TRUE(status.ok());
    EXPECT_TRUE(delete_response.success());

    size_t count_after = cloud_scheduler_service_->GetJobCount(TEST_VEHICLE_ID);
    EXPECT_EQ(count_after, count_before - 1);
}

TEST_F(SchedulerBidirectionalSyncTest, ValidationRejectsEmptyVehicleId) {
    auto stub = createCloudSchedulerStub();

    ::ifex::cloud::scheduler::CreateJobRequest request;
    request.set_title("Invalid Job");
    request.set_service("test");
    request.set_method("test");

    ::ifex::cloud::scheduler::CreateJobResponse response;
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + 10s);

    auto status = stub->CreateJob(&context, request, &response);

    ASSERT_TRUE(status.ok());
    EXPECT_FALSE(response.success());
    EXPECT_FALSE(response.error_message().empty());
    LOG(INFO) << "Validation error (expected): " << response.error_message();
}

TEST_F(SchedulerBidirectionalSyncTest, ValidationRejectsEmptyService) {
    auto stub = createCloudSchedulerStub();

    ::ifex::cloud::scheduler::CreateJobRequest request;
    request.set_vehicle_id(TEST_VEHICLE_ID);
    request.set_title("Invalid Job");

    ::ifex::cloud::scheduler::CreateJobResponse response;
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + 10s);

    auto status = stub->CreateJob(&context, request, &response);

    ASSERT_TRUE(status.ok());
    EXPECT_FALSE(response.success());
    EXPECT_FALSE(response.error_message().empty());
}

}  // namespace ifex::cloud::test

int main(int argc, char** argv) {
    google::InitGoogleLogging(argv[0]);
    FLAGS_logtostderr = true;

    // Install signal handlers
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    ::testing::InitGoogleTest(&argc, argv);
    mosquitto_lib_init();
    int result = RUN_ALL_TESTS();
    mosquitto_lib_cleanup();

    cleanup_processes();

    return result;
}
