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

// =============================================================================
// Hash-based Protocol Serialization Tests
// =============================================================================

TEST_F(DiscoverySyncBridgeTest, HashListSerialization) {
    swdv::discovery_sync_envelope::hash_list_t hash_list;
    auto* entry1 = hash_list.add_hashes();
    entry1->set_service_name("service1");
    entry1->set_schema_hash("abcd1234567890abcdef1234567890abcdef1234567890abcdef1234567890ab");
    auto* entry2 = hash_list.add_hashes();
    entry2->set_service_name("service2");
    entry2->set_schema_hash("1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef");

    std::string serialized;
    ASSERT_TRUE(hash_list.SerializeToString(&serialized));
    EXPECT_GT(serialized.size(), 0);

    swdv::discovery_sync_envelope::hash_list_t parsed;
    ASSERT_TRUE(parsed.ParseFromString(serialized));
    ASSERT_EQ(parsed.hashes_size(), 2);
    EXPECT_EQ(parsed.hashes(0).schema_hash(), "abcd1234567890abcdef1234567890abcdef1234567890abcdef1234567890ab");
    EXPECT_EQ(parsed.hashes(1).schema_hash(), "1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef");
}

TEST_F(DiscoverySyncBridgeTest, SchemaMapSerialization) {
    swdv::discovery_sync_envelope::schema_map_t schema_map;
    auto* entry1 = schema_map.add_schemas();
    entry1->set_schema_hash("hash1");
    entry1->set_ifex_schema("name: service1\nversion: 1.0.0");
    auto* entry2 = schema_map.add_schemas();
    entry2->set_schema_hash("hash2");
    entry2->set_ifex_schema("name: service2\nversion: 2.0.0");

    std::string serialized;
    ASSERT_TRUE(schema_map.SerializeToString(&serialized));
    EXPECT_GT(serialized.size(), 0);

    swdv::discovery_sync_envelope::schema_map_t parsed;
    ASSERT_TRUE(parsed.ParseFromString(serialized));
    ASSERT_EQ(parsed.schemas_size(), 2);
    EXPECT_EQ(parsed.schemas(0).schema_hash(), "hash1");
    EXPECT_EQ(parsed.schemas(0).ifex_schema(), "name: service1\nversion: 1.0.0");
    EXPECT_EQ(parsed.schemas(1).schema_hash(), "hash2");
    EXPECT_EQ(parsed.schemas(1).ifex_schema(), "name: service2\nversion: 2.0.0");
}

TEST_F(DiscoverySyncBridgeTest, DiscoveryEnvelopeWithManifest) {
    swdv::discovery_sync_envelope::discovery_envelope_t envelope;
    envelope.set_vehicle_id("vehicle-001");

    auto* manifest = envelope.mutable_manifest();
    auto* entry = manifest->add_hashes();
    entry->set_service_name("test_service");
    entry->set_schema_hash("abcd1234567890abcdef1234567890abcdef1234567890abcdef1234567890ab");

    std::string serialized;
    ASSERT_TRUE(envelope.SerializeToString(&serialized));

    swdv::discovery_sync_envelope::discovery_envelope_t parsed;
    ASSERT_TRUE(parsed.ParseFromString(serialized));
    EXPECT_EQ(parsed.vehicle_id(), "vehicle-001");
    EXPECT_TRUE(parsed.has_manifest());
    EXPECT_FALSE(parsed.has_request());
    EXPECT_FALSE(parsed.has_schemas());
    ASSERT_EQ(parsed.manifest().hashes_size(), 1);
    EXPECT_EQ(parsed.manifest().hashes(0).schema_hash(), "abcd1234567890abcdef1234567890abcdef1234567890abcdef1234567890ab");
}

TEST_F(DiscoverySyncBridgeTest, DiscoveryEnvelopeWithSchemaRequest) {
    swdv::discovery_sync_envelope::discovery_envelope_t envelope;
    envelope.set_vehicle_id("vehicle-001");

    auto* request = envelope.mutable_request();
    request->add_hashes("unknown_hash_1");
    request->add_hashes("unknown_hash_2");

    std::string serialized;
    ASSERT_TRUE(envelope.SerializeToString(&serialized));

    swdv::discovery_sync_envelope::discovery_envelope_t parsed;
    ASSERT_TRUE(parsed.ParseFromString(serialized));
    EXPECT_EQ(parsed.vehicle_id(), "vehicle-001");
    EXPECT_FALSE(parsed.has_manifest());
    EXPECT_TRUE(parsed.has_request());
    EXPECT_FALSE(parsed.has_schemas());
    ASSERT_EQ(parsed.request().hashes_size(), 2);
}

TEST_F(DiscoverySyncBridgeTest, DiscoveryEnvelopeWithSchemas) {
    swdv::discovery_sync_envelope::discovery_envelope_t envelope;
    envelope.set_vehicle_id("vehicle-001");

    auto* schemas = envelope.mutable_schemas();
    auto* entry = schemas->add_schemas();
    entry->set_schema_hash("hash1");
    entry->set_ifex_schema("name: service1\nversion: 1.0.0");

    std::string serialized;
    ASSERT_TRUE(envelope.SerializeToString(&serialized));

    swdv::discovery_sync_envelope::discovery_envelope_t parsed;
    ASSERT_TRUE(parsed.ParseFromString(serialized));
    EXPECT_EQ(parsed.vehicle_id(), "vehicle-001");
    EXPECT_FALSE(parsed.has_manifest());
    EXPECT_FALSE(parsed.has_request());
    EXPECT_TRUE(parsed.has_schemas());
    ASSERT_EQ(parsed.schemas().schemas_size(), 1);
    EXPECT_EQ(parsed.schemas().schemas(0).schema_hash(), "hash1");
    EXPECT_EQ(parsed.schemas().schemas(0).ifex_schema(), "name: service1\nversion: 1.0.0");
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
    EXPECT_EQ(stats.manifests_sent, 0);
    EXPECT_EQ(stats.schema_responses_sent, 0);
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
