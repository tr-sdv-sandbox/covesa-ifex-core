#include "test_fixture.hpp"
#include <glog/logging.h>
#include <fstream>
#include <filesystem>
#include <cstdlib>
#include <atomic>

namespace fs = std::filesystem;

// Static member definitions
pid_t IntegrationTestFixture::discovery_pid_ = 0;
pid_t IntegrationTestFixture::dispatcher_pid_ = 0;
pid_t IntegrationTestFixture::echo_pid_ = 0;
pid_t IntegrationTestFixture::settings_pid_ = 0;
pid_t IntegrationTestFixture::test_types_pid_ = 0;
pid_t IntegrationTestFixture::scheduler_pid_ = 0;
bool IntegrationTestFixture::services_started_ = false;

// Cleanup function called on exit (ensures services are stopped even on crash/abort)
static void cleanup_services_atexit() {
    IntegrationTestFixture::cleanup_all_services();
}

void IntegrationTestFixture::GlobalSetUp() {
    // Skip if already started (global environment calls this once)
    if (services_started_) {
        return;
    }

    // Register atexit handler to ensure cleanup on any exit
    static bool registered = false;
    if (!registered) {
        std::atexit(cleanup_services_atexit);
        registered = true;
    }

    // Start discovery service first (others depend on it)
    discovery_pid_ = start_service(
        get_build_dir() + "/reference-services/discovery/ifex-discovery-service",
        "discovery",
        TEST_DISCOVERY_PORT
    );

    if (!wait_for_service(TEST_DISCOVERY_ADDRESS)) {
        TearDownTestSuite();
        FAIL() << "Discovery service failed to start";
    }

    // Start all other services in parallel (they all depend only on discovery)
    dispatcher_pid_ = start_service(
        get_build_dir() + "/reference-services/dispatcher/ifex-dispatcher-service",
        "dispatcher",
        TEST_DISPATCHER_PORT
    );

    scheduler_pid_ = start_service(
        get_build_dir() + "/reference-services/scheduler/ifex-scheduler-service",
        "scheduler",
        TEST_SCHEDULER_PORT
    );

    echo_pid_ = start_service(
        get_build_dir() + "/test-services/ifex-echo-service",
        "echo",
        TEST_ECHO_PORT
    );

    settings_pid_ = start_service(
        get_build_dir() + "/test-services/settings/ifex-settings-service",
        "settings",
        TEST_SETTINGS_PORT
    );

    test_types_pid_ = start_service(
        get_build_dir() + "/test-services/test-types/ifex-test-types-service",
        "test-types",
        TEST_TYPES_PORT
    );

    // Wait for all services in parallel using threads
    std::atomic<bool> dispatcher_ready{false};
    std::atomic<bool> scheduler_ready{false};
    std::atomic<bool> echo_ready{false};
    std::atomic<bool> settings_ready{false};
    std::atomic<bool> test_types_ready{false};

    std::thread t1([&]() { dispatcher_ready = wait_for_service(TEST_DISPATCHER_ADDRESS); });
    std::thread t2([&]() { scheduler_ready = wait_for_service(TEST_SCHEDULER_ADDRESS); });
    std::thread t3([&]() { echo_ready = wait_for_service(TEST_ECHO_ADDRESS); });
    std::thread t4([&]() { settings_ready = wait_for_service(TEST_SETTINGS_ADDRESS); });
    std::thread t5([&]() { test_types_ready = wait_for_service(TEST_TYPES_ADDRESS); });

    t1.join();
    t2.join();
    t3.join();
    t4.join();
    t5.join();

    if (!dispatcher_ready) LOG(ERROR) << "Dispatcher service failed to start";
    if (!scheduler_ready) LOG(ERROR) << "Scheduler service failed to start";
    if (!echo_ready) LOG(ERROR) << "Echo service failed to start";
    if (!settings_ready) LOG(ERROR) << "Settings service failed to start";
    if (!test_types_ready) LOG(ERROR) << "Test types service failed to start";

    if (!dispatcher_ready || !scheduler_ready || !echo_ready || !settings_ready || !test_types_ready) {
        TearDownTestSuite();
        FAIL() << "One or more services failed to start";
    }

    services_started_ = true;
}

void IntegrationTestFixture::GlobalTearDown() {
    // Only cleanup if we started the services
    if (!services_started_) {
        return;
    }
    cleanup_all_services();
    services_started_ = false;
}

void IntegrationTestFixture::cleanup_all_services() {
    // Only log if any services are running
    if (test_types_pid_ || settings_pid_ || echo_pid_ ||
        scheduler_pid_ || dispatcher_pid_ || discovery_pid_) {
        LOG(INFO) << "Stopping test services...";
    }

    stop_service(test_types_pid_, "test-types");
    stop_service(settings_pid_, "settings");
    stop_service(echo_pid_, "echo");
    stop_service(scheduler_pid_, "scheduler");
    stop_service(dispatcher_pid_, "dispatcher");
    stop_service(discovery_pid_, "discovery");

    if (test_types_pid_ == 0 && settings_pid_ == 0 && echo_pid_ == 0 &&
        scheduler_pid_ == 0 && dispatcher_pid_ == 0 && discovery_pid_ == 0) {
        LOG(INFO) << "All test services stopped";
    }
}

void IntegrationTestFixture::SetUp() {
    // Create channels for each test
    discovery_channel_ = grpc::CreateChannel(TEST_DISCOVERY_ADDRESS, grpc::InsecureChannelCredentials());
    dispatcher_channel_ = grpc::CreateChannel(TEST_DISPATCHER_ADDRESS, grpc::InsecureChannelCredentials());
    scheduler_channel_ = grpc::CreateChannel(TEST_SCHEDULER_ADDRESS, grpc::InsecureChannelCredentials());
}

void IntegrationTestFixture::TearDown() {
    // Channels will be cleaned up automatically
}

pid_t IntegrationTestFixture::start_service(const std::string& executable, const std::string& name, int port) {
    if (!fs::exists(executable)) {
        LOG(ERROR) << "Service executable not found: " << executable;
        return 0;
    }

    pid_t pid = fork();

    if (pid == 0) {
        // Child process
        std::string port_str = std::to_string(port);
        std::string listen_addr = "0.0.0.0:" + port_str;

        // Set environment variables
        setenv("IFEX_SCHEMA_DIR", (get_build_dir() + "/ifex").c_str(), 1);
        setenv("GLOG_logtostderr", "1", 1);

        // Redirect output to log files
        std::string log_file = "/tmp/ifex_test_" + name + "_" + port_str + ".log";
        freopen(log_file.c_str(), "w", stdout);
        freopen(log_file.c_str(), "w", stderr);

        // Execute the service
        if (name == "discovery") {
            std::string listen_param = "--listen=" + listen_addr;
            execl(executable.c_str(), executable.c_str(), listen_param.c_str(), nullptr);
        } else if (name == "dispatcher") {
            std::string listen_param = "--listen=" + listen_addr;
            std::string discovery_param = "--discovery=" + std::string(TEST_DISCOVERY_ADDRESS);
            execl(executable.c_str(), executable.c_str(),
                  listen_param.c_str(),
                  discovery_param.c_str(),
                  nullptr);
        } else if (name == "scheduler") {
            std::string port_param = "--port=" + port_str;
            std::string discovery_param = "--discovery=" + std::string(TEST_DISCOVERY_ADDRESS);
            execl(executable.c_str(), executable.c_str(),
                  port_param.c_str(),
                  discovery_param.c_str(),
                  nullptr);
        } else if (name == "echo") {
            std::string listen_param = "--listen=" + listen_addr;
            std::string discovery_param = "--discovery=" + std::string(TEST_DISCOVERY_ADDRESS);
            execl(executable.c_str(), executable.c_str(),
                  listen_param.c_str(),
                  discovery_param.c_str(),
                  nullptr);
        } else if (name == "settings") {
            fs::path settings_dir = fs::path(executable).parent_path();
            if (chdir(settings_dir.c_str()) != 0) {
                LOG(ERROR) << "Failed to change to settings directory: " << settings_dir;
                _exit(1);
            }

            std::string listen_param = "--listen=" + listen_addr;
            std::string discovery_param = "--discovery=" + std::string(TEST_DISCOVERY_ADDRESS);
            execl(executable.c_str(), executable.c_str(),
                  listen_param.c_str(),
                  discovery_param.c_str(),
                  nullptr);
        } else if (name == "test-types") {
            fs::path test_types_dir = fs::path(executable).parent_path();
            if (chdir(test_types_dir.c_str()) != 0) {
                LOG(ERROR) << "Failed to change to test-types directory: " << test_types_dir;
                _exit(1);
            }

            std::string listen_param = "--listen=" + listen_addr;
            std::string discovery_param = "--discovery=" + std::string(TEST_DISCOVERY_ADDRESS);
            execl(executable.c_str(), executable.c_str(),
                  listen_param.c_str(),
                  discovery_param.c_str(),
                  nullptr);
        }

        // If exec fails
        LOG(ERROR) << "Failed to exec " << executable << ": " << strerror(errno);
        _exit(1);
    } else if (pid < 0) {
        LOG(ERROR) << "Failed to fork for " << name << " service";
        return 0;
    }

    LOG(INFO) << "Started " << name << " service with PID " << pid;
    return pid;
}

void IntegrationTestFixture::stop_service(pid_t& pid, const std::string& name) {
    if (pid > 0) {
        LOG(INFO) << "Stopping " << name << " service (PID: " << pid << ")";

        // Send SIGTERM for graceful shutdown
        kill(pid, SIGTERM);

        // Wait for process to terminate with timeout
        int status;
        int wait_count = 0;
        while (waitpid(pid, &status, WNOHANG) == 0 && wait_count < 50) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            wait_count++;
        }

        // Force kill if still running
        if (waitpid(pid, &status, WNOHANG) == 0) {
            LOG(WARNING) << "Force killing " << name << " service";
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
        }

        pid = 0;
    }
}

bool IntegrationTestFixture::wait_for_service(const std::string& address, int timeout_seconds) {
    auto start = std::chrono::steady_clock::now();

    // Create channel with aggressive connection settings
    grpc::ChannelArguments args;
    args.SetInt(GRPC_ARG_INITIAL_RECONNECT_BACKOFF_MS, 100);
    args.SetInt(GRPC_ARG_MIN_RECONNECT_BACKOFF_MS, 100);
    args.SetInt(GRPC_ARG_MAX_RECONNECT_BACKOFF_MS, 500);

    auto channel = grpc::CreateCustomChannel(address, grpc::InsecureChannelCredentials(), args);
    auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(timeout_seconds);

    while (std::chrono::system_clock::now() < deadline) {
        // GetState(true) triggers connection attempt
        auto state = channel->GetState(true);
        if (state == GRPC_CHANNEL_READY) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();
            LOG(INFO) << "Service " << address << " ready in " << elapsed << "ms";
            return true;
        }

        // Wait for state change with short timeout
        channel->WaitForStateChange(state,
            std::chrono::system_clock::now() + std::chrono::milliseconds(100));
    }

    LOG(ERROR) << "Service at " << address << " failed to become ready within timeout";
    return false;
}

std::string IntegrationTestFixture::get_build_dir() {
    // Try to find the build directory relative to the test executable
    fs::path current = fs::current_path();

    // Look for CMakeCache.txt to identify build directory
    while (!current.empty() && current != current.root_path()) {
        if (fs::exists(current / "CMakeCache.txt")) {
            return current.string();
        }
        current = current.parent_path();
    }

    // Fallback to current directory
    return ".";
}
