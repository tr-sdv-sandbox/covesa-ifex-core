#include "../../../common/include/cloud_vehicle_db_adapter.hpp"
#include "../../../common/include/cloud_vehicle_sync_core.hpp"

// Prefer system gtest when available; otherwise provide lightweight fallbacks so
// this file can be parsed/validated in environments without GTest.
#if __has_include(<gtest/gtest.h>)
#  include <gtest/gtest.h>
#else
#  include <string>
#  include <map>
#  include <vector>
#  include "gtest_fallback.h"
#endif

#include "../include/fake_cloud_vehicle_db_adapter.hpp"

namespace ifex {
namespace sync {
namespace {

using std::string;


ByteBuffer bytes(const char* s) {
    ByteBuffer out;
    while (*s) { out.push_back(static_cast<std::uint8_t>(*s)); ++s; }
    return out;
}

CanonicalRecord make_record(const char* id, const char* ns, const char* origin, VersionVector v, RecordOperation op, const char* payload, const char* corr = "c") {
    CanonicalRecord r;
    r.locator.record_id = bytes(id);
    r.locator.namespace_name = ns;
    r.locator.origin_node_id = origin;
    r.version_vector = v;
    r.operation = op;
    r.payload = bytes(payload);
    r.schema_version = 1;
    r.correlation_id = corr;
    r.payload_checksum = 0;
    return r;
}

VersionAck make_ack(const CanonicalRecord& record) {
    VersionAck ack;
    ack.locator = record.locator;
    ack.version_vector = record.version_vector;
    ack.correlation_id = record.correlation_id;
    ack.idempotency_key = "ack";
    return ack;
}

bool supports_duplicate_apply_contract(CloudVehicleDbAdapter& adapter) {
    CanonicalRecord r = make_record("dup-contract", "jobs", "cloud", {1,0}, RecordOperation::kUpdate, "payload");
    const ApplyResult first = adapter.apply_record(r, "dup-key");
    const ApplyResult second = adapter.apply_record(r, "dup-key");
    return first.disposition == ApplyDisposition::kApplied &&
           second.disposition == ApplyDisposition::kDuplicate;
}

TEST(FakeAdapterContract, IdempotentApplyAndDuplicate) {
    InMemoryFakeAdapter a;
    EXPECT_TRUE(supports_duplicate_apply_contract(a));
}

// Negative-targeted contract test: ensure duplicate apply is detected (selector: adapter_contract_negative)
TEST(AdapterContractNegative, DuplicateApply) {
    // Use a deliberately broken adapter (does not honor idempotency) to
    // demonstrate the negative-contract test that should detect broken impls.
    class BrokenFakeAdapter : public CloudVehicleDbAdapter {
    public:
        std::vector<CanonicalRecord> list_dirty_records(const DirtyRecordQuery& query) override { return {}; }
        ApplyResult apply_record(const CanonicalRecord& record,
                                 const std::string& idempotency_key,
                                 const std::string& sender_node_id = "") override {
            // Broken: always overwrite and mark as applied (no idempotency)
            ApplyResult res;
            res.disposition = ApplyDisposition::kApplied;
            res.durable_version = record.version_vector;
            return res;
        }
        CheckpointReadResult read_checkpoint(const SyncSessionKey& session) override { return {}; }
        void write_checkpoint(const SyncSessionKey& session, const CheckpointToken& checkpoint) override {}
        void persist_remote_acks(const SyncSessionKey& session,
                                 const std::vector<VersionAck>& durable_acks) override {}
        std::vector<VersionAck> list_remote_acks(const SyncSessionKey& session) override { return {}; }
        std::uint64_t compute_state_checksum(const StateScope& scope) override { return 0; }
        std::vector<RecordLocator> list_record_ids(const RecordIdQuery& query) override { return {}; }
        void persist_conflict(const ConflictRecord& conflict) override {}
        std::vector<ConflictRecord> query_conflicts(const ConflictQuery& query) override { return {}; }
        std::vector<CanonicalRecord> list_tombstones_for_gc(const TombstoneGcQuery& query) override { return {}; }
    } broken;

    InMemoryFakeAdapter reference;
    EXPECT_TRUE(supports_duplicate_apply_contract(reference));
    EXPECT_FALSE(supports_duplicate_apply_contract(broken));
}

TEST(FakeAdapterContract, CheckpointMonotonic) {
    InMemoryFakeAdapter a;
    SyncSessionKey s{"local","remote","jobs"};
    CheckpointToken t;
    t.sequence_number = 5;
    a.write_checkpoint(s, t);
    auto read = a.read_checkpoint(s);
    EXPECT_TRUE(read.found);
    EXPECT_EQ(read.checkpoint.sequence_number, 5U);
}

TEST(FakeAdapterContract, DurableAckBatchClearsAllMatchingDirtyRecords) {
    InMemoryFakeAdapter a;
    SyncSessionKey session{"local", "remote", "jobs"};
    CanonicalRecord first = make_record("a1", "jobs", "cloud", {1, 0}, RecordOperation::kUpdate, "p1");
    CanonicalRecord second = make_record("a2", "jobs", "cloud", {2, 0}, RecordOperation::kUpdate, "p2");
    a.apply_record(first, "ack-1");
    a.apply_record(second, "ack-2");

    VersionAck first_ack = make_ack(first);
    VersionAck second_ack = make_ack(second);
    a.persist_remote_acks(session, {first_ack, second_ack});
    const auto durable_acks = a.list_remote_acks(session);
    EXPECT_EQ(durable_acks.size(), 2U);

    CheckpointToken checkpoint;
    checkpoint.sequence_number = 3;
    checkpoint.last_record = second.locator;
    checkpoint.last_version = second.version_vector;
    a.write_checkpoint(session, checkpoint);

    auto dirty = a.list_dirty_records({session, 10, true});
    EXPECT_TRUE(dirty.empty());
}

TEST(FakeAdapterContract, WriteCheckpointDoesNotPersistAckSideEffects) {
    InMemoryFakeAdapter a;
    SyncSessionKey session{"local", "remote", "jobs"};
    CanonicalRecord record = make_record("cp-no-ack", "jobs", "cloud", {1, 0}, RecordOperation::kUpdate, "p");
    a.apply_record(record, "cp-no-ack-key");

    CheckpointToken checkpoint;
    checkpoint.sequence_number = 1;
    checkpoint.last_record = record.locator;
    checkpoint.last_version = record.version_vector;
    a.write_checkpoint(session, checkpoint);

    const auto dirty = a.list_dirty_records({session, 10, true});
    EXPECT_EQ(dirty.size(), 1U);
    EXPECT_EQ(dirty[0].version_vector, record.version_vector);
}

TEST(FakeAdapterContract, DeterministicChecksum) {
    InMemoryFakeAdapter a;
    CanonicalRecord r1 = make_record("id-a", "ns", "cloud", {1,0}, RecordOperation::kUpdate, "A");
    CanonicalRecord r2 = make_record("id-b", "ns", "cloud", {2,0}, RecordOperation::kUpdate, "B");
    a.apply_record(r1, "k1");
    a.apply_record(r2, "k2");
    StateScope scope{"ns", true};
    auto c1 = a.compute_state_checksum(scope);
    auto c2 = a.compute_state_checksum(scope);
    EXPECT_EQ(c1, c2);
}

TEST(FakeAdapterContract, DirtyRecordLimitZeroMeansUnlimited) {
    InMemoryFakeAdapter a;
    SyncSessionKey session{"local", "remote", "jobs"};
    a.apply_record(make_record("lim-1", "jobs", "cloud", {1, 0}, RecordOperation::kUpdate, "p1"), "lim-1");
    a.apply_record(make_record("lim-2", "jobs", "cloud", {2, 0}, RecordOperation::kUpdate, "p2"), "lim-2");

    const auto dirty = a.list_dirty_records({session, 0, true});
    EXPECT_EQ(dirty.size(), 2U);
}

TEST(FakeAdapterContract, ChecksumTracksLogicalStateNotPayloadChecksumField) {
    InMemoryFakeAdapter lhs;
    InMemoryFakeAdapter rhs;
    CanonicalRecord left = make_record("cs-1", "jobs", "cloud", {1, 0}, RecordOperation::kUpdate, "same");
    CanonicalRecord right = left;
    left.payload_checksum = 123;
    right.payload_checksum = 999;

    lhs.apply_record(left, "cs-key");
    rhs.apply_record(right, "cs-key");

    EXPECT_EQ(lhs.compute_state_checksum({"jobs", true}), rhs.compute_state_checksum({"jobs", true}));
}

TEST(FakeAdapterContract, ListIdsAndTombstoneBehavior) {
    InMemoryFakeAdapter a;
    CanonicalRecord r = make_record("id-t", "ns2", "cloud", {1,0}, RecordOperation::kDelete, "");
    r.tombstone_at_ms = 1000;
    a.apply_record(r, "kdel");
    RecordIdQuery q{"ns2", true, 0};
    auto ids = a.list_record_ids(q);
    EXPECT_EQ(ids.size(), 1U);
    VersionAck ack = make_ack(r);
    a.persist_remote_acks({"local","remote","ns2"}, {ack});
    TombstoneGcQuery tq{{"local","remote","ns2"}, 2000, 10};
    auto tombs = a.list_tombstones_for_gc(tq);
    EXPECT_EQ(tombs.size(), 1U);
}

TEST(FakeAdapterContract, ConflictPersistenceAndQuery) {
    InMemoryFakeAdapter a;
    ConflictRecord c;
    c.locator.namespace_name = "cn";
    c.detected_at_ms = 1234;
    a.persist_conflict(c);
    ConflictQuery q{"cn", 0, false, 10};
    auto res = a.query_conflicts(q);
    EXPECT_EQ(res.size(), 1U);
}

TEST(FakeAdapterContract, StaleUpdateIsRejectedAndPersistsConflict) {
    InMemoryFakeAdapter a;
    CanonicalRecord newer = make_record("stale-id", "jobs", "cloud", {2, 0}, RecordOperation::kUpdate, "new");
    CanonicalRecord older = make_record("stale-id", "jobs", "cloud", {1, 0}, RecordOperation::kUpdate, "old");
    a.apply_record(newer, "stale-newer");

    const auto stale = a.apply_record(older, "stale-older", "cloud");
    EXPECT_EQ(stale.disposition, ApplyDisposition::kStaleRejected);
    EXPECT_TRUE(stale.has_persisted_conflict);

    const auto conflicts = a.query_conflicts({"jobs", 0, true, 10});
    EXPECT_EQ(conflicts.size(), 1U);
    EXPECT_EQ(conflicts[0].conflict_class, ConflictClass::kStaleReplay);
}

TEST(FakeAdapterContract, ConcurrentUpdatePersistsConflict) {
    InMemoryFakeAdapter a;
    CanonicalRecord local = make_record("cc-id", "jobs", "cloud", {2, 0}, RecordOperation::kUpdate, "left");
    CanonicalRecord remote = make_record("cc-id", "jobs", "cloud", {1, 1}, RecordOperation::kUpdate, "right");
    a.apply_record(local, "cc-local");

    const auto concurrent = a.apply_record(remote, "cc-remote", "cloud");
    EXPECT_EQ(concurrent.disposition, ApplyDisposition::kConflictPersisted);
    EXPECT_TRUE(concurrent.has_persisted_conflict);

    const auto conflicts = a.query_conflicts({"jobs", 0, true, 10});
    EXPECT_EQ(conflicts.size(), 1U);
    EXPECT_EQ(conflicts[0].conflict_class, ConflictClass::kConcurrentUpdate);
}

TEST(FakeAdapterContract, CloudOwnedNamespaceRejectsTruckMutation) {
    InMemoryFakeAdapter a;
    CanonicalRecord cloud_owned = make_record("owner-id", "cloud-owned", "cloud", {1, 0}, RecordOperation::kUpdate, "payload");

    const auto rejected = a.apply_record(cloud_owned, "owner-reject", "truck-007");
    EXPECT_EQ(rejected.disposition, ApplyDisposition::kNonOwnerRejected);
    EXPECT_TRUE(rejected.has_persisted_conflict);

    const auto conflicts = a.query_conflicts({"cloud-owned", 0, true, 10});
    EXPECT_EQ(conflicts.size(), 1U);
    EXPECT_EQ(conflicts[0].conflict_class, ConflictClass::kNonOwnerMutation);
}

TEST(FakeAdapterContract, StaleCheckpointNegative) {
    InMemoryFakeAdapter a;
    SyncSessionKey s{"l","r","none"};
    auto r = a.read_checkpoint(s);
    EXPECT_FALSE(r.found);
}

}
}
}
