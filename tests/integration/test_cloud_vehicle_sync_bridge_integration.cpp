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
#include <functional>
#include <filesystem>
#include <limits>
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

    static sync::CanonicalRecord MakeTombstone(const std::string& id,
                                               const std::string& origin,
                                               std::uint64_t cloud_seq,
                                               std::uint64_t truck_seq,
                                               const std::string& idempotency_key,
                                               const std::string& tombstone_reason) {
        sync::CanonicalRecord record = MakeRecord(id, origin, cloud_seq, truck_seq, "", idempotency_key);
        record.operation = sync::RecordOperation::kDelete;
        record.payload.clear();
        record.tombstone_at_ms = record.updated_at_ms;
        record.tombstone_reason = tombstone_reason;
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

    bool LoadRecord(sync::CloudVehicleDbAdapter& adapter,
                    const sync::SyncSessionKey& session,
                    const std::string& id,
                    sync::CanonicalRecord* out) {
        const auto records = adapter.list_dirty_records({session, 100, true});
        for (const auto& record : records) {
            const std::string record_id(record.locator.record_id.begin(), record.locator.record_id.end());
            if (record_id == id) {
                if (out != nullptr) {
                    *out = record;
                }
                return true;
            }
        }
        return false;
    }

    std::size_t RecordCount(sync::CloudVehicleDbAdapter& adapter) {
        return adapter.list_record_ids({"shared", true, 0}).size();
    }

    std::size_t ConflictCount(sync::CloudVehicleDbAdapter& adapter) {
        return adapter.query_conflicts({"shared", 0, false, 100}).size();
    }

    std::vector<sync::ConflictRecord> Conflicts(sync::CloudVehicleDbAdapter& adapter) {
        return adapter.query_conflicts({"shared", 0, false, 100});
    }

    std::size_t DirtyCount(sync::CloudVehicleDbAdapter& adapter, const sync::SyncSessionKey& session) {
        return adapter.list_dirty_records({session, 100, true}).size();
    }

    std::uint64_t Checksum(sync::CloudVehicleDbAdapter& adapter) {
        return adapter.compute_state_checksum({"shared", true});
    }

    sync::CheckpointReadResult ReadCheckpoint(sync::CloudVehicleDbAdapter& adapter,
                                              const sync::SyncSessionKey& session) {
        return adapter.read_checkpoint(session);
    }

    std::size_t TombstoneCountForGc(sync::CloudVehicleDbAdapter& adapter,
                                    const sync::SyncSessionKey& session) {
        return adapter.list_tombstones_for_gc(
            {session, std::numeric_limits<std::uint64_t>::max(), 100}).size();
    }

    void ForceSyncBoth() {
        if (cloud_bridge_) {
            cloud_bridge_->ForceSync();
        }
        if (truck_bridge_) {
            truck_bridge_->ForceSync();
        }
    }

    bool IsConverged() {
        const auto cloud_checksum = Checksum(*cloud_adapter_);
        const auto truck_checksum = Checksum(*truck_adapter_);
        if (cloud_checksum != truck_checksum) {
            return false;
        }

        return DirtyCount(*cloud_adapter_, CloudSession()) == 0U &&
               DirtyCount(*truck_adapter_, TruckSession()) == 0U;
    }

    bool WaitForConvergence(std::chrono::seconds timeout = 20s) {
        ForceSyncBoth();
        return WaitFor([&]() { return IsConverged(); }, timeout);
    }

    static sync::SyncSessionKey InspectSession() {
        sync::SyncSessionKey key;
        key.local_node_id = "inspect";
        key.remote_node_id = "snapshot";
        key.namespace_name = "shared";
        return key;
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

    bool SendRawPayloadToTruck(const std::vector<std::uint8_t>& payload) {
        const auto result = cloud_transport_client_->SendToVehicle(
            kVehicleId,
            payload,
            swdv::cloud_backend_transport_service::persistence_t::VOLATILE);
        return static_cast<int>(result.status()) == 0;
    }

    bool SendRawEnvelopeToTruck(const swdv::cloud_vehicle_sync_envelope::CloudVehicleSyncEnvelope& envelope) {
        std::string serialized;
        if (!envelope.SerializeToString(&serialized)) {
            return false;
        }
        return SendRawPayloadToTruck(std::vector<std::uint8_t>(serialized.begin(), serialized.end()));
    }

    static void PopulateProtoRecord(const sync::CanonicalRecord& record,
                                    swdv::cloud_vehicle_sync_envelope::RecordEnvelope* proto) {
        proto->mutable_locator()->set_namespace_name(record.locator.namespace_name);
        proto->mutable_locator()->set_origin_node_id(record.locator.origin_node_id);
        proto->mutable_locator()->set_record_id(
            std::string(record.locator.record_id.begin(), record.locator.record_id.end()));
        proto->mutable_version_vector()->set_cloud_seq(record.version_vector.cloud_seq);
        proto->mutable_version_vector()->set_truck_seq(record.version_vector.truck_seq);
        switch (record.operation) {
            case sync::RecordOperation::kCreate:
                proto->set_operation(swdv::cloud_vehicle_sync_envelope::RECORD_OPERATION_CREATE);
                break;
            case sync::RecordOperation::kUpdate:
                proto->set_operation(swdv::cloud_vehicle_sync_envelope::RECORD_OPERATION_UPDATE);
                break;
            case sync::RecordOperation::kDelete:
                proto->set_operation(swdv::cloud_vehicle_sync_envelope::RECORD_OPERATION_DELETE);
                break;
        }
        proto->set_payload(std::string(record.payload.begin(), record.payload.end()));
        proto->set_schema_version(record.schema_version);
        proto->set_idempotency_key(record.idempotency_key);
        proto->set_correlation_id(record.correlation_id);
        proto->set_updated_at_ms(record.updated_at_ms);
        proto->set_tombstone_at_ms(record.tombstone_at_ms);
        proto->set_tombstone_reason(record.tombstone_reason);
    }

    static void PopulateProtoAck(const sync::VersionAck& ack,
                                 swdv::cloud_vehicle_sync_envelope::VersionAck* proto) {
        proto->mutable_locator()->set_namespace_name(ack.locator.namespace_name);
        proto->mutable_locator()->set_origin_node_id(ack.locator.origin_node_id);
        proto->mutable_locator()->set_record_id(
            std::string(ack.locator.record_id.begin(), ack.locator.record_id.end()));
        proto->mutable_version_vector()->set_cloud_seq(ack.version_vector.cloud_seq);
        proto->mutable_version_vector()->set_truck_seq(ack.version_vector.truck_seq);
        proto->set_correlation_id(ack.correlation_id);
        proto->set_idempotency_key(ack.idempotency_key);
    }

    static void PopulateProtoCheckpoint(const sync::CheckpointToken& checkpoint,
                                        swdv::cloud_vehicle_sync_envelope::CheckpointToken* proto) {
        proto->set_sequence_number(checkpoint.sequence_number);
        proto->mutable_last_record()->set_namespace_name(checkpoint.last_record.namespace_name);
        proto->mutable_last_record()->set_origin_node_id(checkpoint.last_record.origin_node_id);
        proto->mutable_last_record()->set_record_id(
            std::string(checkpoint.last_record.record_id.begin(), checkpoint.last_record.record_id.end()));
        proto->mutable_last_version()->set_cloud_seq(checkpoint.last_version.cloud_seq);
        proto->mutable_last_version()->set_truck_seq(checkpoint.last_version.truck_seq);
    }
};

TEST_F(CloudVehicleSyncBridgeIntegrationTest, CloudAndTruckOriginatedRowsConvergeWithExactCounts) {
    ASSERT_TRUE(StartCloudBridge());
    ASSERT_TRUE(StartTruckBridge());

    ApplyCloudLocal(MakeRecord("cloud-row-1", "cloud", 1, 0, "payload-cloud-1", "idem-cloud-row-1"));
    ApplyTruckLocal(MakeRecord("truck-row-1", kTruckNodeId, 0, 1, "payload-truck-1", "idem-truck-row-1"));

    ASSERT_TRUE(WaitForConvergence(30s));

    EXPECT_EQ(RecordCount(*cloud_adapter_), 2U);
    EXPECT_EQ(RecordCount(*truck_adapter_), 2U);
    EXPECT_EQ(Checksum(*cloud_adapter_), Checksum(*truck_adapter_));
    EXPECT_EQ(ConflictCount(*cloud_adapter_), 0U);
    EXPECT_EQ(ConflictCount(*truck_adapter_), 0U);
    EXPECT_EQ(DirtyCount(*cloud_adapter_, CloudSession()), 0U);
    EXPECT_EQ(DirtyCount(*truck_adapter_, TruckSession()), 0U);
    EXPECT_TRUE(HasRecord(*cloud_adapter_, "cloud-row-1"));
    EXPECT_TRUE(HasRecord(*cloud_adapter_, "truck-row-1"));
    EXPECT_TRUE(HasRecord(*truck_adapter_, "cloud-row-1"));
    EXPECT_TRUE(HasRecord(*truck_adapter_, "truck-row-1"));
}

TEST_F(CloudVehicleSyncBridgeIntegrationTest, OfflineConcurrentUpdatesPersistOneConflictPerSide) {
    ASSERT_TRUE(StartCloudBridge());
    ASSERT_TRUE(StartTruckBridge());

    ApplyCloudLocal(MakeRecord("shared-conflict", "cloud", 1, 0, "base-value", "idem-shared-conflict-base"));
    ASSERT_TRUE(WaitForConvergence(30s));

    StopTruckTransport();
    std::this_thread::sleep_for(500ms);

    ApplyCloudLocal(MakeRecord("shared-conflict", "cloud", 2, 0, "cloud-offline-value", "idem-shared-conflict-cloud"));
    ApplyTruckLocal(MakeRecord("shared-conflict", "cloud", 1, 1, "truck-offline-value", "idem-shared-conflict-truck"));

    ASSERT_TRUE(StartTruckTransport());
    ForceSyncBoth();

    ASSERT_TRUE(WaitFor([&]() {
        return ConflictCount(*cloud_adapter_) == 1U && ConflictCount(*truck_adapter_) == 1U;
    }, 30s));

    const auto cloud_conflicts = Conflicts(*cloud_adapter_);
    const auto truck_conflicts = Conflicts(*truck_adapter_);
    sync::CanonicalRecord cloud_row;
    sync::CanonicalRecord truck_row;

    EXPECT_EQ(RecordCount(*cloud_adapter_), 1U);
    EXPECT_EQ(RecordCount(*truck_adapter_), 1U);
    ASSERT_TRUE(LoadRecord(*cloud_adapter_, InspectSession(), "shared-conflict", &cloud_row));
    ASSERT_TRUE(LoadRecord(*truck_adapter_, InspectSession(), "shared-conflict", &truck_row));
    ASSERT_EQ(cloud_conflicts.size(), 1U);
    ASSERT_EQ(truck_conflicts.size(), 1U);
    EXPECT_EQ(std::string(cloud_row.payload.begin(), cloud_row.payload.end()), "cloud-offline-value");
    EXPECT_EQ(cloud_row.version_vector.cloud_seq, 2U);
    EXPECT_EQ(cloud_row.version_vector.truck_seq, 0U);
    EXPECT_EQ(std::string(truck_row.payload.begin(), truck_row.payload.end()), "truck-offline-value");
    EXPECT_EQ(truck_row.version_vector.cloud_seq, 1U);
    EXPECT_EQ(truck_row.version_vector.truck_seq, 1U);
    EXPECT_EQ(cloud_conflicts[0].conflict_class, sync::ConflictClass::kConcurrentUpdate);
    EXPECT_EQ(truck_conflicts[0].conflict_class, sync::ConflictClass::kConcurrentUpdate);
    EXPECT_EQ(std::string(cloud_conflicts[0].local_payload.begin(), cloud_conflicts[0].local_payload.end()),
              "cloud-offline-value");
    EXPECT_EQ(std::string(cloud_conflicts[0].remote_payload.begin(), cloud_conflicts[0].remote_payload.end()),
              "truck-offline-value");
    EXPECT_EQ(std::string(truck_conflicts[0].local_payload.begin(), truck_conflicts[0].local_payload.end()),
              "truck-offline-value");
    EXPECT_EQ(std::string(truck_conflicts[0].remote_payload.begin(), truck_conflicts[0].remote_payload.end()),
              "cloud-offline-value");
    EXPECT_GE(cloud_bridge_->GetStats().records_conflicted, 1U);
    EXPECT_GE(truck_bridge_->GetStats().records_conflicted, 1U);
}

TEST_F(CloudVehicleSyncBridgeIntegrationTest, DeleteVsModifyConvergesToSingleVisibleTombstone) {
    ASSERT_TRUE(StartCloudBridge());
    ASSERT_TRUE(StartTruckBridge());

    ApplyCloudLocal(MakeRecord("delete-vs-modify", "cloud", 1, 0, "base-delete-value", "idem-delete-vs-modify-base"));
    ASSERT_TRUE(WaitForConvergence(30s));

    StopTruckTransport();
    std::this_thread::sleep_for(500ms);

    ApplyCloudLocal(MakeTombstone("delete-vs-modify", "cloud", 2, 1, "idem-delete-vs-modify-delete", "cloud delete"));
    ApplyTruckLocal(MakeRecord("delete-vs-modify", "cloud", 1, 1, "truck-offline-edit", "idem-delete-vs-modify-truck"));

    ASSERT_TRUE(StartTruckTransport());
    ASSERT_TRUE(WaitForConvergence(30s));

    const auto cloud_conflicts = Conflicts(*cloud_adapter_);
    const auto truck_conflicts = Conflicts(*truck_adapter_);
    sync::CanonicalRecord cloud_row;
    sync::CanonicalRecord truck_row;

    EXPECT_EQ(RecordCount(*cloud_adapter_), 1U);
    EXPECT_EQ(RecordCount(*truck_adapter_), 1U);
    EXPECT_EQ(Checksum(*cloud_adapter_), Checksum(*truck_adapter_));
    ASSERT_TRUE(LoadRecord(*cloud_adapter_, InspectSession(), "delete-vs-modify", &cloud_row));
    ASSERT_TRUE(LoadRecord(*truck_adapter_, InspectSession(), "delete-vs-modify", &truck_row));
    ASSERT_EQ(cloud_conflicts.size(), 1U);
    EXPECT_TRUE(truck_conflicts.empty());
    EXPECT_EQ(cloud_row.operation, sync::RecordOperation::kDelete);
    EXPECT_EQ(truck_row.operation, sync::RecordOperation::kDelete);
    EXPECT_TRUE(cloud_row.payload.empty());
    EXPECT_TRUE(truck_row.payload.empty());
    EXPECT_EQ(cloud_row.version_vector.cloud_seq, 2U);
    EXPECT_EQ(cloud_row.version_vector.truck_seq, 1U);
    EXPECT_EQ(truck_row.version_vector.cloud_seq, 2U);
    EXPECT_EQ(truck_row.version_vector.truck_seq, 1U);
    EXPECT_EQ(cloud_conflicts[0].conflict_class, sync::ConflictClass::kStaleReplay);
    EXPECT_EQ(TombstoneCountForGc(*cloud_adapter_, CloudSession()), 1U);
    EXPECT_EQ(TombstoneCountForGc(*truck_adapter_, TruckSession()), 1U);
}

TEST_F(CloudVehicleSyncBridgeIntegrationTest, SenderMismatchEnvelopeIsIgnoredBeforeApply) {
    ASSERT_TRUE(StartCloudBridge());
    ASSERT_TRUE(StartTruckBridge());

    const auto stats_before = truck_bridge_->GetStats();
    const sync::CanonicalRecord forged =
        MakeRecord("forged-sender", "cloud", 3, 0, "ignored-payload", "idem-forged-sender");

    swdv::cloud_vehicle_sync_envelope::CloudVehicleSyncEnvelope envelope;
    auto* sync = envelope.mutable_sync_exchange();
    sync->set_sender_node_id("unexpected-peer");
    sync->set_recipient_node_id(kTruckNodeId);
    sync->set_state_checksum(Checksum(*cloud_adapter_));
    sync->set_correlation_id("forged-sender-corr");
    sync->set_idempotency_key("forged-sender-msg");
    PopulateProtoRecord(forged, sync->add_records());

    ASSERT_TRUE(SendRawEnvelopeToTruck(envelope));
    std::this_thread::sleep_for(1500ms);

    EXPECT_FALSE(HasRecord(*truck_adapter_, "forged-sender"));
    EXPECT_EQ(ConflictCount(*truck_adapter_), 0U);
    EXPECT_EQ(truck_bridge_->GetStats().records_applied, stats_before.records_applied);
    EXPECT_EQ(truck_bridge_->GetStats().records_conflicted, stats_before.records_conflicted);
}

TEST_F(CloudVehicleSyncBridgeIntegrationTest, MultipleReconnectCyclesRemainConvergedWithExactCounts) {
    ASSERT_TRUE(StartCloudBridge());
    ASSERT_TRUE(StartTruckBridge());

    for (std::uint64_t cycle = 1; cycle <= 3; ++cycle) {
        StopTruckTransport();
        std::this_thread::sleep_for(400ms);

        ApplyCloudLocal(MakeRecord("cycle-cloud-" + std::to_string(cycle),
                                   "cloud",
                                   cycle,
                                   0,
                                   "cloud-cycle-payload-" + std::to_string(cycle),
                                   "idem-cycle-cloud-" + std::to_string(cycle)));
        ApplyTruckLocal(MakeRecord("cycle-truck-" + std::to_string(cycle),
                                   kTruckNodeId,
                                   0,
                                   cycle,
                                   "truck-cycle-payload-" + std::to_string(cycle),
                                   "idem-cycle-truck-" + std::to_string(cycle)));

        ASSERT_TRUE(StartTruckTransport());
        ASSERT_TRUE(WaitForConvergence(30s));

        const std::size_t expected_count = static_cast<std::size_t>(cycle * 2);
        EXPECT_EQ(RecordCount(*cloud_adapter_), expected_count);
        EXPECT_EQ(RecordCount(*truck_adapter_), expected_count);
        EXPECT_EQ(Checksum(*cloud_adapter_), Checksum(*truck_adapter_));
    }

    EXPECT_EQ(ConflictCount(*cloud_adapter_), 0U);
    EXPECT_EQ(ConflictCount(*truck_adapter_), 0U);
}

TEST_F(CloudVehicleSyncBridgeIntegrationTest, DuplicateDeliveryIsIdempotent) {
    ASSERT_TRUE(StartCloudBridge());
    ASSERT_TRUE(StartTruckBridge());

    const sync::CanonicalRecord duplicate_record =
        MakeRecord("dup-1", "cloud", 10, 0, "dup-payload", "dup-record-key");
    ApplyCloudLocal(duplicate_record);

    swdv::cloud_vehicle_sync_envelope::CloudVehicleSyncEnvelope envelope;
    auto* sync = envelope.mutable_sync_exchange();
    sync->set_sender_node_id("cloud");
    sync->set_recipient_node_id(kTruckNodeId);
    sync->set_state_checksum(Checksum(*cloud_adapter_));
    sync->set_correlation_id("dup-corr");
    sync->set_idempotency_key("dup-msg");
    PopulateProtoRecord(duplicate_record, sync->add_records());

    ASSERT_TRUE(SendRawEnvelopeToTruck(envelope));
    ASSERT_TRUE(SendRawEnvelopeToTruck(envelope));
    ASSERT_TRUE(WaitForConvergence(30s));

    EXPECT_EQ(RecordCount(*cloud_adapter_), 1U);
    EXPECT_EQ(RecordCount(*truck_adapter_), 1U);
    EXPECT_EQ(Checksum(*cloud_adapter_), Checksum(*truck_adapter_));
    EXPECT_EQ(ConflictCount(*cloud_adapter_), 0U);
    EXPECT_EQ(ConflictCount(*truck_adapter_), 0U);
    EXPECT_GE(truck_bridge_->GetStats().records_duplicated, 1U);
}

TEST_F(CloudVehicleSyncBridgeIntegrationTest, StaleAckReplayLeavesCheckpointMonotonicAndIgnored) {
    ASSERT_TRUE(StartCloudBridge());
    ASSERT_TRUE(StartTruckBridge());

    const sync::CanonicalRecord first_record =
        MakeRecord("stale-ack-1", kTruckNodeId, 0, 1, "truck-ack-payload-1", "idem-stale-ack-1");
    ApplyTruckLocal(first_record);

    ASSERT_TRUE(WaitForConvergence(30s));
    const sync::CanonicalRecord second_version =
        MakeRecord("stale-ack-1", kTruckNodeId, 0, 2, "truck-ack-payload-2", "idem-stale-ack-1-v2");
    ApplyTruckLocal(second_version);
    ASSERT_TRUE(WaitForConvergence(30s));

    ASSERT_TRUE(WaitFor([&]() {
        const auto checkpoint = ReadCheckpoint(*truck_adapter_, TruckSession());
        return checkpoint.found && checkpoint.checkpoint.sequence_number > 0;
    }, 30s));

    const auto checkpoint_before = ReadCheckpoint(*truck_adapter_, TruckSession());
    ASSERT_TRUE(checkpoint_before.found);

    StopTruckBridge();
    std::this_thread::sleep_for(500ms);
    ASSERT_TRUE(StartTruckBridge());

    const auto stats_after_restart = truck_bridge_->GetStats();

    sync::VersionAck ack;
    ack.locator = second_version.locator;
    ack.version_vector = first_record.version_vector;
    ack.correlation_id = "stale-ack-corr";
    ack.idempotency_key = "stale-ack-msg";

    sync::CheckpointToken stale_checkpoint = checkpoint_before.checkpoint;
    if (stale_checkpoint.sequence_number > 0) {
        stale_checkpoint.sequence_number--;
    }

    swdv::cloud_vehicle_sync_envelope::CloudVehicleSyncEnvelope replay_envelope;
    auto* advance = replay_envelope.mutable_checkpoint_advance();
    advance->set_sender_node_id("cloud");
    advance->set_recipient_node_id(kTruckNodeId);
    advance->set_state_checksum(Checksum(*cloud_adapter_));
    advance->set_correlation_id("stale-ack-corr");
    advance->set_idempotency_key("stale-ack-msg");
    PopulateProtoCheckpoint(stale_checkpoint, advance->mutable_durable_checkpoint());
    PopulateProtoAck(ack, advance->add_durable_acks());

    ASSERT_TRUE(SendRawEnvelopeToTruck(replay_envelope));
    ASSERT_TRUE(WaitFor([&]() {
        return truck_bridge_->GetStats().checkpoint_messages_received >=
               stats_after_restart.checkpoint_messages_received + 1;
    }, 10s));

    const auto checkpoint_after_replay = ReadCheckpoint(*truck_adapter_, TruckSession());
    ASSERT_TRUE(checkpoint_after_replay.found);
    EXPECT_EQ(checkpoint_after_replay.checkpoint.sequence_number,
              checkpoint_before.checkpoint.sequence_number);
    EXPECT_EQ(checkpoint_after_replay.checkpoint.last_record.record_id,
              checkpoint_before.checkpoint.last_record.record_id);
    EXPECT_EQ(checkpoint_after_replay.checkpoint.last_version.cloud_seq,
              checkpoint_before.checkpoint.last_version.cloud_seq);
    EXPECT_EQ(checkpoint_after_replay.checkpoint.last_version.truck_seq,
              checkpoint_before.checkpoint.last_version.truck_seq);
    EXPECT_EQ(truck_bridge_->GetStats().checkpoint_messages_sent,
              stats_after_restart.checkpoint_messages_sent);

    ApplyTruckLocal(MakeRecord("stale-ack-2",
                                kTruckNodeId,
                                0,
                                3,
                                "truck-ack-payload-3",
                                "idem-stale-ack-2"));

    ASSERT_TRUE(WaitForConvergence(30s));
    ASSERT_TRUE(WaitFor([&]() {
        const auto checkpoint = ReadCheckpoint(*truck_adapter_, TruckSession());
        return checkpoint.found &&
               checkpoint.checkpoint.sequence_number > checkpoint_before.checkpoint.sequence_number;
    }, 30s));
    EXPECT_EQ(Checksum(*cloud_adapter_), Checksum(*truck_adapter_));
}

TEST_F(CloudVehicleSyncBridgeIntegrationTest, GapRecoveryRequestsAndFetchesMissingRecords) {
    ApplyCloudLocal(MakeRecord("gap-1", "cloud", 30, 0, "gap-payload", "idem-gap-1"));

    sync::VersionAck gap_ack;
    gap_ack.locator.namespace_name = "shared";
    gap_ack.locator.origin_node_id = "cloud";
    gap_ack.locator.record_id.assign({'g', 'a', 'p', '-', '1'});
    gap_ack.version_vector.cloud_seq = 30;
    gap_ack.version_vector.truck_seq = 0;
    cloud_adapter_->persist_remote_acks(CloudSession(), {gap_ack});
    EXPECT_EQ(cloud_adapter_->list_remote_acks(CloudSession()).size(), 1U);

    sync::CheckpointToken cloud_checkpoint;
    cloud_checkpoint.sequence_number = 1;
    cloud_checkpoint.last_record.namespace_name = "shared";
    cloud_checkpoint.last_record.origin_node_id = "cloud";
    cloud_checkpoint.last_record.record_id.assign({'g', 'a', 'p', '-', '1'});
    cloud_checkpoint.last_version.cloud_seq = 30;
    cloud_checkpoint.last_version.truck_seq = 0;
    cloud_adapter_->write_checkpoint(CloudSession(), cloud_checkpoint);

    EXPECT_EQ(DirtyCount(*cloud_adapter_, CloudSession()), 0U);
    EXPECT_EQ(DirtyCount(*truck_adapter_, TruckSession()), 0U);
    EXPECT_NE(Checksum(*cloud_adapter_), Checksum(*truck_adapter_));

    ASSERT_TRUE(StartCloudBridge());
    ASSERT_TRUE(StartTruckBridge());

    ASSERT_TRUE(WaitForConvergence(30s));

    const auto cloud_stats = cloud_bridge_->GetStats();
    const auto truck_stats = truck_bridge_->GetStats();
    EXPECT_EQ(RecordCount(*cloud_adapter_), 1U);
    EXPECT_EQ(RecordCount(*truck_adapter_), 1U);
    EXPECT_EQ(Checksum(*cloud_adapter_), Checksum(*truck_adapter_));
    EXPECT_EQ(ConflictCount(*cloud_adapter_), 0U);
    EXPECT_EQ(ConflictCount(*truck_adapter_), 0U);
    EXPECT_GE(cloud_stats.gap_requests_sent + truck_stats.gap_requests_sent, 1U);
    EXPECT_GE(cloud_stats.gap_responses_received + truck_stats.gap_responses_received, 1U);
}

TEST_F(CloudVehicleSyncBridgeIntegrationTest, MalformedEnvelopeIsRejectedWithoutStateCorruption) {
    ASSERT_TRUE(StartCloudBridge());
    ASSERT_TRUE(StartTruckBridge());

    ApplyTruckLocal(MakeRecord("malformed-safe-baseline",
                               kTruckNodeId,
                               0,
                               1,
                               "baseline-payload",
                               "idem-malformed-safe-baseline"));

    ASSERT_TRUE(WaitForConvergence(30s));
    ASSERT_TRUE(WaitFor([&]() {
        const auto checkpoint = ReadCheckpoint(*truck_adapter_, TruckSession());
        return checkpoint.found && checkpoint.checkpoint.sequence_number > 0;
    }, 30s));

    const std::uint64_t checksum_before = Checksum(*truck_adapter_);
    const std::size_t row_count_before = RecordCount(*truck_adapter_);
    const std::size_t conflict_count_before = ConflictCount(*truck_adapter_);
    const auto checkpoint_before = ReadCheckpoint(*truck_adapter_, TruckSession());
    const auto stats_before = truck_bridge_->GetStats();

    ASSERT_TRUE(SendRawPayloadToTruck({0xff, 0xff, 0xff, 0xff}));
    std::this_thread::sleep_for(1500ms);

    const auto checkpoint_after = ReadCheckpoint(*truck_adapter_, TruckSession());
    const auto stats_after = truck_bridge_->GetStats();

    EXPECT_EQ(Checksum(*truck_adapter_), checksum_before);
    EXPECT_EQ(RecordCount(*truck_adapter_), row_count_before);
    EXPECT_EQ(ConflictCount(*truck_adapter_), conflict_count_before);
    EXPECT_TRUE(checkpoint_after.found);
    EXPECT_EQ(checkpoint_after.checkpoint.sequence_number, checkpoint_before.checkpoint.sequence_number);
    EXPECT_EQ(checkpoint_after.checkpoint.last_record.record_id,
              checkpoint_before.checkpoint.last_record.record_id);
    EXPECT_EQ(stats_after.sync_messages_received, stats_before.sync_messages_received);
    EXPECT_EQ(stats_after.checkpoint_messages_received, stats_before.checkpoint_messages_received);
    EXPECT_EQ(stats_after.gap_requests_received, stats_before.gap_requests_received);
    EXPECT_EQ(stats_after.gap_responses_received, stats_before.gap_responses_received);
    EXPECT_EQ(Checksum(*cloud_adapter_), Checksum(*truck_adapter_));
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
