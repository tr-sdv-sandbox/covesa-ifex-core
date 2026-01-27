/**
 * @file dispatcher_bridge_test.cpp
 * @brief Tests for DispatcherBridge
 */

#include <gtest/gtest.h>
#include <glog/logging.h>

#include "dispatcher_bridge.hpp"
#include "ifex_content_ids.hpp"
#include "dispatcher-rpc-envelope.pb.h"

namespace ifex::reference {
namespace {

class DispatcherBridgeTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Tests require running services, so most are integration tests
    }

    void TearDown() override {
    }
};

// =============================================================================
// Unit Tests (no external dependencies)
// =============================================================================

TEST_F(DispatcherBridgeTest, ConfigDefaults) {
    DispatcherBridgeConfig config;
    EXPECT_EQ(config.dispatcher_endpoint, "localhost:50052");
    EXPECT_EQ(config.backend_transport_endpoint, "localhost:50060");
    EXPECT_EQ(config.rpc_content_id, 200);
    EXPECT_EQ(config.max_concurrent_requests, 100);
    EXPECT_EQ(config.default_timeout_ms, 30000);
    EXPECT_EQ(config.num_workers, 4);
}

TEST_F(DispatcherBridgeTest, ContentIdConstants) {
    EXPECT_EQ(ifex::content_id::DISPATCHER_RPC, 200);
    EXPECT_EQ(ifex::content_id::DISCOVERY_SYNC, 201);
    EXPECT_EQ(ifex::content_id::SCHEDULER_SYNC, 202);
    EXPECT_EQ(ifex::content_id::APP_BASE, 1000);
}

TEST_F(DispatcherBridgeTest, RpcRequestSerialization) {
    swdv::dispatcher_rpc_envelope::rpc_request_t request;
    request.set_correlation_id("test-123");
    request.set_service_name("climate-comfort");
    request.set_method_name("set_temperature");
    request.set_parameters_json(R"({"target": 22})");
    request.set_timeout_ms(5000);
    request.set_request_timestamp_ms(1234567890000);

    std::string serialized;
    ASSERT_TRUE(request.SerializeToString(&serialized));
    EXPECT_GT(serialized.size(), 0);

    swdv::dispatcher_rpc_envelope::rpc_request_t parsed;
    ASSERT_TRUE(parsed.ParseFromString(serialized));
    EXPECT_EQ(parsed.correlation_id(), "test-123");
    EXPECT_EQ(parsed.service_name(), "climate-comfort");
    EXPECT_EQ(parsed.method_name(), "set_temperature");
    EXPECT_EQ(parsed.parameters_json(), R"({"target": 22})");
    EXPECT_EQ(parsed.timeout_ms(), 5000);
    EXPECT_EQ(parsed.request_timestamp_ms(), 1234567890000);
}

TEST_F(DispatcherBridgeTest, RpcResponseSerialization) {
    swdv::dispatcher_rpc_envelope::rpc_response_t response;
    response.set_correlation_id("test-123");
    response.set_status(swdv::dispatcher_rpc_envelope::SUCCESS);
    response.set_result_json(R"({"acknowledged": true})");
    response.set_duration_ms(150);
    response.set_service_endpoint("localhost:50055");
    response.set_response_timestamp_ms(1234567890500);

    std::string serialized;
    ASSERT_TRUE(response.SerializeToString(&serialized));
    EXPECT_GT(serialized.size(), 0);

    swdv::dispatcher_rpc_envelope::rpc_response_t parsed;
    ASSERT_TRUE(parsed.ParseFromString(serialized));
    EXPECT_EQ(parsed.correlation_id(), "test-123");
    EXPECT_EQ(parsed.status(), swdv::dispatcher_rpc_envelope::SUCCESS);
    EXPECT_EQ(parsed.result_json(), R"({"acknowledged": true})");
    EXPECT_EQ(parsed.duration_ms(), 150);
    EXPECT_EQ(parsed.service_endpoint(), "localhost:50055");
    EXPECT_EQ(parsed.response_timestamp_ms(), 1234567890500);
}

TEST_F(DispatcherBridgeTest, RpcStatusValues) {
    EXPECT_EQ(swdv::dispatcher_rpc_envelope::SUCCESS, 0);
    EXPECT_EQ(swdv::dispatcher_rpc_envelope::FAILED, 1);
    EXPECT_EQ(swdv::dispatcher_rpc_envelope::TIMEOUT, 2);
    EXPECT_EQ(swdv::dispatcher_rpc_envelope::SERVICE_UNAVAILABLE, 3);
    EXPECT_EQ(swdv::dispatcher_rpc_envelope::METHOD_NOT_FOUND, 4);
    EXPECT_EQ(swdv::dispatcher_rpc_envelope::INVALID_PARAMETERS, 5);
    EXPECT_EQ(swdv::dispatcher_rpc_envelope::TRANSPORT_ERROR, 6);
    EXPECT_EQ(swdv::dispatcher_rpc_envelope::DUPLICATE_REQUEST, 7);
}

// =============================================================================
// Integration Tests (require running services + Docker for MQTT)
// =============================================================================

// These tests are marked as DISABLED_ because they require:
// 1. Backend Transport service running
// 2. Dispatcher service running
// 3. MQTT broker (via Docker)
// Run them manually with: --gtest_also_run_disabled_tests

TEST_F(DispatcherBridgeTest, DISABLED_StartStop) {
    DispatcherBridgeConfig config;
    config.dispatcher_endpoint = "localhost:50052";
    config.backend_transport_endpoint = "localhost:50060";

    DispatcherBridge bridge(config);
    ASSERT_TRUE(bridge.Start());
    EXPECT_TRUE(bridge.IsRunning());

    bridge.Stop();
    EXPECT_FALSE(bridge.IsRunning());
}

TEST_F(DispatcherBridgeTest, DISABLED_StatsInitiallyZero) {
    DispatcherBridgeConfig config;
    DispatcherBridge bridge(config);

    auto stats = bridge.GetStats();
    EXPECT_EQ(stats.requests_received, 0);
    EXPECT_EQ(stats.requests_completed, 0);
    EXPECT_EQ(stats.requests_failed, 0);
    EXPECT_EQ(stats.requests_timed_out, 0);
    EXPECT_EQ(stats.requests_rejected, 0);
    EXPECT_EQ(stats.pending_count, 0);
}

}  // namespace
}  // namespace ifex::reference

int main(int argc, char** argv) {
    google::InitGoogleLogging(argv[0]);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
