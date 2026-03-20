#include "test_fixture.hpp"

#include "cloud_backend_transport_server.hpp"
#include "cloud_backend_transport_client.hpp"
#include "backend_transport_server.hpp"
#include "cloud_vehicle_sync_bridge.hpp"
#include "sqlite_cloud_vehicle_db_adapter.hpp"
#include "cloud-vehicle-sync-envelope.pb.h"

#include <glog/logging.h>
#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>
#include <mosquitto.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace ifex {
namespace test {

using namespace std::chrono_literals;

class CloudVehicleSyncBridgeIntegrationTest : public IntegrationTestFixture {
protected:
    static constexpr std::uint32_t kSyncContentId = 242;
    static constexpr int kVehicleTransportPort = 51260;
    static constexpr int kCloudTransportPort = 51261;
    static constexpr const char* kVehicleId = "cloud-vehicle-sync-vehicle-01";
    static constexpr const char* kTruckNodeId = "truck-01";

    std::shared_ptr<sync::SqliteCloudVehicleDbAdapter> cloud_adapter_;
    std::shared_ptr<sync::SqliteCloudVehicleDbAdapter> truck_adapter_;
    std::filesystem::path cloud_db_path_;
    std::filesystem::path truck_db_path_;

    std::unique_ptr<ifex::cloud::CloudBackendTransportServer> cloud_transport_;
    std::unique_ptr<grpc::Server> cloud_transport_grpc_;

    std::unique_ptr<ifex::reference::BackendTransportServer> truck_transport_;
    std::unique_ptr<grpc::Server> truck_transport_grpc_;

    std::unique_ptr<ifex::sync::bridge::CloudVehicleCloudBridge> cloud_bridge_;
    std::unique_ptr<ifex::sync::bridge::CloudVehicleTruckBridge> truck_bridge_;

    std::unique_ptr<ifex::cloud::CloudBackendTransportClient> cloud_transport_client_;

    void SetUp() override {
        IntegrationTestFixture::SetUp();
        if (!IsMqttAvailable()) {
            GTEST_SKIP() << "MQTT not available";
            return;
        }

        cloud_db_path_ = UniqueDbPath("cloud");
        truck_db_path_ = UniqueDbPath("truck");
        std::filesystem::remove(cloud_db_path_);
        std::filesystem::remove(truck_db_path_);

        cloud_adapter_ = std::make_shared<sync::SqliteCloudVehicleDbAdapter>(MakeAdapterConfig(cloud_db_path_));
        truck_adapter_ = std::make_shared<sync::SqliteCloudVehicleDbAdapter>(MakeAdapterConfig(truck_db_path_));

        ASSERT_TRUE(StartCloudTransport());
        ASSERT_TRUE(StartTruckTransport());

        cloud_transport_client_ = std::make_unique<ifex::cloud::CloudBackendTransportClient>(
            "localhost:" + std::to_string(kCloudTransportPort), kSyncContentId);
    }

    void TearDown() override {
        StopTruckBridge();
        StopCloudBridge();
        StopTruckTransport();
        StopCloudTransport();

        cloud_transport_client_.reset();
        cloud_adapter_.reset();
        truck_adapter_.reset();

        std::filesystem::remove(cloud_db_path_);
        std::filesystem::remove(truck_db_path_);

        IntegrationTestFixture::TearDown();
    }

    static std::filesystem::path UniqueDbPath(const char* role) {
        const auto stamp = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        return std::filesystem::temp_directory_path() /
               (std::string("ifex-sync-bridge-") + role + "-" + std::to_string(stamp) + ".sqlite");
    }

    static sync::DatabaseAdapterConfig MakeAdapterConfig(const std::filesystem::path& path) {
        sync::DatabaseAdapterConfig config;
        config.database_path = path.string();
        config.namespace_owners = {
            {"shared", sync::RecordOwner::kShared},
        };
        config.default_owner = sync::RecordOwner::kShared;
        return config;
    }

    bool StartCloudTransport() {
        ifex::cloud::CloudBackendTransportServer::Config config;
        config.mqtt_host = GetMqttHost();
        config.mqtt_port = GetMqttPort();
        config.partition_id = 0;
        config.total_partitions = 1;
        cloud_transport_ = std::make_unique<ifex::cloud::CloudBackendTransportServer>(config);
        if (!cloud_transport_->Start()) {
            return false;
        }

        grpc::ServerBuilder builder;
        builder.AddListeningPort("0.0.0.0:" + std::to_string(kCloudTransportPort),
                                 grpc::InsecureServerCredentials());
        using namespace swdv::cloud_backend_transport_service;
        builder.RegisterService(static_cast<send_to_vehicle_service::Service*>(cloud_transport_.get()));
        builder.RegisterService(static_cast<get_vehicle_status_service::Service*>(cloud_transport_.get()));
        builder.RegisterService(static_cast<get_channel_info_service::Service*>(cloud_transport_.get()));
        builder.RegisterService(static_cast<get_queue_status_service::Service*>(cloud_transport_.get()));
        builder.RegisterService(static_cast<get_stats_service::Service*>(cloud_transport_.get()));
        builder.RegisterService(static_cast<healthy_service::Service*>(cloud_transport_.get()));
        builder.RegisterService(static_cast<on_vehicle_message_service::Service*>(cloud_transport_.get()));
        builder.RegisterService(static_cast<on_ack_service::Service*>(cloud_transport_.get()));
        builder.RegisterService(static_cast<on_vehicle_status_service::Service*>(cloud_transport_.get()));
        builder.RegisterService(static_cast<on_queue_status_changed_service::Service*>(cloud_transport_.get()));
        cloud_transport_grpc_ = builder.BuildAndStart();
        return cloud_transport_grpc_ != nullptr;
    }

    bool StartTruckTransport() {
        ifex::reference::BackendTransportServer::Config config;
        config.mqtt_host = GetMqttHost();
        config.mqtt_port = GetMqttPort();
        config.vehicle_id = kVehicleId;
        config.persistence_dir = "/tmp/ifex-sync-bridge-truck-transport";
        truck_transport_ = std::make_unique<ifex::reference::BackendTransportServer>(config);
        if (!truck_transport_->Start()) {
            return false;
        }

        grpc::ServerBuilder builder;
        builder.AddListeningPort("0.0.0.0:" + std::to_string(kVehicleTransportPort),
                                 grpc::InsecureServerCredentials());
        using namespace swdv::backend_transport_service;
        builder.RegisterService(static_cast<publish_service::Service*>(truck_transport_.get()));
        builder.RegisterService(static_cast<get_connection_status_service::Service*>(truck_transport_.get()));
        builder.RegisterService(static_cast<get_queue_status_service::Service*>(truck_transport_.get()));
        builder.RegisterService(static_cast<get_stats_service::Service*>(truck_transport_.get()));
        builder.RegisterService(static_cast<healthy_service::Service*>(truck_transport_.get()));
        builder.RegisterService(static_cast<get_content_id_service::Service*>(truck_transport_.get()));
        builder.RegisterService(static_cast<on_content_service::Service*>(truck_transport_.get()));
        builder.RegisterService(static_cast<on_ack_service::Service*>(truck_transport_.get()));
        builder.RegisterService(static_cast<on_connection_changed_service::Service*>(truck_transport_.get()));
        builder.RegisterService(static_cast<on_queue_status_changed_service::Service*>(truck_transport_.get()));
        truck_transport_grpc_ = builder.BuildAndStart();
        return truck_transport_grpc_ != nullptr;
    }

    void StopCloudTransport() {
        if (cloud_transport_grpc_) {
            cloud_transport_grpc_->Shutdown();
            cloud_transport_grpc_.reset();
        }
        if (cloud_transport_) {
            cloud_transport_->Stop();
            cloud_transport_.reset();
        }
    }

    void StopTruckTransport() {
        if (truck_transport_grpc_) {
            truck_transport_grpc_->Shutdown();
            truck_transport_grpc_.reset();
        }
        if (truck_transport_) {
            truck_transport_->Stop();
            truck_transport_.reset();
        }
    }

    bool StartCloudBridge() {
        sync::bridge::CloudBridgeConfig config;
        config.common.local_node_id = "cloud";
        config.common.remote_node_id = kTruckNodeId;
        config.common.namespace_name = "shared";
        config.common.content_id = kSyncContentId;
        config.common.poll_interval_ms = 200;
        config.common.heartbeat_interval_ms = 800;
        config.common.max_batch_records = 64;
        config.common.adapter = cloud_adapter_;
        config.cloud_transport_address = "localhost:" + std::to_string(kCloudTransportPort);
        config.vehicle_id = kVehicleId;

        cloud_bridge_ = std::make_unique<sync::bridge::CloudVehicleCloudBridge>(std::move(config));
        return cloud_bridge_->Start();
    }

    bool StartTruckBridge() {
        sync::bridge::TruckBridgeConfig config;
        config.common.local_node_id = kTruckNodeId;
        config.common.remote_node_id = "cloud";
        config.common.namespace_name = "shared";
        config.common.content_id = kSyncContentId;
        config.common.poll_interval_ms = 200;
        config.common.heartbeat_interval_ms = 800;
        config.common.max_batch_records = 64;
        config.common.adapter = truck_adapter_;
        config.backend_transport_address = "localhost:" + std::to_string(kVehicleTransportPort);

        truck_bridge_ = std::make_unique<sync::bridge::CloudVehicleTruckBridge>(std::move(config));
        return truck_bridge_->Start();
    }

    void StopCloudBridge() {
        if (cloud_bridge_) {
            cloud_bridge_->Stop();
            cloud_bridge_.reset();
        }
    }

    void StopTruckBridge() {
        if (truck_bridge_) {
            truck_bridge_->Stop();
            truck_bridge_.reset();
        }
    }

    static sync::CanonicalRecord MakeRecord(const std::string& id,
                                            const std::string& origin,
                                            std::uint64_t cloud_seq,
                                            std::uint64_t truck_seq,
                                            const std::string& payload,
                                            const std::string& idempotency_key) {
        sync::CanonicalRecord record;
        record.locator.namespace_name = "shared";
        record.locator.origin_node_id = origin;
        record.locator.record_id.assign(id.begin(), id.end());
        record.version_vector.cloud_seq = cloud_seq;
        record.version_vector.truck_seq = truck_seq;
        record.operation = sync::RecordOperation::kUpdate;
        record.payload.assign(payload.begin(), payload.end());
        record.schema_version = 1;
        record.idempotency_key = idempotency_key;
        record.correlation_id = idempotency_key;
        record.updated_at_ms = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
        return record;
    }

    static sync::SyncSessionKey CloudSession() {
        sync::SyncSessionKey key;
        key.local_node_id = "cloud";
        key.remote_node_id = kTruckNodeId;
        key.namespace_name = "shared";
        return key;
    }

    static sync::SyncSessionKey TruckSession() {
        sync::SyncSessionKey key;
        key.local_node_id = kTruckNodeId;
        key.remote_node_id = "cloud";
        key.namespace_name = "shared";
        return key;
    }

    bool WaitFor(const std::function<bool()>& predicate,
                 std::chrono::seconds timeout = 20s,
                 std::chrono::milliseconds interval = 150ms) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (predicate()) {
                return true;
            }
            std::this_thread::sleep_for(interval);
        }
        return predicate();
    }

    bool HasRecord(sync::CloudVehicleDbAdapter& adapter, const std::string& id) {
        const auto ids = adapter.list_record_ids({"shared", true, 0});
        for (const auto& locator : ids) {
            const std::string record_id(locator.record_id.begin(), locator.record_id.end());
            if (record_id == id) {
                return true;
            }
        }
        return false;
    }

    std::size_t RecordCount(sync::CloudVehicleDbAdapter& adapter) {
        return adapter.list_record_ids({"shared", true, 0}).size();
    }

    bool IsConverged() {
        const auto cloud_checksum = cloud_adapter_->compute_state_checksum({"shared", true});
        const auto truck_checksum = truck_adapter_->compute_state_checksum({"shared", true});
        if (cloud_checksum != truck_checksum) {
            return false;
        }

        const bool cloud_dirty = !cloud_adapter_->list_dirty_records({CloudSession(), 1, true}).empty();
        const bool truck_dirty = !truck_adapter_->list_dirty_records({TruckSession(), 1, true}).empty();
        return !cloud_dirty && !truck_dirty;
    }

    void ApplyCloudLocal(const sync::CanonicalRecord& record) {
        const auto result = cloud_adapter_->apply_record(record, record.idempotency_key);
        ASSERT_TRUE(result.disposition == sync::ApplyDisposition::kApplied ||
                    result.disposition == sync::ApplyDisposition::kDuplicate);
    }

    void ApplyTruckLocal(const sync::CanonicalRecord& record) {
        const auto result = truck_adapter_->apply_record(record, record.idempotency_key);
        ASSERT_TRUE(result.disposition == sync::ApplyDisposition::kApplied ||
                    result.disposition == sync::ApplyDisposition::kDuplicate);
    }

    bool SendRawEnvelopeToTruck(const swdv::cloud_vehicle_sync_envelope::CloudVehicleSyncEnvelope& envelope) {
        std::string serialized;
        if (!envelope.SerializeToString(&serialized)) {
            return false;
        }
        const std::vector<std::uint8_t> payload(serialized.begin(), serialized.end());
        const auto result = cloud_transport_client_->SendToVehicle(
            kVehicleId,
            payload,
            swdv::cloud_backend_transport_service::persistence_t::VOLATILE);
        return static_cast<int>(result.status()) == 0;
    }
};

TEST_F(CloudVehicleSyncBridgeIntegrationTest, BridgesStartIndependentlyAndRecoverAfterDisconnect) {
    ASSERT_TRUE(StartCloudBridge());
    ASSERT_TRUE(cloud_bridge_->IsRunning());

    ApplyCloudLocal(MakeRecord("init-cloud", "cloud", 1, 0, "payload-a", "idem-init-cloud"));

    ASSERT_TRUE(StartTruckBridge());
    ASSERT_TRUE(WaitFor([&]() { return HasRecord(*truck_adapter_, "init-cloud"); }, 20s));

    StopTruckTransport();

    ApplyCloudLocal(MakeRecord("offline-cloud", "cloud", 2, 0, "payload-b", "idem-offline-cloud"));
    ApplyTruckLocal(MakeRecord("offline-truck", kTruckNodeId, 0, 1, "payload-c", "idem-offline-truck"));

    ASSERT_TRUE(StartTruckTransport());

    ASSERT_TRUE(WaitFor([&]() { return IsConverged(); }, 30s));
    EXPECT_TRUE(HasRecord(*cloud_adapter_, "offline-cloud"));
    EXPECT_TRUE(HasRecord(*cloud_adapter_, "offline-truck"));
    EXPECT_TRUE(HasRecord(*truck_adapter_, "offline-cloud"));
    EXPECT_TRUE(HasRecord(*truck_adapter_, "offline-truck"));
}

TEST_F(CloudVehicleSyncBridgeIntegrationTest, DuplicateDeliveryIsIdempotent) {
    ASSERT_TRUE(StartCloudBridge());
    ASSERT_TRUE(StartTruckBridge());

    swdv::cloud_vehicle_sync_envelope::CloudVehicleSyncEnvelope envelope;
    auto* sync = envelope.mutable_sync_exchange();
    sync->set_sender_node_id("cloud");
    sync->set_recipient_node_id(kTruckNodeId);
    sync->set_state_checksum(cloud_adapter_->compute_state_checksum({"shared", true}));
    sync->set_correlation_id("dup-corr");
    sync->set_idempotency_key("dup-msg");
    auto* record = sync->add_records();
    record->mutable_locator()->set_namespace_name("shared");
    record->mutable_locator()->set_origin_node_id("cloud");
    record->mutable_locator()->set_record_id("dup-1");
    record->mutable_version_vector()->set_cloud_seq(10);
    record->mutable_version_vector()->set_truck_seq(0);
    record->set_operation(swdv::cloud_vehicle_sync_envelope::RECORD_OPERATION_UPDATE);
    record->set_payload("dup-payload");
    record->set_schema_version(1);
    record->set_idempotency_key("dup-record-key");
    record->set_correlation_id("dup-corr");

    ASSERT_TRUE(SendRawEnvelopeToTruck(envelope));
    ASSERT_TRUE(SendRawEnvelopeToTruck(envelope));

    ASSERT_TRUE(WaitFor([&]() { return RecordCount(*truck_adapter_) == 1; }, 20s));

    const auto cloud_conflicts = cloud_adapter_->query_conflicts({"shared", 0, true, 100});
    const auto truck_conflicts = truck_adapter_->query_conflicts({"shared", 0, true, 100});
    EXPECT_TRUE(cloud_conflicts.empty());
    EXPECT_TRUE(truck_conflicts.empty());

    const auto truck_stats = truck_bridge_->GetStats();
    EXPECT_GE(truck_stats.records_duplicated, 1U);
}

TEST_F(CloudVehicleSyncBridgeIntegrationTest, ReconnectOfflineChangesConvergeBackToQuiescence) {
    ASSERT_TRUE(StartCloudBridge());
    ASSERT_TRUE(StartTruckBridge());

    StopTruckTransport();

    ApplyCloudLocal(MakeRecord("reconnect-cloud", "cloud", 20, 0, "payload-cloud", "idem-reconnect-cloud"));
    ApplyTruckLocal(MakeRecord("reconnect-truck", kTruckNodeId, 0, 20, "payload-truck", "idem-reconnect-truck"));

    ASSERT_TRUE(StartTruckTransport());
    ASSERT_TRUE(WaitFor([&]() { return IsConverged(); }, 30s));

    EXPECT_TRUE(HasRecord(*cloud_adapter_, "reconnect-cloud"));
    EXPECT_TRUE(HasRecord(*cloud_adapter_, "reconnect-truck"));
    EXPECT_TRUE(HasRecord(*truck_adapter_, "reconnect-cloud"));
    EXPECT_TRUE(HasRecord(*truck_adapter_, "reconnect-truck"));
}

TEST_F(CloudVehicleSyncBridgeIntegrationTest, GapRecoveryRequestsAndFetchesMissingRecords) {
    ApplyCloudLocal(MakeRecord("gap-1", "cloud", 30, 0, "gap-payload", "idem-gap-1"));

    sync::CheckpointToken cloud_checkpoint;
    cloud_checkpoint.sequence_number = 1;
    cloud_checkpoint.last_record.namespace_name = "shared";
    cloud_checkpoint.last_record.origin_node_id = "cloud";
    cloud_checkpoint.last_record.record_id.assign({'g', 'a', 'p', '-', '1'});
    cloud_checkpoint.last_version.cloud_seq = 30;
    cloud_checkpoint.last_version.truck_seq = 0;
    cloud_adapter_->write_checkpoint(CloudSession(), cloud_checkpoint);

    ASSERT_TRUE(StartCloudBridge());
    ASSERT_TRUE(StartTruckBridge());

    ASSERT_TRUE(WaitFor([&]() { return HasRecord(*truck_adapter_, "gap-1"); }, 30s));
    ASSERT_TRUE(WaitFor([&]() { return IsConverged(); }, 30s));

    const auto cloud_stats = cloud_bridge_->GetStats();
    const auto truck_stats = truck_bridge_->GetStats();
    EXPECT_GE(cloud_stats.gap_requests_sent + truck_stats.gap_requests_sent, 1U);
    EXPECT_GE(cloud_stats.gap_responses_received + truck_stats.gap_responses_received, 1U);
}

}
}

int main(int argc, char** argv) {
    google::InitGoogleLogging(argv[0]);
    FLAGS_logtostderr = true;
    ::testing::InitGoogleTest(&argc, argv);
    mosquitto_lib_init();

    class Environment : public ::testing::Environment {
    public:
        void SetUp() override { IntegrationTestFixture::GlobalSetUp(); }
        void TearDown() override { IntegrationTestFixture::GlobalTearDown(); }
    };
    ::testing::AddGlobalTestEnvironment(new Environment);

    int result = RUN_ALL_TESTS();
    mosquitto_lib_cleanup();
    return result;
}
