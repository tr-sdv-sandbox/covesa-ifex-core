#pragma once

#include <gtest/gtest.h>
#include <memory>
#include <grpcpp/grpcpp.h>
#include <thread>
#include <chrono>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>

class IntegrationTestFixture : public ::testing::Test {
protected:
    static constexpr int TEST_DISCOVERY_PORT = 50099;
    static constexpr int TEST_DISPATCHER_PORT = 50098;
    static constexpr int TEST_ECHO_PORT = 50097;
    static constexpr int TEST_SETTINGS_PORT = 50096;
    static constexpr int TEST_TYPES_PORT = 50095;
    static constexpr int TEST_SCHEDULER_PORT = 50094;

    static constexpr const char* TEST_DISCOVERY_ADDRESS = "localhost:50099";
    static constexpr const char* TEST_DISPATCHER_ADDRESS = "localhost:50098";
    static constexpr const char* TEST_ECHO_ADDRESS = "localhost:50097";
    static constexpr const char* TEST_SETTINGS_ADDRESS = "localhost:50096";
    static constexpr const char* TEST_TYPES_ADDRESS = "localhost:50095";
    static constexpr const char* TEST_SCHEDULER_ADDRESS = "localhost:50094";

    // Process IDs for services
    static pid_t discovery_pid_;
    static pid_t dispatcher_pid_;
    static pid_t echo_pid_;
    static pid_t settings_pid_;
    static pid_t test_types_pid_;
    static pid_t scheduler_pid_;

    // gRPC channels
    std::shared_ptr<grpc::Channel> discovery_channel_;
    std::shared_ptr<grpc::Channel> dispatcher_channel_;
    std::shared_ptr<grpc::Channel> scheduler_channel_;

    void SetUp() override;
    void TearDown() override;

public:
    // Called by global environment - start/stop services once for all tests
    static void GlobalSetUp();
    static void GlobalTearDown();

    // Public cleanup for atexit handler
    static void cleanup_all_services();

    // Track if services are already running
    static bool services_started_;

private:
    static pid_t start_service(const std::string& executable, const std::string& name, int port);
    static void stop_service(pid_t& pid, const std::string& name);
    static bool wait_for_service(const std::string& address, int timeout_seconds = 5);

    static std::string get_build_dir();
    static std::string get_schema_dir();
};
