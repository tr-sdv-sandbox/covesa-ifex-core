/**
 * @file mqtt_test_fixture.hpp
 * @brief Test fixture that automatically manages MQTT broker Docker container
 *
 * Based on vep-core's MqttTestFixture pattern.
 */

#pragma once

#include <gtest/gtest.h>
#include <glog/logging.h>
#include <cstdlib>
#include <thread>
#include <chrono>

namespace ifex::test {

class MqttTestFixture : public ::testing::Test {
protected:
    static constexpr const char* MQTT_IMAGE = "eclipse-mosquitto:2";
    static constexpr const char* CONTAINER_NAME = "ifex-mqtt-test-broker";
    static constexpr const char* MQTT_PORT = "11883";  // Different port to avoid conflicts
    static std::string mqtt_host;
    static int mqtt_port;
    static bool container_started;

    static void SetUpTestSuite() {
        LOG(INFO) << "=== Setting up MQTT test environment ===";

        // Check if MQTT_HOST environment variable is set (external broker)
        const char* env_host = std::getenv("MQTT_HOST");
        if (env_host) {
            mqtt_host = env_host;
            const char* env_port = std::getenv("MQTT_PORT");
            mqtt_port = env_port ? std::atoi(env_port) : 1883;
            container_started = true;
            LOG(INFO) << "Using MQTT from environment: " << mqtt_host << ":" << mqtt_port;
            return;
        }

        // Check Docker availability
        if (std::system("docker --version > /dev/null 2>&1") != 0) {
            GTEST_SKIP() << "Docker is not available. Skipping MQTT integration tests.";
            return;
        }

        // Stop any existing container
        StopContainer();

        // Start MQTT container
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
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    const std::string& getMqttHost() const { return mqtt_host; }
    int getMqttPort() const { return mqtt_port; }

private:
    static bool StartContainer() {
        LOG(INFO) << "Starting MQTT broker container...";

        // Mosquitto 2.x requires config for anonymous access
        std::string cmd = "docker run -d --rm "
                          "--name " + std::string(CONTAINER_NAME) + " "
                          "-p " + std::string(MQTT_PORT) + ":1883 "
                          + std::string(MQTT_IMAGE) + " "
                          "sh -c 'echo -e \"listener 1883\\nallow_anonymous true\" > /tmp/m.conf && "
                          "mosquitto -c /tmp/m.conf'";

        LOG(INFO) << "Docker command: " << cmd;

        if (std::system(cmd.c_str()) != 0) {
            LOG(ERROR) << "Failed to start Docker container";
            return false;
        }

        // Wait for container to be ready
        LOG(INFO) << "Waiting for MQTT broker to be ready...";
        for (int i = 0; i < 30; ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));

            // Check if container is running
            std::string check_running = "docker ps -q -f name=" + std::string(CONTAINER_NAME) + " | grep -q .";
            if (std::system(check_running.c_str()) != 0) {
                LOG(ERROR) << "Container stopped unexpectedly";
                [[maybe_unused]] int log_result = std::system(("docker logs " + std::string(CONTAINER_NAME) + " 2>&1 | tail -20").c_str());
                return false;
            }

            // Check if port is open
            std::string check_port = "nc -z localhost " + std::string(MQTT_PORT) + " 2>/dev/null";
            if (std::system(check_port.c_str()) == 0) {
                LOG(INFO) << "MQTT broker is ready!";
                container_started = true;
                return true;
            }
        }

        LOG(ERROR) << "Timeout waiting for MQTT broker to be ready";
        StopContainer();
        return false;
    }

    static void StopContainer() {
        LOG(INFO) << "Stopping MQTT container...";
        [[maybe_unused]] int stop_result = std::system(("docker stop " + std::string(CONTAINER_NAME) + " 2>/dev/null").c_str());
        [[maybe_unused]] int rm_result = std::system(("docker rm -f " + std::string(CONTAINER_NAME) + " 2>/dev/null").c_str());
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        container_started = false;
    }
};

// Static member definitions
inline std::string MqttTestFixture::mqtt_host;
inline int MqttTestFixture::mqtt_port = 11883;
inline bool MqttTestFixture::container_started = false;

}  // namespace ifex::test
