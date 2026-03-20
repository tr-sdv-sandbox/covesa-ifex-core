#include "../include/sqlite_cloud_vehicle_db_adapter.hpp"

#if __has_include(<gtest/gtest.h>)
#  include <gtest/gtest.h>
#else
#  include "../../testing/tests/gtest_fallback.h"
#endif

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

namespace ifex {
namespace sync {
namespace {

ByteBuffer bytes(const char* value) {
    ByteBuffer out;
    while (*value != '\0') {
        out.push_back(static_cast<std::uint8_t>(*value));
        ++value;
    }
    return out;
}

CanonicalRecord make_record(const char* record_id,
                            const char* namespace_name,
                            const char* origin_node_id,
                            VersionVector version,
                            RecordOperation operation,
                            const char* payload,
                            const char* correlation_id = "corr") {
    CanonicalRecord record;
    record.locator.record_id = bytes(record_id);
    record.locator.namespace_name = namespace_name;
    record.locator.origin_node_id = origin_node_id;
    record.version_vector = version;
    record.operation = operation;
    record.payload = bytes(payload);
    record.schema_version = 1;
    record.correlation_id = correlation_id;
    return record;
}

std::filesystem::path unique_db_path(const char* suffix) {
    const auto stamp = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    return std::filesystem::temp_directory_path() /
           (std::string("ifex-sync-database-adapter-") + suffix + "-" +
            std::to_string(stamp) + ".sqlite");
}

DatabaseAdapterConfig make_config(const std::filesystem::path& db_path) {
    DatabaseAdapterConfig config;
    config.database_path = db_path.string();
    config.namespace_owners = {
        {"planning", RecordOwner::kCloud},
        {"facts", RecordOwner::kTruck},
        {"shared", RecordOwner::kShared},
    };
    return config;
}

TEST(DatabaseAdapterContract, DirtyEnumerationUsesPerSessionAcks) {
    const std::filesystem::path db_path = unique_db_path("dirty");
    std::filesystem::remove(db_path);

    {
        SqliteCloudVehicleDbAdapter adapter(make_config(db_path));
        SyncSessionKey session{"cloud", "truck-001", "planning"};

        CanonicalRecord first = make_record("job-1", "planning", "cloud", {1, 0},
                                            RecordOperation::kCreate, "payload-1");
        first.wall_clock_ms = 10;
        first.updated_at_ms = 10;
        CanonicalRecord second = make_record("job-2", "planning", "cloud", {2, 0},
                                             RecordOperation::kUpdate, "payload-2");
        second.wall_clock_ms = 999999;
        second.updated_at_ms = 999999;

        EXPECT_EQ(adapter.apply_record(first, "idem-job-1").disposition,
                  ApplyDisposition::kApplied);
        EXPECT_EQ(adapter.apply_record(second, "idem-job-2").disposition,
                  ApplyDisposition::kApplied);

        CheckpointToken checkpoint;
        checkpoint.sequence_number = 1;
        checkpoint.last_record = first.locator;
        checkpoint.last_version = first.version_vector;
        adapter.write_checkpoint(session, checkpoint);

        const std::vector<CanonicalRecord> dirty =
            adapter.list_dirty_records({session, 10, true});
        EXPECT_EQ(dirty.size(), 1U);
        if (dirty.size() == 1U) {
            EXPECT_EQ(dirty[0].locator.record_id, bytes("job-2"));
            EXPECT_EQ(dirty[0].version_vector.cloud_seq, 2U);
        }

        const std::vector<CanonicalRecord> dirty_again =
            adapter.list_dirty_records({session, 10, true});
        EXPECT_EQ(dirty_again.size(), dirty.size());
        if (!dirty.empty() && !dirty_again.empty()) {
            EXPECT_EQ(dirty_again[0].locator.record_id, dirty[0].locator.record_id);
        }
    }

    std::filesystem::remove(db_path);
}

TEST(DatabaseAdapterContract, ReplaySafeApplyKeepsSingleLogicalRowAndCheckpointMonotonic) {
    const std::filesystem::path db_path = unique_db_path("replay");
    std::filesystem::remove(db_path);

    {
        SqliteCloudVehicleDbAdapter adapter(make_config(db_path));
        SyncSessionKey session{"truck-001", "cloud", "planning"};
        const CanonicalRecord record = make_record("job-9", "planning", "cloud", {5, 0},
                                                   RecordOperation::kUpdate, "cfg");

        EXPECT_EQ(adapter.apply_record(record, "idem-replay").disposition,
                  ApplyDisposition::kApplied);
        EXPECT_EQ(adapter.apply_record(record, "idem-replay").disposition,
                  ApplyDisposition::kDuplicate);

        const std::vector<RecordLocator> ids =
            adapter.list_record_ids({"planning", true, 0});
        EXPECT_EQ(ids.size(), 1U);

        CheckpointToken high;
        high.sequence_number = 9;
        high.last_record = record.locator;
        high.last_version = record.version_vector;
        adapter.write_checkpoint(session, high);

        CheckpointToken low = high;
        low.sequence_number = 3;
        low.last_version = {1, 0};
        adapter.write_checkpoint(session, low);

        const CheckpointReadResult stored = adapter.read_checkpoint(session);
        EXPECT_TRUE(stored.found);
        if (stored.found) {
            EXPECT_EQ(stored.checkpoint.sequence_number, 9U);
            EXPECT_EQ(stored.checkpoint.last_version.cloud_seq, 5U);
        }
    }

    std::filesystem::remove(db_path);
}

TEST(DatabaseAdapterContract, ChecksumDependsOnLogicalStateOnly) {
    const std::filesystem::path first_db_path = unique_db_path("checksum-a");
    const std::filesystem::path second_db_path = unique_db_path("checksum-b");
    std::filesystem::remove(first_db_path);
    std::filesystem::remove(second_db_path);

    std::uint64_t checksum_a = 0;
    std::uint64_t checksum_b = 0;

    {
        SqliteCloudVehicleDbAdapter first_adapter(make_config(first_db_path));
        CanonicalRecord first = make_record("plan-1", "planning", "cloud", {7, 0},
                                            RecordOperation::kUpdate, "payload");
        first.wall_clock_ms = 100;
        first.updated_at_ms = 100;
        first.idempotency_key = "logical-a";
        first.correlation_id = "corr-a";
        first.payload_checksum = 11;
        first_adapter.apply_record(first, "logical-a");
        checksum_a = first_adapter.compute_state_checksum({"planning", true});
    }

    {
        SqliteCloudVehicleDbAdapter second_adapter(make_config(second_db_path));
        CanonicalRecord second = make_record("plan-1", "planning", "cloud", {7, 0},
                                             RecordOperation::kUpdate, "payload");
        second.wall_clock_ms = 999999;
        second.updated_at_ms = 999999;
        second.idempotency_key = "logical-b";
        second.correlation_id = "corr-b";
        second.payload_checksum = 99;
        second_adapter.apply_record(second, "logical-b");
        checksum_b = second_adapter.compute_state_checksum({"planning", true});
    }

    EXPECT_EQ(checksum_a, checksum_b);

    std::filesystem::remove(first_db_path);
    std::filesystem::remove(second_db_path);
}

TEST(DatabaseAdapterContract, TombstoneGcRequiresAckAndRetentionCutoff) {
    const std::filesystem::path db_path = unique_db_path("tombstone");
    std::filesystem::remove(db_path);

    {
        SqliteCloudVehicleDbAdapter adapter(make_config(db_path));
        SyncSessionKey session{"truck-001", "cloud", "facts"};
        CanonicalRecord tombstone = make_record("fact-1", "facts", "truck-001", {0, 4},
                                                RecordOperation::kDelete, "");
        tombstone.tombstone_at_ms = 500;

        EXPECT_EQ(adapter.apply_record(tombstone, "idem-tomb").disposition,
                  ApplyDisposition::kApplied);
        EXPECT_EQ(adapter.list_tombstones_for_gc({session, 1000, 10}).size(), 0U);

        CheckpointToken checkpoint;
        checkpoint.sequence_number = 1;
        checkpoint.last_record = tombstone.locator;
        checkpoint.last_version = tombstone.version_vector;
        adapter.write_checkpoint(session, checkpoint);

        EXPECT_EQ(adapter.list_tombstones_for_gc({session, 400, 10}).size(), 0U);
        EXPECT_EQ(adapter.list_tombstones_for_gc({session, 500, 10}).size(), 1U);
    }

    std::filesystem::remove(db_path);
}

TEST(DatabaseAdapterConflict, SharedRowConflictIsPersistedDurablyAndReplaySafe) {
    const std::filesystem::path db_path = unique_db_path("conflict");
    std::filesystem::remove(db_path);

    {
        SqliteCloudVehicleDbAdapter adapter(make_config(db_path));
        const CanonicalRecord local = make_record("shared-1", "shared", "cloud", {3, 1},
                                                  RecordOperation::kUpdate, "left");
        CanonicalRecord remote = make_record("shared-1", "shared", "cloud", {2, 2},
                                             RecordOperation::kUpdate, "right", "corr-shared");
        remote.updated_at_ms = 42;

        EXPECT_EQ(adapter.apply_record(local, "idem-local").disposition,
                  ApplyDisposition::kApplied);

        const ApplyResult first_conflict = adapter.apply_record(remote, "idem-conflict");
        EXPECT_EQ(first_conflict.disposition, ApplyDisposition::kConflictPersisted);
        EXPECT_TRUE(first_conflict.has_persisted_conflict);
        if (first_conflict.has_persisted_conflict) {
            EXPECT_EQ(first_conflict.persisted_conflict.conflict_class,
                      ConflictClass::kConcurrentUpdate);
        }

        const ApplyResult replay = adapter.apply_record(remote, "idem-conflict");
        EXPECT_EQ(replay.disposition, ApplyDisposition::kDuplicate);

        const std::vector<ConflictRecord> conflicts =
            adapter.query_conflicts({"shared", 0, false, 10});
        EXPECT_EQ(conflicts.size(), 1U);
        if (conflicts.size() == 1U) {
            EXPECT_EQ(conflicts[0].local_payload, bytes("left"));
            EXPECT_EQ(conflicts[0].remote_payload, bytes("right"));
            EXPECT_EQ(conflicts[0].local_version.cloud_seq, 3U);
            EXPECT_EQ(conflicts[0].remote_version.truck_seq, 2U);
        }

        const std::vector<RecordLocator> ids = adapter.list_record_ids({"shared", true, 0});
        EXPECT_EQ(ids.size(), 1U);
    }

    std::filesystem::remove(db_path);
}

TEST(DatabaseAdapterConflict, OwnedNamespacesRejectNonOwnerMutationsDurably) {
    const std::filesystem::path db_path = unique_db_path("ownership");
    std::filesystem::remove(db_path);

    {
        SqliteCloudVehicleDbAdapter adapter(make_config(db_path));
        CanonicalRecord invalid_planning = make_record("plan-2", "planning", "truck-002", {0, 1},
                                                       RecordOperation::kUpdate, "truck-change");

        const ApplyResult planning_result = adapter.apply_record(invalid_planning, "idem-owner");
        EXPECT_EQ(planning_result.disposition, ApplyDisposition::kNonOwnerRejected);
        EXPECT_TRUE(planning_result.has_persisted_conflict);

        const std::vector<ConflictRecord> conflicts =
            adapter.query_conflicts({"planning", 0, false, 10});
        EXPECT_EQ(conflicts.size(), 1U);
        if (conflicts.size() == 1U) {
            EXPECT_EQ(conflicts[0].conflict_class, ConflictClass::kNonOwnerMutation);
            EXPECT_EQ(conflicts[0].remote_payload, bytes("truck-change"));
        }

        EXPECT_EQ(adapter.list_record_ids({"planning", true, 0}).size(), 0U);
    }

    std::filesystem::remove(db_path);
}

}
}
}
