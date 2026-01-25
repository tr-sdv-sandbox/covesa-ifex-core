/**
 * @file cloud_scheduler_sync_bridge_test.cpp
 * @brief Unit tests for CloudSchedulerSyncBridge
 *
 * Basic tests for configuration and instantiation.
 * See cloud_scheduler_sync_bridge_integration_test.cpp for E2E tests.
 */

#include "cloud_scheduler_sync_bridge.hpp"
#include <gtest/gtest.h>

namespace ifex::cloud {
namespace {

TEST(CloudSchedulerSyncBridgeTest, ConfigDefaults) {
    CloudSchedulerSyncBridgeConfig config;
    EXPECT_EQ(config.scheduler_address, "localhost:50102");
    EXPECT_EQ(config.transport_address, "localhost:50100");
    EXPECT_EQ(config.content_id, 202);
    EXPECT_TRUE(config.bridge_instance_id.empty());
}

TEST(CloudSchedulerSyncBridgeTest, CreateBridge) {
    CloudSchedulerSyncBridgeConfig config;
    config.scheduler_address = "localhost:50102";
    config.transport_address = "localhost:50100";

    CloudSchedulerSyncBridge bridge(config);
    EXPECT_FALSE(bridge.IsRunning());
}

}  // namespace
}  // namespace ifex::cloud
