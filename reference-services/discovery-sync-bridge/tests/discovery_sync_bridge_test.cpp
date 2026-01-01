/**
 * @file discovery_sync_bridge_test.cpp
 * @brief Unit tests for DiscoverySyncBridge
 */

#include <gtest/gtest.h>
#include <glog/logging.h>

#include "discovery_sync_bridge.hpp"
#include "ifex_content_ids.hpp"
#include "discovery-sync-envelope.pb.h"

namespace ifex::reference {
namespace {

class DiscoverySyncBridgeTest : public ::testing::Test {
protected:
    void SetUp() override {
    }

    void TearDown() override {
    }
};

// =============================================================================
// Unit Tests (no external dependencies)
// =============================================================================

TEST_F(DiscoverySyncBridgeTest, ConfigDefaults) {
    DiscoverySyncBridgeConfig config;
    EXPECT_EQ(config.discovery_endpoint, "localhost:50051");
    EXPECT_EQ(config.backend_transport_endpoint, "localhost:50060");
    EXPECT_EQ(config.sync_content_id, 201);
    EXPECT_EQ(config.vehicle_id, "vehicle-001");
    EXPECT_EQ(config.initialization_delay_ms, 5000);
    EXPECT_EQ(config.poll_interval_ms, 1000);
    EXPECT_EQ(config.batch_window_ms, 100);
    EXPECT_EQ(config.heartbeat_interval_ms, 30000);
    EXPECT_TRUE(config.state_persistence_path.empty());
}

TEST_F(DiscoverySyncBridgeTest, ContentIdConstants) {
    EXPECT_EQ(ifex::content_id::DISCOVERY_SYNC, 201);
    EXPECT_EQ(ifex::content_id::SCHEDULER_SYNC, 202);
}

TEST_F(DiscoverySyncBridgeTest, SyncEventTypeSerialization) {
    swdv::discovery_sync_envelope::sync_event_t event;
    event.set_event_type(swdv::discovery_sync_envelope::SERVICE_REGISTERED);
    event.set_sequence_number(42);
    event.set_timestamp_ns(1234567890000000000);
    event.set_registration_id("reg_001");

    auto* info = event.mutable_service_info();
    info->set_registration_id("reg_001");
    info->set_name("test-service");
    info->set_version("1.0.0");
    info->mutable_endpoint()->set_address("localhost:50055");
    info->mutable_endpoint()->set_transport(swdv::discovery_sync_envelope::GRPC);
    info->set_status(swdv::discovery_sync_envelope::AVAILABLE);
    info->set_last_heartbeat_ms(1234567890000);

    std::string serialized;
    ASSERT_TRUE(event.SerializeToString(&serialized));
    EXPECT_GT(serialized.size(), 0);

    swdv::discovery_sync_envelope::sync_event_t parsed;
    ASSERT_TRUE(parsed.ParseFromString(serialized));
    EXPECT_EQ(parsed.event_type(), swdv::discovery_sync_envelope::SERVICE_REGISTERED);
    EXPECT_EQ(parsed.sequence_number(), 42);
    EXPECT_EQ(parsed.registration_id(), "reg_001");
    EXPECT_EQ(parsed.service_info().name(), "test-service");
    EXPECT_EQ(parsed.service_info().status(), swdv::discovery_sync_envelope::AVAILABLE);
}

TEST_F(DiscoverySyncBridgeTest, SyncMessageSerialization) {
    swdv::discovery_sync_envelope::sync_message_t message;
    message.set_vehicle_id("vehicle-001");
    message.set_bridge_instance_id("dsb_1234567890abcdef");
    message.set_state_checksum(0xDEADBEEF);
    message.set_total_services(3);

    // Add FULL_SYNC event
    auto* event = message.add_events();
    event->set_event_type(swdv::discovery_sync_envelope::FULL_SYNC);
    event->set_sequence_number(1);
    event->set_timestamp_ns(1234567890000000000);

    std::string serialized;
    ASSERT_TRUE(message.SerializeToString(&serialized));
    EXPECT_GT(serialized.size(), 0);

    swdv::discovery_sync_envelope::sync_message_t parsed;
    ASSERT_TRUE(parsed.ParseFromString(serialized));
    EXPECT_EQ(parsed.vehicle_id(), "vehicle-001");
    EXPECT_EQ(parsed.bridge_instance_id(), "dsb_1234567890abcdef");
    EXPECT_EQ(parsed.state_checksum(), 0xDEADBEEF);
    EXPECT_EQ(parsed.total_services(), 3);
    EXPECT_EQ(parsed.events_size(), 1);
    EXPECT_EQ(parsed.events(0).event_type(), swdv::discovery_sync_envelope::FULL_SYNC);
}

TEST_F(DiscoverySyncBridgeTest, EventTypeValues) {
    EXPECT_EQ(swdv::discovery_sync_envelope::FULL_SYNC, 0);
    EXPECT_EQ(swdv::discovery_sync_envelope::SERVICE_REGISTERED, 1);
    EXPECT_EQ(swdv::discovery_sync_envelope::SERVICE_UNREGISTERED, 2);
    EXPECT_EQ(swdv::discovery_sync_envelope::SERVICE_STATUS_CHANGED, 3);
    EXPECT_EQ(swdv::discovery_sync_envelope::HEARTBEAT, 4);
}

TEST_F(DiscoverySyncBridgeTest, ServiceStatusValues) {
    EXPECT_EQ(swdv::discovery_sync_envelope::AVAILABLE, 0);
    EXPECT_EQ(swdv::discovery_sync_envelope::UNAVAILABLE, 1);
    EXPECT_EQ(swdv::discovery_sync_envelope::STARTING, 2);
    EXPECT_EQ(swdv::discovery_sync_envelope::STOPPING, 3);
    EXPECT_EQ(swdv::discovery_sync_envelope::ERROR, 4);
}

TEST_F(DiscoverySyncBridgeTest, TransportTypeValues) {
    EXPECT_EQ(swdv::discovery_sync_envelope::GRPC, 0);
    EXPECT_EQ(swdv::discovery_sync_envelope::HTTP_REST, 1);
    EXPECT_EQ(swdv::discovery_sync_envelope::DBUS, 2);
    EXPECT_EQ(swdv::discovery_sync_envelope::SOMEIP, 3);
    EXPECT_EQ(swdv::discovery_sync_envelope::MQTT, 4);
}

TEST_F(DiscoverySyncBridgeTest, SyncedServiceStateHash) {
    SyncedServiceState state1;
    state1.registration_id = "reg_001";
    state1.name = "service1";
    state1.version = "1.0.0";
    state1.address = "localhost:50055";
    state1.status = swdv::discovery_sync_envelope::AVAILABLE;
    state1.last_heartbeat_ms = 1000;

    SyncedServiceState state2 = state1;

    // Same state should have same hash
    EXPECT_EQ(state1.ComputeHash(), state2.ComputeHash());

    // Different status should have different hash
    state2.status = swdv::discovery_sync_envelope::UNAVAILABLE;
    EXPECT_NE(state1.ComputeHash(), state2.ComputeHash());

    // Different heartbeat should have different hash
    state2 = state1;
    state2.last_heartbeat_ms = 2000;
    EXPECT_NE(state1.ComputeHash(), state2.ComputeHash());

    // Different name should have different hash
    state2 = state1;
    state2.name = "service2";
    EXPECT_NE(state1.ComputeHash(), state2.ComputeHash());
}

TEST_F(DiscoverySyncBridgeTest, SyncAckSerialization) {
    swdv::discovery_sync_envelope::sync_ack_t ack;
    ack.set_last_sequence_received(100);
    ack.set_checksum_match(true);
    ack.set_request_full_sync(false);

    std::string serialized;
    ASSERT_TRUE(ack.SerializeToString(&serialized));

    swdv::discovery_sync_envelope::sync_ack_t parsed;
    ASSERT_TRUE(parsed.ParseFromString(serialized));
    EXPECT_EQ(parsed.last_sequence_received(), 100);
    EXPECT_TRUE(parsed.checksum_match());
    EXPECT_FALSE(parsed.request_full_sync());
}

// =============================================================================
// Integration Tests (require running services + Docker for MQTT)
// =============================================================================

// These tests are marked as DISABLED_ because they require:
// 1. Backend Transport service running
// 2. Discovery service running
// 3. MQTT broker (via Docker)
// Run them manually with: --gtest_also_run_disabled_tests

TEST_F(DiscoverySyncBridgeTest, DISABLED_StartStop) {
    DiscoverySyncBridgeConfig config;
    config.discovery_endpoint = "localhost:50051";
    config.backend_transport_endpoint = "localhost:50060";
    config.initialization_delay_ms = 100;  // Short delay for testing

    DiscoverySyncBridge bridge(config);
    ASSERT_TRUE(bridge.Start());
    EXPECT_TRUE(bridge.IsRunning());

    // Wait for initialization
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_TRUE(bridge.IsInitialized());

    bridge.Stop();
    EXPECT_FALSE(bridge.IsRunning());
}

TEST_F(DiscoverySyncBridgeTest, DISABLED_StatsInitiallyZero) {
    DiscoverySyncBridgeConfig config;
    DiscoverySyncBridge bridge(config);

    auto stats = bridge.GetStats();
    EXPECT_EQ(stats.events_sent, 0);
    EXPECT_EQ(stats.full_syncs_sent, 0);
    EXPECT_EQ(stats.delta_syncs_sent, 0);
    EXPECT_EQ(stats.heartbeats_sent, 0);
    EXPECT_EQ(stats.bytes_sent, 0);
    EXPECT_FALSE(stats.is_initialized);
    EXPECT_FALSE(stats.is_connected);
}

}  // namespace
}  // namespace ifex::reference

int main(int argc, char** argv) {
    google::InitGoogleLogging(argv[0]);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
