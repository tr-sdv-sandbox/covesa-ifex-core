#include "../include/cloud_vehicle_sync_core.hpp"

#if __has_include(<gtest/gtest.h>)
#  include <gtest/gtest.h>
#else
#  include "../../adapters/testing/tests/gtest_fallback.h"
#endif

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
                            const char* ns,
                            const char* origin,
                            VersionVector version,
                            RecordOperation op,
                            const char* payload,
                            const char* correlation_id = "corr") {
    CanonicalRecord record;
    record.locator.record_id = bytes(record_id);
    record.locator.namespace_name = ns;
    record.locator.origin_node_id = origin;
    record.version_vector = version;
    record.operation = op;
    record.payload = bytes(payload);
    record.schema_version = 1;
    record.correlation_id = correlation_id;
    return record;
}

VersionAck make_ack(const CanonicalRecord& record) {
    VersionAck ack;
    ack.locator = record.locator;
    ack.version_vector = record.version_vector;
    ack.correlation_id = record.correlation_id;
    ack.idempotency_key = "ack-key";
    return ack;
}

TEST(SyncCoreTest, DuplicateApplyReplay) {
    const CanonicalRecord local = make_record("id-1", "jobs", "cloud", {2, 1}, RecordOperation::kUpdate, "payload-a");
    const CanonicalRecord remote = make_record("id-1", "jobs", "cloud", {2, 1}, RecordOperation::kUpdate, "payload-a");

    const ResolveOutcome outcome = CloudVehicleSyncCore::resolve_remote_record(
        remote,
        &local,
        RecordOwner::kShared,
        "cloud",
        1000);

    EXPECT_EQ(outcome.disposition, ApplyDisposition::kDuplicate);
    EXPECT_FALSE(outcome.should_apply);
    EXPECT_TRUE(outcome.is_replay);
    EXPECT_TRUE(outcome.checkpoint_safe);
}

TEST(SyncCoreTest, DominatedRemoteUpdateIsStaleRejected) {
    const CanonicalRecord local = make_record("id-2", "jobs", "cloud", {3, 1}, RecordOperation::kUpdate, "payload-local");
    const CanonicalRecord remote = make_record("id-2", "jobs", "cloud", {2, 1}, RecordOperation::kUpdate, "payload-remote");

    const ResolveOutcome outcome = CloudVehicleSyncCore::resolve_remote_record(
        remote,
        &local,
        RecordOwner::kShared,
        "cloud",
        2000);

    EXPECT_EQ(outcome.disposition, ApplyDisposition::kStaleRejected);
    EXPECT_FALSE(outcome.should_apply);
    EXPECT_TRUE(outcome.should_persist_conflict);
    EXPECT_EQ(outcome.conflict_record.conflict_class, ConflictClass::kStaleReplay);
}

TEST(SyncCoreTest, ConcurrentConflictGeneratesDeterministicRecord) {
    const CanonicalRecord local = make_record("id-3", "jobs", "cloud", {3, 1}, RecordOperation::kUpdate, "left", "corr-3");
    const CanonicalRecord remote = make_record("id-3", "jobs", "cloud", {2, 2}, RecordOperation::kUpdate, "right", "corr-3");

    const ResolveOutcome first = CloudVehicleSyncCore::resolve_remote_record(
        remote,
        &local,
        RecordOwner::kShared,
        "cloud",
        3000);
    const ResolveOutcome second = CloudVehicleSyncCore::resolve_remote_record(
        remote,
        &local,
        RecordOwner::kShared,
        "cloud",
        3000);

    EXPECT_EQ(first.disposition, ApplyDisposition::kConflictPersisted);
    EXPECT_TRUE(first.should_persist_conflict);
    EXPECT_EQ(first.conflict_record.conflict_class, ConflictClass::kConcurrentUpdate);
    EXPECT_EQ(first.conflict_record.local_version, second.conflict_record.local_version);
    EXPECT_EQ(first.conflict_record.remote_version, second.conflict_record.remote_version);
    EXPECT_EQ(first.conflict_record.local_payload, second.conflict_record.local_payload);
    EXPECT_EQ(first.conflict_record.remote_payload, second.conflict_record.remote_payload);
    EXPECT_EQ(first.conflict_record.detected_at_ms, second.conflict_record.detected_at_ms);
}

TEST(SyncCoreTest, TombstoneReplayMarkedDuplicate) {
    const CanonicalRecord local = make_record("id-4", "jobs", "cloud", {4, 0}, RecordOperation::kDelete, "", "corr-4");
    const CanonicalRecord remote = make_record("id-4", "jobs", "cloud", {4, 0}, RecordOperation::kDelete, "", "corr-4");

    const ResolveOutcome outcome = CloudVehicleSyncCore::resolve_remote_record(
        remote,
        &local,
        RecordOwner::kCloud,
        "cloud",
        4000);

    EXPECT_EQ(outcome.disposition, ApplyDisposition::kDuplicate);
    EXPECT_TRUE(outcome.is_replay);
    EXPECT_TRUE(outcome.is_tombstone_replay);
}

TEST(SyncCoreTest, AckReplayDoesNotAdvanceCheckpoint) {
    const CanonicalRecord record = make_record("id-5", "jobs", "cloud", {1, 0}, RecordOperation::kUpdate, "p");
    const VersionAck ack = make_ack(record);

    CheckpointToken checkpoint;
    checkpoint.sequence_number = 10;
    checkpoint.last_record = record.locator;
    checkpoint.last_version = record.version_vector;

    const AckProcessingResult first = CloudVehicleSyncCore::process_acks({ack}, {}, checkpoint);
    EXPECT_FALSE(first.checkpoint_advanced);
    EXPECT_EQ(first.accepted_acks.size(), 0U);
    EXPECT_EQ(first.replayed_acks.size(), 1U);
    EXPECT_EQ(first.next_checkpoint.sequence_number, 10U);

    const AckProcessingResult replay = CloudVehicleSyncCore::process_acks({ack}, {ack}, first.next_checkpoint);
    EXPECT_FALSE(replay.checkpoint_advanced);
    EXPECT_EQ(replay.accepted_acks.size(), 0U);
    EXPECT_EQ(replay.replayed_acks.size(), 1U);
    EXPECT_EQ(replay.next_checkpoint.sequence_number, 10U);
}

TEST(SyncCoreTest, DominatedAckReplayDoesNotAdvanceCheckpoint) {
    const CanonicalRecord stale_record = make_record("id-5", "jobs", "cloud", {1, 0}, RecordOperation::kUpdate, "p");
    const CanonicalRecord newest_record = make_record("id-5", "jobs", "cloud", {2, 0}, RecordOperation::kUpdate, "p2");
    const VersionAck stale_ack = make_ack(stale_record);
    const VersionAck newest_ack = make_ack(newest_record);

    CheckpointToken checkpoint;
    checkpoint.sequence_number = 10;
    checkpoint.last_record = newest_record.locator;
    checkpoint.last_version = newest_record.version_vector;

    const AckProcessingResult replay = CloudVehicleSyncCore::process_acks({stale_ack}, {newest_ack}, checkpoint);
    EXPECT_FALSE(replay.checkpoint_advanced);
    EXPECT_EQ(replay.accepted_acks.size(), 0U);
    EXPECT_EQ(replay.replayed_acks.size(), 1U);
    EXPECT_EQ(replay.next_checkpoint.sequence_number, 10U);
    EXPECT_EQ(replay.next_checkpoint.last_version.cloud_seq, 2U);
}

TEST(SyncCoreTest, GapRecoveryOnlyOnMismatchWithoutDirty) {
    const GapRecoveryDecision mismatch_dirty =
        CloudVehicleSyncCore::decide_gap_recovery(100, 200, true);
    EXPECT_FALSE(mismatch_dirty.trigger_gap_recovery);

    const GapRecoveryDecision mismatch_clean =
        CloudVehicleSyncCore::decide_gap_recovery(100, 200, false);
    EXPECT_TRUE(mismatch_clean.trigger_gap_recovery);
    EXPECT_EQ(mismatch_clean.reason, "checksum_mismatch_no_dirty_records");

    const GapRecoveryDecision matched =
        CloudVehicleSyncCore::decide_gap_recovery(300, 300, false);
    EXPECT_FALSE(matched.trigger_gap_recovery);
}

TEST(SyncCoreTest, NonOwnerMutationRejected) {
    const CanonicalRecord local = make_record("id-6", "jobs", "cloud", {1, 0}, RecordOperation::kCreate, "cloud-data");
    const CanonicalRecord remote = make_record("id-6", "jobs", "cloud", {1, 1}, RecordOperation::kUpdate, "truck-data", "corr-6");

    const ResolveOutcome outcome = CloudVehicleSyncCore::resolve_remote_record(
        remote,
        &local,
        RecordOwner::kCloud,
        "truck-007",
        6000);

    EXPECT_EQ(outcome.disposition, ApplyDisposition::kNonOwnerRejected);
    EXPECT_FALSE(outcome.should_apply);
    EXPECT_TRUE(outcome.should_persist_conflict);
    EXPECT_EQ(outcome.conflict_record.conflict_class, ConflictClass::kNonOwnerMutation);
}

}
}
}
