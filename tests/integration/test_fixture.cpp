#include "test_fixture.hpp"
#include <glog/logging.h>
#include <fstream>
#include <filesystem>
#include <cstdlib>
#include <atomic>
#include <algorithm>
#include <set>
#include <utility>
#include <vector>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <limits.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace fs = std::filesystem;

// Static member definitions - gRPC services
pid_t IntegrationTestFixture::discovery_pid_ = 0;
pid_t IntegrationTestFixture::dispatcher_pid_ = 0;
pid_t IntegrationTestFixture::echo_pid_ = 0;
pid_t IntegrationTestFixture::test_types_pid_ = 0;
pid_t IntegrationTestFixture::scheduler_pid_ = 0;
bool IntegrationTestFixture::services_started_ = false;

int IntegrationTestFixture::discovery_port_ = IntegrationTestFixture::TEST_DISCOVERY_PORT;
int IntegrationTestFixture::dispatcher_port_ = IntegrationTestFixture::TEST_DISPATCHER_PORT;
int IntegrationTestFixture::echo_port_ = IntegrationTestFixture::TEST_ECHO_PORT;
int IntegrationTestFixture::test_types_port_ = IntegrationTestFixture::TEST_TYPES_PORT;
int IntegrationTestFixture::scheduler_port_ = IntegrationTestFixture::TEST_SCHEDULER_PORT;

std::string IntegrationTestFixture::discovery_address_ = IntegrationTestFixture::TEST_DISCOVERY_ADDRESS;
std::string IntegrationTestFixture::dispatcher_address_ = IntegrationTestFixture::TEST_DISPATCHER_ADDRESS;
std::string IntegrationTestFixture::echo_address_ = IntegrationTestFixture::TEST_ECHO_ADDRESS;
std::string IntegrationTestFixture::test_types_address_ = IntegrationTestFixture::TEST_TYPES_ADDRESS;
std::string IntegrationTestFixture::scheduler_address_ = IntegrationTestFixture::TEST_SCHEDULER_ADDRESS;

// Static member definitions - MQTT
std::string IntegrationTestFixture::mqtt_host_;
int IntegrationTestFixture::mqtt_port_ = IntegrationTestFixture::MQTT_DEFAULT_PORT;
bool IntegrationTestFixture::mqtt_started_ = false;

namespace {

std::string make_local_address(int port) {
    return "localhost:" + std::to_string(port);
}

bool is_build_dir(const fs::path& candidate) {
    return fs::exists(candidate / "CMakeCache.txt") &&
           fs::exists(candidate / "reference-services") &&
           fs::exists(candidate / "tests");
}

bool is_repo_root(const fs::path& candidate) {
    return fs::exists(candidate / "CMakeLists.txt") &&
           fs::exists(candidate / "reference-services") &&
           fs::exists(candidate / "tests");
}

fs::path normalize_build_dir_candidate(const fs::path& candidate) {
    fs::path ifex_subdir = candidate / "covesa-ifex-core" / "reference-services";
    if (fs::exists(ifex_subdir)) {
        return candidate / "covesa-ifex-core";
    }
    return candidate;
}

fs::path find_build_dir_from(fs::path start) {
    fs::path current = std::move(start);
    while (!current.empty() && current != current.root_path()) {
        if (is_build_dir(current)) {
            return normalize_build_dir_candidate(current);
        }
        current = current.parent_path();
    }
    return {};
}

fs::path find_repo_root_from(fs::path start) {
    fs::path current = std::move(start);
    while (!current.empty() && current != current.root_path()) {
        if (is_repo_root(current)) {
            return current;
        }
        current = current.parent_path();
    }
    return {};
}

fs::path get_executable_path() {
#ifdef __APPLE__
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(size);
    if (_NSGetExecutablePath(buffer.data(), &size) == 0) {
        return fs::weakly_canonical(fs::path(buffer.data()));
    }
    return {};
#else
    char buffer[PATH_MAX] = {0};
    ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (len <= 0) {
        return {};
    }
    buffer[len] = '\0';
    return fs::weakly_canonical(fs::path(buffer));
#endif
}

bool can_bind_port(int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(port));

    bool ok = bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    close(sock);
    return ok;
}

int choose_port(int preferred_port, const std::set<int>& reserved) {
    constexpr int kFallbackSpan = 128;
    for (int candidate = preferred_port; candidate <= preferred_port + kFallbackSpan; ++candidate) {
        if (reserved.count(candidate) != 0) {
            continue;
        }
        if (can_bind_port(candidate)) {
            return candidate;
        }
    }
    return 0;
}

std::vector<fs::path> build_dir_candidates() {
    std::vector<fs::path> candidates;

    fs::path executable_path = get_executable_path();
    if (!executable_path.empty()) {
        candidates.push_back(find_build_dir_from(executable_path.parent_path()));
        fs::path repo_root = find_repo_root_from(executable_path.parent_path());
        if (!repo_root.empty()) {
            candidates.push_back(repo_root / "build");
        }
    }

    candidates.push_back(find_build_dir_from(fs::current_path()));

    fs::path repo_root = find_repo_root_from(fs::current_path());
    if (!repo_root.empty()) {
        candidates.push_back(repo_root / "build");
    }

    candidates.push_back(fs::current_path());
    candidates.push_back(fs::current_path() / "build");

    std::vector<fs::path> normalized_candidates;
    for (const fs::path& candidate : candidates) {
        if (candidate.empty() || !is_build_dir(candidate)) {
            continue;
        }

        fs::path normalized = fs::weakly_canonical(candidate);
        if (std::find(normalized_candidates.begin(), normalized_candidates.end(), normalized) == normalized_candidates.end()) {
            normalized_candidates.push_back(normalized);
        }
    }

    return normalized_candidates;
}

fs::path resolve_build_dir() {
    std::vector<fs::path> candidates = build_dir_candidates();
    if (!candidates.empty()) {
        return candidates.front();
    }
    return {};
}

fs::path resolve_build_relative_path(const fs::path& relative_path) {
    for (const fs::path& build_dir : build_dir_candidates()) {
        fs::path candidate = build_dir / relative_path;
        if (fs::exists(candidate)) {
            return fs::weakly_canonical(candidate);
        }
    }
    return {};
}

}

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

    // Clean up scheduler persistence directory to avoid stale test data
    // (tombstones from previous test runs would pollute job counts)
    std::filesystem::remove_all("/tmp/ifex-scheduler-test-persist");
    std::filesystem::create_directories("/tmp/ifex-scheduler-test-persist");

    if (!resolve_runtime_addresses()) {
        FAIL() << "Unable to resolve free runtime ports for integration test services";
    }

    // Start MQTT container first (optional - tests can skip if not available)
    if (!StartMqttContainer()) {
        LOG(WARNING) << "MQTT container not available - some tests will be skipped";
    }

    // Start discovery service first (others depend on it)
    discovery_pid_ = start_service(
        "reference-services/discovery/vehicle/service/ifex-discovery-service",
        "discovery",
        GetDiscoveryPort()
    );

    if (!wait_for_service(GetDiscoveryAddress())) {
        TearDownTestSuite();
        FAIL() << "Discovery service failed to start";
    }

    // Start all other services in parallel (they all depend only on discovery)
    dispatcher_pid_ = start_service(
        "reference-services/dispatcher/vehicle/service/ifex-dispatcher-service",
        "dispatcher",
        GetDispatcherPort()
    );

    scheduler_pid_ = start_service(
        "reference-services/scheduler/vehicle/service/ifex-scheduler-service",
        "scheduler",
        GetSchedulerPort()
    );

    echo_pid_ = start_service(
        "test-services/ifex-echo-service",
        "echo",
        GetEchoPort()
    );

    test_types_pid_ = start_service(
        "tests/test-types/ifex-test-types-service",
        "test-types",
        GetTestTypesPort()
    );

    // Wait for all services in parallel using threads
    std::atomic<bool> dispatcher_ready{false};
    std::atomic<bool> scheduler_ready{false};
    std::atomic<bool> echo_ready{false};
    std::atomic<bool> test_types_ready{false};

    std::thread t1([&]() { dispatcher_ready = wait_for_service(GetDispatcherAddress()); });
    std::thread t2([&]() { scheduler_ready = wait_for_service(GetSchedulerAddress()); });
    std::thread t3([&]() { echo_ready = wait_for_service(GetEchoAddress()); });
    std::thread t4([&]() { test_types_ready = wait_for_service(GetTestTypesAddress()); });

    t1.join();
    t2.join();
    t3.join();
    t4.join();

    if (!dispatcher_ready) LOG(ERROR) << "Dispatcher service failed to start";
    if (!scheduler_ready) LOG(ERROR) << "Scheduler service failed to start";
    if (!echo_ready) LOG(ERROR) << "Echo service failed to start";
    if (!test_types_ready) LOG(ERROR) << "Test types service failed to start";

    if (!dispatcher_ready || !scheduler_ready || !echo_ready || !test_types_ready) {
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
    if (test_types_pid_ || echo_pid_ || scheduler_pid_ || dispatcher_pid_ || discovery_pid_ || mqtt_started_) {
        LOG(INFO) << "Stopping test services...";
    }

    stop_service(test_types_pid_, "test-types");
    stop_service(echo_pid_, "echo");
    stop_service(scheduler_pid_, "scheduler");
    stop_service(dispatcher_pid_, "dispatcher");
    stop_service(discovery_pid_, "discovery");
    StopMqttContainer();

    if (test_types_pid_ == 0 && echo_pid_ == 0 && scheduler_pid_ == 0 &&
        dispatcher_pid_ == 0 && discovery_pid_ == 0 && !mqtt_started_) {
        LOG(INFO) << "All test services stopped";
    }
}

void IntegrationTestFixture::SetUp() {
    // Create channels for each test
    discovery_channel_ = grpc::CreateChannel(GetDiscoveryAddress(), grpc::InsecureChannelCredentials());
    dispatcher_channel_ = grpc::CreateChannel(GetDispatcherAddress(), grpc::InsecureChannelCredentials());
    scheduler_channel_ = grpc::CreateChannel(GetSchedulerAddress(), grpc::InsecureChannelCredentials());
}

void IntegrationTestFixture::TearDown() {
    // Channels will be cleaned up automatically
}

pid_t IntegrationTestFixture::start_service(const std::string& executable, const std::string& name, int port) {
    fs::path resolved_executable = resolve_build_relative_path(executable);
    if (resolved_executable.empty()) {
        LOG(ERROR) << "Service executable not found: " << executable
                   << " (cwd=" << fs::current_path().string() << ")";
        return 0;
    }

    std::string resolved_executable_string = resolved_executable.string();

    pid_t pid = fork();

    if (pid == 0) {
        // Child process
        std::string port_str = std::to_string(port);
        std::string listen_addr = "0.0.0.0:" + port_str;

        // Set environment variables
        setenv("GLOG_logtostderr", "1", 1);

        // Redirect output to log files
        std::string log_file = "/tmp/ifex_test_" + name + "_" + port_str + ".log";
        freopen(log_file.c_str(), "w", stdout);
        freopen(log_file.c_str(), "w", stderr);

        // Execute the service
        if (name == "discovery") {
            std::string listen_param = "--listen=" + listen_addr;
            execl(resolved_executable_string.c_str(), resolved_executable_string.c_str(), listen_param.c_str(), nullptr);
        } else if (name == "dispatcher") {
            std::string listen_param = "--listen=" + listen_addr;
            std::string discovery_param = "--discovery=" + GetDiscoveryAddress();
            execl(resolved_executable_string.c_str(), resolved_executable_string.c_str(),
                  listen_param.c_str(),
                  discovery_param.c_str(),
                  nullptr);
        } else if (name == "scheduler") {
            std::string listen_param = "--listen=" + listen_addr;
            std::string discovery_param = "--discovery=" + GetDiscoveryAddress();
            std::string persist_param = "--persistence-dir=/tmp/ifex-scheduler-test-persist";
            execl(resolved_executable_string.c_str(), resolved_executable_string.c_str(),
                  listen_param.c_str(),
                  discovery_param.c_str(),
                  persist_param.c_str(),
                  nullptr);
        } else if (name == "echo") {
            std::string listen_param = "--listen=" + listen_addr;
            std::string discovery_param = "--discovery=" + GetDiscoveryAddress();
            execl(resolved_executable_string.c_str(), resolved_executable_string.c_str(),
                  listen_param.c_str(),
                  discovery_param.c_str(),
                  nullptr);
        } else if (name == "test-types") {
            fs::path test_types_dir = resolved_executable.parent_path();
            if (chdir(test_types_dir.c_str()) != 0) {
                LOG(ERROR) << "Failed to change to test-types directory: " << test_types_dir;
                _exit(1);
            }

            std::string listen_param = "--listen=" + listen_addr;
            std::string discovery_param = "--discovery=" + GetDiscoveryAddress();
            execl(resolved_executable_string.c_str(), resolved_executable_string.c_str(),
                  listen_param.c_str(),
                  discovery_param.c_str(),
                  nullptr);
        }

        // If exec fails
        LOG(ERROR) << "Failed to exec " << resolved_executable_string << ": " << strerror(errno);
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
    fs::path build_dir = resolve_build_dir();
    if (!build_dir.empty()) {
        return build_dir.string();
    }

    return fs::current_path().string();
}

std::string IntegrationTestFixture::get_schema_dir() {
    // IFEX schema files are installed to <top-level-build>/ifex/
    fs::path current = fs::current_path();

    while (!current.empty() && current != current.root_path()) {
        if (fs::exists(current / "CMakeCache.txt")) {
            // Schemas are always at top-level build dir / ifex
            if (fs::exists(current / "ifex")) {
                return (current / "ifex").string();
            }
            // Fallback: try local ifex directory
            std::string build_dir = get_build_dir();
            if (fs::exists(build_dir + "/ifex")) {
                return build_dir + "/ifex";
            }
            return current.string() + "/ifex";
        }
        current = current.parent_path();
    }

    return "./ifex";
}

bool IntegrationTestFixture::resolve_runtime_addresses() {
    std::set<int> reserved;

    discovery_port_ = choose_port(TEST_DISCOVERY_PORT, reserved);
    if (discovery_port_ == 0) {
        return false;
    }
    reserved.insert(discovery_port_);

    dispatcher_port_ = choose_port(TEST_DISPATCHER_PORT, reserved);
    if (dispatcher_port_ == 0) {
        return false;
    }
    reserved.insert(dispatcher_port_);

    scheduler_port_ = choose_port(TEST_SCHEDULER_PORT, reserved);
    if (scheduler_port_ == 0) {
        return false;
    }
    reserved.insert(scheduler_port_);

    echo_port_ = choose_port(TEST_ECHO_PORT, reserved);
    if (echo_port_ == 0) {
        return false;
    }
    reserved.insert(echo_port_);

    test_types_port_ = choose_port(TEST_TYPES_PORT, reserved);
    if (test_types_port_ == 0) {
        return false;
    }

    discovery_address_ = make_local_address(discovery_port_);
    dispatcher_address_ = make_local_address(dispatcher_port_);
    scheduler_address_ = make_local_address(scheduler_port_);
    echo_address_ = make_local_address(echo_port_);
    test_types_address_ = make_local_address(test_types_port_);

    LOG(INFO) << "Resolved integration test addresses: "
              << "discovery=" << discovery_address_ << ", "
              << "dispatcher=" << dispatcher_address_ << ", "
              << "scheduler=" << scheduler_address_ << ", "
              << "echo=" << echo_address_ << ", "
              << "test-types=" << test_types_address_;
    return true;
}

bool IntegrationTestFixture::RestartScheduler() {
    LOG(INFO) << "=== Restarting scheduler service ===";

    // Stop current scheduler
    stop_service(scheduler_pid_, "scheduler");

    // Brief delay to ensure scheduler has fully stopped
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Start scheduler again
    scheduler_pid_ = start_service(
        "reference-services/scheduler/vehicle/service/ifex-scheduler-service",
        "scheduler",
        GetSchedulerPort()
    );

    if (scheduler_pid_ == 0) {
        LOG(ERROR) << "Failed to restart scheduler";
        return false;
    }

    // Wait for scheduler to be ready
    if (!wait_for_service(GetSchedulerAddress())) {
        LOG(ERROR) << "Scheduler failed to become ready after restart";
        return false;
    }

    LOG(INFO) << "Scheduler restarted successfully";
    return true;
}

// =============================================================================
// MQTT Docker Container Management
// =============================================================================

bool IntegrationTestFixture::StartMqttContainer() {
    LOG(INFO) << "=== Setting up MQTT test environment ===";

    // Check if MQTT_HOST environment variable is set (external broker)
    const char* env_host = std::getenv("MQTT_HOST");
    if (env_host) {
        mqtt_host_ = env_host;
        const char* env_port = std::getenv("MQTT_PORT");
        mqtt_port_ = env_port ? std::atoi(env_port) : 1883;
        mqtt_started_ = true;
        LOG(INFO) << "Using MQTT from environment: " << mqtt_host_ << ":" << mqtt_port_;
        return true;
    }

    // Check Docker availability
    if (std::system("docker --version > /dev/null 2>&1") != 0) {
        LOG(WARNING) << "Docker is not available - MQTT tests will be skipped";
        return false;
    }

    // Stop any existing container
    StopMqttContainer();

    // Start MQTT container
    LOG(INFO) << "Starting MQTT broker container...";

    std::string port_str = std::to_string(MQTT_DEFAULT_PORT);
    std::string cmd = "docker run -d --rm "
                      "--name " + std::string(MQTT_CONTAINER_NAME) + " "
                      "-p " + port_str + ":1883 "
                      + std::string(MQTT_IMAGE) + " "
                      "sh -c 'echo -e \"listener 1883\\nallow_anonymous true\" > /tmp/m.conf && "
                      "mosquitto -c /tmp/m.conf'";

    LOG(INFO) << "Docker command: " << cmd;

    if (std::system(cmd.c_str()) != 0) {
        LOG(ERROR) << "Failed to start MQTT Docker container";
        return false;
    }

    // Wait for container to be ready
    LOG(INFO) << "Waiting for MQTT broker to be ready...";
    for (int i = 0; i < 100; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // Check if port is open
        std::string check_port = "nc -z localhost " + port_str + " 2>/dev/null";
        if (std::system(check_port.c_str()) == 0) {
            mqtt_host_ = "localhost";
            mqtt_port_ = MQTT_DEFAULT_PORT;
            mqtt_started_ = true;
            LOG(INFO) << "MQTT broker is ready at " << mqtt_host_ << ":" << mqtt_port_;
            return true;
        }

        // Every 10 iterations, verify container is still running
        if (i % 10 == 9) {
            std::string check_running = "docker ps -q -f name=" + std::string(MQTT_CONTAINER_NAME) + " | grep -q .";
            if (std::system(check_running.c_str()) != 0) {
                LOG(ERROR) << "MQTT container stopped unexpectedly";
                [[maybe_unused]] int log_result = std::system(("docker logs " + std::string(MQTT_CONTAINER_NAME) + " 2>&1 | tail -20").c_str());
                return false;
            }
        }
    }

    LOG(ERROR) << "Timeout waiting for MQTT broker to be ready";
    StopMqttContainer();
    return false;
}

void IntegrationTestFixture::StopMqttContainer() {
    // Only stop container if we started it (not external broker from environment)
    if (!std::getenv("MQTT_HOST")) {
        LOG(INFO) << "Stopping MQTT container...";
        [[maybe_unused]] int stop_result = std::system(("docker stop " + std::string(MQTT_CONTAINER_NAME) + " 2>/dev/null").c_str());
        [[maybe_unused]] int rm_result = std::system(("docker rm -f " + std::string(MQTT_CONTAINER_NAME) + " 2>/dev/null").c_str());
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    mqtt_started_ = false;
}
