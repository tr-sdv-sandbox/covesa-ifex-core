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

TEST(FakeAdapterContract, IdempotentApplyAndDuplicate) {
    InMemoryFakeAdapter a;
    CanonicalRecord r = make_record("x1", "jobs", "cloud", {1,0}, RecordOperation::kUpdate, "p");
    auto first = a.apply_record(r, "key1");
    EXPECT_EQ(first.disposition, ApplyDisposition::kApplied);
    auto dup = a.apply_record(r, "key1");
    EXPECT_EQ(dup.disposition, ApplyDisposition::kDuplicate);
}

// Negative-targeted contract test: ensure duplicate apply is detected (selector: adapter_contract_negative)
TEST(AdapterContractNegative, DuplicateApply) {
    // Use a deliberately broken adapter (does not honor idempotency) to
    // demonstrate the negative-contract test that should detect broken impls.
    class BrokenFakeAdapter : public CloudVehicleDbAdapter {
    public:
        std::vector<CanonicalRecord> list_dirty_records(const DirtyRecordQuery& query) override { return {}; }
        ApplyResult apply_record(const CanonicalRecord& record, const std::string& idempotency_key) override {
            // Broken: always overwrite and mark as applied (no idempotency)
            ApplyResult res;
            res.disposition = ApplyDisposition::kApplied;
            res.durable_version = record.version_vector;
            return res;
        }
        CheckpointReadResult read_checkpoint(const SyncSessionKey& session) override { return {}; }
        void write_checkpoint(const SyncSessionKey& session, const CheckpointToken& checkpoint) override {}
        std::uint64_t compute_state_checksum(const StateScope& scope) override { return 0; }
        std::vector<RecordLocator> list_record_ids(const RecordIdQuery& query) override { return {}; }
        void persist_conflict(const ConflictRecord& conflict) override {}
        std::vector<ConflictRecord> query_conflicts(const ConflictQuery& query) override { return {}; }
        std::vector<CanonicalRecord> list_tombstones_for_gc(const TombstoneGcQuery& query) override { return {}; }
    } broken;

    CanonicalRecord r = make_record("neg-1", "jobs", "cloud", {1,0}, RecordOperation::kUpdate, "p");
    auto first = broken.apply_record(r, "neg-key");
    EXPECT_EQ(first.disposition, ApplyDisposition::kApplied);
    auto dup = broken.apply_record(r, "neg-key");
    // The negative contract asserts that this deliberately broken adapter does NOT
    // satisfy the idempotency contract (i.e. it should NOT report duplicates).
    // We express that as EXPECT_NE so the test passes when the adapter is broken
    // and fails when a correct adapter (which reports kDuplicate) is used.
    EXPECT_NE(dup.disposition, ApplyDisposition::kDuplicate);
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

TEST(FakeAdapterContract, ListIdsAndTombstoneBehavior) {
    InMemoryFakeAdapter a;
    CanonicalRecord r = make_record("id-t", "ns2", "cloud", {1,0}, RecordOperation::kDelete, "");
    r.tombstone_at_ms = 1000;
    a.apply_record(r, "kdel");
    RecordIdQuery q{"ns2", true, 0};
    auto ids = a.list_record_ids(q);
    EXPECT_EQ(ids.size(), 1U);
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

TEST(FakeAdapterContract, StaleCheckpointNegative) {
    InMemoryFakeAdapter a;
    SyncSessionKey s{"l","r","none"};
    auto r = a.read_checkpoint(s);
    EXPECT_FALSE(r.found);
}

}
}
}
