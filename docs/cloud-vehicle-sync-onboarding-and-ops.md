# Cloud-Vehicle Synchronization: Adapter Onboarding and Operations Guide

**Document Version:** 1.0  
**Date:** 2026-03-26  
**Scope:** v1 reference implementation, non-production

## Overview

This guide covers how to integrate custom data repositories with the cloud-vehicle sync protocol without reimplementing conflict detection, replay handling, or checkpoint logic. It also explains operator concepts for interpreting conflicts, checkpoints, and tombstones in production scenarios.

### Key Principle

The protocol splits responsibilities:

| Responsibility                                                | Owner                  | Provided By              |
| ------------------------------------------------------------- | ---------------------- | ------------------------ |
| Protocol logic (versioning, conflict detection, gap recovery) | `CloudVehicleSyncCore` | Core library             |
| Data persistence and queries                                  | Your adapter           | Your implementation      |
| Transport envelope handling                                   | Transport bridge       | Reference implementation |

By implementing one adapter interface, your storage backend gains conflict surfacing, idempotency, and resumable sync automatically.

---

## Part 1: Adapter Onboarding

### 1.1 The Adapter SPI (Service Provider Interface)

All adapters implement this interface:

```cpp
class CloudVehicleDbAdapter {
public:
    virtual ~CloudVehicleDbAdapter() = default;

    // Retrieve dirty (unaacked) records for resumption
    virtual std::vector<CanonicalRecord> list_dirty_records(
        const DirtyRecordQuery& query) = 0;

    // Apply incoming record, with idempotency handling
    virtual ApplyResult apply_record(
        const CanonicalRecord& record,
        const std::string& idempotency_key,
        const std::string& sender_node_id = "") = 0;

    // Checkpoint management (track sync progress)
    virtual CheckpointReadResult read_checkpoint(
        const SyncSessionKey& session) = 0;
    virtual void write_checkpoint(
        const SyncSessionKey& session,
        const CheckpointToken& checkpoint) = 0;
    virtual void persist_remote_acks(
        const SyncSessionKey& session,
        const std::vector<VersionAck>& durable_acks) = 0;
    virtual std::vector<VersionAck> list_remote_acks(
        const SyncSessionKey& session) = 0;

    // Checksumming (verify state match between peers)
    virtual uint64_t compute_state_checksum(
        const StateScope& scope) = 0;

    // Gap recovery support
    virtual std::vector<RecordLocator> list_record_ids(
        const RecordIdQuery& query) = 0;

    // Conflict persistence and queries
    virtual void persist_conflict(const ConflictRecord& conflict) = 0;
    virtual std::vector<ConflictRecord> query_conflicts(
        const ConflictQuery& query) = 0;

    // Tombstone lifecycle management
    virtual std::vector<CanonicalRecord> list_tombstones_for_gc(
        const TombstoneGcQuery& query) = 0;
};
```

### 1.2 Essential Data Structures

All adapters work with these canonical types defined in `cloud_vehicle_sync_types.hpp`:

#### RecordLocator

Unique identity for a record across distributed systems:

```cpp
struct RecordLocator {
    ByteBuffer record_id;          // Your record's key (opaque bytes)
    std::string namespace_name;    // Logical grouping ("jobs", "config", etc.)
    std::string origin_node_id;    // "cloud" or "truck-001"
};
```

#### CanonicalRecord

The unified record format all adapters handle:

```cpp
struct CanonicalRecord {
    RecordLocator locator;         // Identity
    VersionVector version_vector;  // Logical versioning (not timestamps)
    RecordOperation operation;     // kCreate | kUpdate | kDelete
    ByteBuffer payload;            // Your serialized data
    std::uint32_t schema_version;  // For payload evolution

    // Metadata (diagnostic only, NOT used for sync logic)
    std::uint64_t wall_clock_ms;   // Sender's clock (NOT for ordering)
    std::uint64_t created_at_ms;   // Reference timestamp
    std::uint64_t updated_at_ms;   // Reference timestamp

    // Idempotency tracking
    std::string idempotency_key;   // Dedup key for this exact version
    std::string correlation_id;    // Optional request/response linkage

    // Checksumming and deletion
    std::uint64_t payload_checksum;
    std::uint64_t tombstone_at_ms; // When deleted (if operation == kDelete)
    std::string tombstone_reason;  // Why deleted
};
```

#### VersionVector

Logical, not wall-clock based:

```cpp
struct VersionVector {
    std::uint64_t cloud_seq = 0;   // Sequence of updates from cloud origin
    std::uint64_t truck_seq = 0;   // Sequence of updates from vehicle origin
};

// Comparison semantics:
bool dominates(const VersionVector& other) const {
    return cloud_seq >= other.cloud_seq && truck_seq >= other.truck_seq &&
           (cloud_seq > other.cloud_seq || truck_seq > other.truck_seq);
}
// Returns true if this version strictly subsumes other
// Used by protocol core to decide: who wins?
```

#### CheckpointToken

Tracks sync progress across reconnects:

```cpp
struct CheckpointToken {
    std::uint64_t sequence_number = 0;  // Monotonic batch counter
    RecordLocator last_record;          // Last record processed
    VersionVector last_version;         // Version of last record
};
```

This is what's persisted after each successful batch. On reconnect, protocol core resumes from the checkpoint instead of full rescan.

#### ConflictRecord

Persisted when sync detects incompatible updates:

```cpp
struct ConflictRecord {
    RecordLocator locator;
    VersionVector local_version;        // What we have locally
    VersionVector remote_version;       // What remote sent
    ByteBuffer local_payload;           // Local record state
    ByteBuffer remote_payload;          // Remote record state
    ConflictClass conflict_class;       // kConcurrentUpdate | kNonOwnerMutation | kStaleReplay
    std::uint64_t detected_at_ms;       // When first seen
    std::string correlation_id;         // Link to originating sync message
    bool resolved = false;              // Application has handled this
};
```

### 1.3 Implementing Each Interface Method

#### `list_dirty_records()`

Returns records that have not yet been acknowledged by the remote side.

**Purpose:** On reconnect, the protocol sends all dirty records again (at-least-once semantics). The adapter identifies which ones are unacked using the query session context.

**Query Input:**

```cpp
struct DirtyRecordQuery {
    SyncSessionKey session;    // (local_node_id, remote_node_id, namespace)
    std::size_t limit = 100;   // Batch size for pagination
    bool include_tombstones;   // Whether to list deleted records
};

struct SyncSessionKey {
    std::string local_node_id;   // "cloud" or "truck-001"
    std::string remote_node_id;  // Peer identity
    std::string namespace_name;  // "jobs", "config", etc.
};
```

**Returns:** Vector of `CanonicalRecord` representing all records in this namespace that are either new or have not been acked by the named remote peer yet.

**Implementation guidance:**

- Maintain a per-session ACK ledger (populated by `persist_remote_acks()` calls, independent of checkpoints).
- `list_dirty_records()` returns records NOT in the ack ledger, ordered by (record_id) for determinism.
- Include tombstones only if `include_tombstones == true` to support optional cleanup scans.

**Example (pseudo-SQL):**

```sql
SELECT r.* FROM records r
WHERE r.namespace = :namespace
  AND NOT EXISTS (
    SELECT 1 FROM session_acks
    WHERE session_acks.local_node = :local
      AND session_acks.remote_node = :remote
      AND session_acks.record_id = r.record_id
      AND session_acks.version = r.version
  )
ORDER BY r.record_id
LIMIT :limit;
```

#### `apply_record()`

Receive and durably persist a record from the remote side.

**Purpose:** The protocol core has already decided this record should be applied (ownership checks, version comparison done). Your adapter just needs to persist it atomically.

**Inputs:**

- `record`: The incoming record with version vector, payload, operation.
- `idempotency_key`: A string like `"{origin}:{record_id}:{version_hash}"` that identifies this exact version. If the same key is applied twice, it's a replay.
- `sender_node_id` (optional): Authenticated peer identity validated at bridge boundary (e.g., mTLS certificate CN). Use to enforce that incoming records originate from the expected peer. Do not confuse with `record.locator.origin_node_id` (record creator metadata, may differ if records flow through intermediaries).

**Returns:** `ApplyResult` indicating what happened:

```cpp
enum class ApplyDisposition {
    kApplied = 0,            // New record stored
    kDuplicate = 1,          // Replay (same idempotency_key seen before)
    kStaleRejected = 2,      // Ignored (older version than local)
    kNonOwnerRejected = 3,   // Ignored (authority violation)
    kConflictPersisted = 4,  // Conflict created instead of update
};

struct ApplyResult {
    ApplyDisposition disposition;
    VersionVector durable_version;      // What was finally stored
    bool has_persisted_conflict;        // If true, populated persisted_conflict
    ConflictRecord persisted_conflict;  // Details of conflict
};
```

**Implementation guidance:**

- Check if `idempotency_key` exists in your dedup table. If yes, return `kDuplicate`.
- Check ownership: if the record's origin cannot modify this record, return `kNonOwnerRejected`.
- Compare versions: if incoming version is stale (older), return `kStaleRejected`.
- Otherwise, update your record table and insert the idempotency key.
- For DELETE operations, set `operation = kDelete` and `tombstone_at_ms = now()` instead of removing the row.

**Key detail:** The protocol core decides whether to apply or conflict. Your job is just to store the decision's result.

#### `read_checkpoint()` and `write_checkpoint()`

Persist and retrieve sync progress.

**Purpose:** On reconnect, instead of sending all records again, the protocol resumes from the last checkpoint, which reduces bandwidth and latency.

**`read_checkpoint()` input:**

```cpp
struct SyncSessionKey {
    std::string local_node_id;
    std::string remote_node_id;
    std::string namespace_name;
};
```

**Returns:**

```cpp
struct CheckpointReadResult {
    bool found = false;
    CheckpointToken checkpoint;  // sequence_number, last_record, last_version
};
```

Implementation: Query your checkpoint table for the named session. If not found, return `found=false` (will start from beginning).

**`write_checkpoint()` input:**
The same session key plus a checkpoint token:

```cpp
struct CheckpointToken {
    std::uint64_t sequence_number;   // Batch number (increments on each advance)
    RecordLocator last_record;       // Last record ID processed
    VersionVector last_version;      // Its version
};
```

**Purpose:** Persist sync progress durably so reconnects can resume from the checkpoint instead of full rescan.

Implementation: Insert/upsert the checkpoint into your checkpoint table. Verify the new sequence_number is monotonically higher than any prior checkpoint for this session.

#### `persist_remote_acks()` and `list_remote_acks()`

Manage durable acknowledgment state, separate from checkpoints.

**Purpose:** Track which record versions have been confirmed received and applied by the remote peer. On reconnect, the bridge can skip re-sending already-acked records and rebuild in-memory ACK state from persistent storage.

**`persist_remote_acks()` input:**

```cpp
struct VersionAck {
    RecordLocator locator;        // Which record
    VersionVector version_vector;  // Up to which version is acked
};
```

**Implementation:** Persist the ack set durably into a per-session ack ledger. This is typically a separate table from records (e.g., `remote_acks` table with columns: `local_node_id`, `remote_node_id`, `namespace`, `record_id`, `origin_node_id`, `cloud_seq`, `truck_seq`).

**`list_remote_acks()` returns:**
All persisted acks for the given session as a vector of `VersionAck`. On a process restart, the bridge calls this to rebuild its in-memory state before resuming sync.

**Key detail:** ACK durability is separate from checkpoint writes. A checkpoint represents sync progress; ACKs represent remote acknowledgments. Both must persist to survive restarts safely.

#### `compute_state_checksum()`

Calculate a deterministic hash of your current state.

**Purpose:** Both sides compute the same checksum; if they differ, gap recovery is triggered to find missing/extra records.

**Input:**

```cpp
struct StateScope {
    std::string namespace_name;    // Which data to checksum
    bool include_tombstones;       // Whether to include deletes
};
```

**Returns:** A `uint64_t` checksum (e.g., FNV-1a style deterministic hash over logical-record bytes in the scope).

**Implementation guidance:**

- Select all records in the namespace (optionally excluding tombstones).
- Sort by record_id for determinism.
- For each record (sorted deterministically), mix: `namespace || origin_node_id || record_id || cloud_seq || truck_seq || operation || payload || schema_version || tombstone_state || tombstone_reason`.
- Exclude wall-clock and transport metadata fields: `wall_clock_ms`, `created_at_ms`, `updated_at_ms`, `idempotency_key`, `correlation_id`, and `payload_checksum` do NOT participate.
- Use a stable deterministic combiner so equivalent logical state yields identical output regardless of apply order.

**Critical:** Checksums MUST be deterministic. If two adapters have the same logical records, they MUST produce the same checksum, even if applied in different order.

#### `list_record_ids()`

Used during gap recovery to identify which records you have.

**Input:**

```cpp
struct RecordIdQuery {
    std::string namespace_name;
    bool include_tombstones;
    std::size_t limit = 0;    // 0 means no limit
};
```

**Returns:** Vector of `RecordLocator` (record_id + namespace + origin_node_id) for all records you have.

**Purpose:** Remote side sends its IDs; you compare and identify missing records. Protocol core requests only the missing ones, avoiding redundant transfer.

Implementation: Simple query to your records table, return all locators in the scope.

#### `persist_conflict()` and `query_conflicts()`

Store and retrieve conflict records.

**`persist_conflict()` input:**

```cpp
struct ConflictRecord {
    RecordLocator locator;
    VersionVector local_version;    // What we have
    VersionVector remote_version;   // What remote sent
    ByteBuffer local_payload;       // Our data
    ByteBuffer remote_payload;      // Their data
    ConflictClass conflict_class;   // kConcurrentUpdate, kNonOwnerMutation, kStaleReplay
    std::uint64_t detected_at_ms;   // When detected (caller-provided for determinism)
    std::string correlation_id;     // Optional linkage
    bool resolved = false;          // Initially false
};
```

Implementation: Insert into your conflicts table. Do NOT auto-resolve; leave it for operators/applications to handle.

**`query_conflicts()` input:**

```cpp
struct ConflictQuery {
    std::string namespace_name;
    std::uint64_t since_detected_at_ms = 0;  // Only conflicts after this time
    bool include_resolved = false;           // Whether to list resolved ones
    std::size_t limit = 100;
};
```

Returns: All conflicts matching the query.

Implementation: Simple query with filters. When applications resolve a conflict, update `resolved = true` in your table.

#### `list_tombstones_for_gc()`

Identify old tombstones ready for deletion.

**Input:**

```cpp
struct TombstoneGcQuery {
    SyncSessionKey session;
    std::uint64_t retention_cutoff_ms;  // Only tombstones created before this time
    std::size_t limit = 100;
};
```

**Returns:** Tombstone records (operation == kDelete, tombstone_at_ms < cutoff) that can be hard-deleted.

**Purpose:** Prevents unbounded storage growth from accumulating deletes.

Implementation: Query for records with `operation = kDelete AND tombstone_at_ms < cutoff_ms`. In your GC job, hard-delete these after confirming all remote peers have acked them.

---

### 1.4 A Complete Example: In-Memory Adapter

Here's a minimal in-memory adapter reference (pseudocode, omitting `persist_remote_acks()` and `list_remote_acks()` for brevity; see section 1.2 for required signatures):

```cpp
#include "cloud_vehicle_db_adapter.hpp"
#include <map>
#include <unordered_map>

namespace ifex::sync {

class InMemoryAdapter : public CloudVehicleDbAdapter {
public:
    std::vector<CanonicalRecord> list_dirty_records(
        const DirtyRecordQuery& query) override {
        std::vector<CanonicalRecord> result;
        for (const auto& [id, record] : records_) {
            if (record.locator.namespace_name != query.session.namespace_name) {
                continue;
            }
            if (record.operation == RecordOperation::kDelete &&
                !query.include_tombstones) {
                continue;
            }
            // Check if this record is acked by remote
            auto ack_key = std::string(query.session.remote_node_id) + ":" +
                          std::string(record.locator.record_id.begin(),
                                    record.locator.record_id.end());
            if (remote_acks_.find(ack_key) == remote_acks_.end()) {
                result.push_back(record);
            }
            if (result.size() >= query.limit) break;
        }
        return result;
    }

    ApplyResult apply_record(
        const CanonicalRecord& record,
        const std::string& idempotency_key,
        const std::string& sender_node_id = "") override {
        ApplyResult result;

        // Check idempotency
        if (seen_idempotency_keys_.count(idempotency_key)) {
            result.disposition = ApplyDisposition::kDuplicate;
            result.durable_version = record.version_vector;
            return result;
        }

        // Store record and mark key as seen
        auto id = std::string(record.locator.record_id.begin(),
                            record.locator.record_id.end());
        records_[id] = record;
        seen_idempotency_keys_.insert(idempotency_key);

        result.disposition = ApplyDisposition::kApplied;
        result.durable_version = record.version_vector;
        return result;
    }

    CheckpointReadResult read_checkpoint(
        const SyncSessionKey& session) override {
        CheckpointReadResult result;
        auto key = session.local_node_id + ":" + session.remote_node_id + ":" +
                   session.namespace_name;
        if (checkpoints_.count(key)) {
            result.found = true;
            result.checkpoint = checkpoints_[key];
        }
        return result;
    }

    void write_checkpoint(
        const SyncSessionKey& session,
        const CheckpointToken& checkpoint) override {
        auto key = session.local_node_id + ":" + session.remote_node_id + ":" +
                   session.namespace_name;
        checkpoints_[key] = checkpoint;

        // Checkpoint persistence is independent of ACK persistence.
        // Remote ACK durability is handled via persist_remote_acks(...).
    }

    uint64_t compute_state_checksum(
        const StateScope& scope) override {
        uint64_t combined = 0;
        for (const auto& [id, record] : records_) {
            if (record.locator.namespace_name != scope.namespace_name) {
                continue;
            }
            if (record.operation == RecordOperation::kDelete &&
                !scope.include_tombstones) {
                continue;
            }
            // Example FNV-1a-style logical-state mixing (pseudo-code)
            combined = fnv1a_mix(combined, record.locator.namespace_name);
            combined = fnv1a_mix(combined, record.locator.origin_node_id);
            combined = fnv1a_mix(combined, record.locator.record_id);
            combined = fnv1a_mix(combined, record.version_vector.cloud_seq);
            combined = fnv1a_mix(combined, record.version_vector.truck_seq);
            combined = fnv1a_mix(combined, static_cast<uint32_t>(record.operation));
            combined = fnv1a_mix(combined, record.payload);
        }
        return combined;
    }

    std::vector<RecordLocator> list_record_ids(
        const RecordIdQuery& query) override {
        std::vector<RecordLocator> result;
        for (const auto& [id, record] : records_) {
            if (record.locator.namespace_name != query.namespace_name) {
                continue;
            }
            if (record.operation == RecordOperation::kDelete &&
                !query.include_tombstones) {
                continue;
            }
            result.push_back(record.locator);
            if (query.limit > 0 && result.size() >= query.limit) break;
        }
        return result;
    }

    void persist_conflict(const ConflictRecord& conflict) override {
        conflicts_.push_back(conflict);
    }

    std::vector<ConflictRecord> query_conflicts(
        const ConflictQuery& query) override {
        std::vector<ConflictRecord> result;
        for (const auto& c : conflicts_) {
            if (c.locator.namespace_name != query.namespace_name) {
                continue;
            }
            if (c.detected_at_ms < query.since_detected_at_ms) {
                continue;
            }
            if (!query.include_resolved && c.resolved) {
                continue;
            }
            result.push_back(c);
            if (result.size() >= query.limit) break;
        }
        return result;
    }

     std::vector<CanonicalRecord> list_tombstones_for_gc(
         const TombstoneGcQuery& query) override {
         std::vector<CanonicalRecord> result;
         for (const auto& [id, record] : records_) {
             if (record.locator.namespace_name != query.session.namespace_name) {
                 continue;
             }
             if (record.operation != RecordOperation::kDelete) {
                 continue;
             }
             if (record.tombstone_at_ms >= query.retention_cutoff_ms) {
                 continue;
             }
             result.push_back(record);
             if (result.size() >= query.limit) break;
         }
         return result;
     }

     // REQUIRED: Persist durable remote ACK set
     void persist_remote_acks(
         const SyncSessionKey& session,
         const std::vector<VersionAck>& durable_acks) override {
         // In-memory store: update remote_acks_ with {session, durable_acks}
         // Real implementation: persist to database as separate ACK ledger
     }

     // REQUIRED: Retrieve persisted remote ACKs for session
     std::vector<VersionAck> list_remote_acks(
         const SyncSessionKey& session) override {
         // In-memory retrieve: return remote_acks_[session]
         // Real implementation: query persisted ACK ledger
         return {};
     }

private:
    std::map<std::string, CanonicalRecord> records_;
    std::unordered_map<std::string, CheckpointToken> checkpoints_;
    std::unordered_set<std::string> seen_idempotency_keys_;
    std::unordered_set<std::string> remote_acks_;
    std::vector<ConflictRecord> conflicts_;
};

}
```

---

### 1.5 Building and Testing Your Adapter

#### Compilation

Link your adapter implementation against:

- `ifex-sync-core` - protocol logic
- `ifex-proto-generated` - canonical record types

```cmake
add_library(my-sync-adapter
    src/my_adapter.cpp
)

target_link_libraries(my-sync-adapter
    PUBLIC
        ifex-sync-core
        ifex-proto-generated
    PRIVATE
        # Your dependencies: sqlite, postgres, etc.
)

target_include_directories(my-sync-adapter
    PUBLIC
        include
        ${CMAKE_SOURCE_DIR}/reference-services/sync/common/include
)
```

#### Unit Testing

Test your adapter in isolation:

```cpp
#include <gtest/gtest.h>
#include "my_sync_adapter.hpp"

TEST(MyAdapterTest, ApplyRecordAndRetrieveDirty) {
    MyAdapter adapter;

    CanonicalRecord record;
    record.locator.record_id = ByteBuffer{0x01, 0x02};
    record.locator.namespace_name = "jobs";
    record.locator.origin_node_id = "cloud";
    record.version_vector = {1, 0};
    record.payload = ByteBuffer{0xAA, 0xBB};

    auto result = adapter.apply_record(record, "cloud:job-1:hash123");
    EXPECT_EQ(result.disposition, ApplyDisposition::kApplied);

    // Retrieve it
    DirtyRecordQuery query;
    query.session.local_node_id = "truck-001";
    query.session.remote_node_id = "cloud";
    query.session.namespace_name = "jobs";

    auto dirty = adapter.list_dirty_records(query);
    EXPECT_EQ(dirty.size(), 1);
    EXPECT_EQ(dirty[0].locator.record_id, record.locator.record_id);
}

TEST(MyAdapterTest, CheckpointAndAckPersistenceAreIndependent) {
    MyAdapter adapter;

    // Apply a record
    // ...

    // Checkpoint it
    CheckpointToken checkpoint;
    checkpoint.sequence_number = 1;
    checkpoint.last_record.record_id = ByteBuffer{0x01, 0x02};
    checkpoint.last_record.namespace_name = "jobs";
    checkpoint.last_record.origin_node_id = "cloud";
    checkpoint.last_version = {1, 0};

    SyncSessionKey session;
    session.local_node_id = "truck-001";
    session.remote_node_id = "cloud";
    session.namespace_name = "jobs";

    adapter.write_checkpoint(session, checkpoint);

    // Still dirty until explicit remote ACK durability is written
    DirtyRecordQuery query;
    query.session = session;
    auto dirty_before_ack = adapter.list_dirty_records(query);
    EXPECT_EQ(dirty_before_ack.size(), 1);

    VersionAck ack;
    ack.locator = checkpoint.last_record;
    ack.version_vector = checkpoint.last_version;
    adapter.persist_remote_acks(session, {ack});

    auto dirty_after_ack = adapter.list_dirty_records(query);
    EXPECT_EQ(dirty_after_ack.size(), 0);
}
```

---

## Part 2: Operations and Monitoring

### 2.1 Conflict Interpretation

Conflicts are the primary way operators detect data anomalies.

#### Conflict Classes

| Class                | Meaning                                      | Example                                                                   | Action                                                           |
| -------------------- | -------------------------------------------- | ------------------------------------------------------------------------- | ---------------------------------------------------------------- |
| `CONCURRENT_UPDATE`  | Both sides modified independently            | Cloud modified job config AND truck added execution result to same record | Manual review and merge decision                                 |
| `NON_OWNER_MUTATION` | Non-owner attempted write                    | Truck tried to modify a cloud-owned job definition                        | Reject mutation, investigate truck-side logic error              |
| `STALE_REPLAY`       | Very old version replayed (shouldn't happen) | Version from 2 hours ago arrives after newer version already stored       | Check for clock skew or transport issues; usually safe to ignore |

#### Querying Conflicts

```bash
# Query all unresolved conflicts for jobs namespace
adapter.query_conflicts({
    namespace_name: "jobs",
    include_resolved: false,
    limit: 100
});
```

Each conflict includes:

- `local_version` / `remote_version`: Version vectors showing what each side had
- `local_payload` / `remote_payload`: The actual data (opaque bytes; deserialize per schema)
- `detected_at_ms`: When sync first saw the conflict
- `correlation_id`: Trace back to the sync message that triggered it

#### Manual Resolution

Once you've determined the correct state, update the record and mark conflict resolved:

```cpp
// Apply the resolved version (sender_node_id optional, defaults to empty)
adapter.apply_record(resolved_record, "manual-resolution");

// Mark conflict as resolved
conflict.resolved = true;
adapter.persist_conflict(conflict);
```

---

### 2.2 Checkpoint and Lag Interpretation

#### Understanding Checkpoints

A checkpoint shows where sync last completed successfully:

```
Checkpoint = (sequence_number, last_record, last_version)
```

| Field             | Meaning                                                                                   |
| ----------------- | ----------------------------------------------------------------------------------------- |
| `sequence_number` | Batch counter. Increments after each successful sync round.                               |
| `last_record`     | Record ID of the last record in that batch. Sync won't re-send records before this.       |
| `last_version`    | The version vector of that last record. Used to detect if state changed since checkpoint. |

#### Measuring Sync Lag

```bash
# On vehicle side
checkpoint = adapter.read_checkpoint(session);
earliest_dirty = adapter.list_dirty_records(session, limit=1)[0];

if (earliest_dirty exists) {
    lag_records = count(adapter.list_dirty_records(session));
    lag_bytes = sum_payload_sizes(adapter.list_dirty_records(session));
    // Report: "X records, Y MB waiting to sync"
}
```

#### Gap Recovery Scenarios

If `compute_state_checksum(scope)` on vehicle differs from cloud's checksum:

1. **No dirty records + checksum mismatch** - Gap recovery triggered
2. **Gap recovery exchanges ID lists** - Finds which records are missing
3. **Fetches only missing records** - Avoids full resync
4. **Resumes from checkpoint** - Doesn't re-send everything

Operators should see:

- Temporary increase in network traffic (ID list exchange, missing record fetch)
- Possible slight latency spike (one-time catch-up)
- Return to normal sync pace after convergence

**Note:** This is v1 behavior. In future versions, fleet-scale gap recovery may require additional strategies.

---

### 2.3 Tombstone Lifecycle and GC

#### When Tombstones Are Created

Every DELETE operation creates a tombstone instead of removing the row:

```
UPDATE records SET operation = kDelete, tombstone_at_ms = now() WHERE record_id = X;
```

**Why:** A vehicle that reconnects after the delete happened needs to see the delete in the record history.

#### Tombstone Retention Policy

Tombstones are retained until:

1. Age exceeds 30 days (configurable: `SYNC_TOMBSTONE_RETENTION_DAYS`)
2. **AND** all remote peers have acknowledged them

#### Garbage Collection

Run GC periodically (e.g., daily):

```cpp
TombstoneGcQuery gc_query;
gc_query.session = {local_node_id, remote_node_id, namespace};
gc_query.retention_cutoff_ms = now() - 30 * 86400 * 1000;  // 30 days
gc_query.limit = 1000;

auto tombstones = adapter.list_tombstones_for_gc(gc_query);
for (const auto& tombstone : tombstones) {
    // Hard-delete from your database
    db.delete_record(tombstone.locator.record_id);
}
```

**Operator concern:** If a peer reconnects with old version history, it might reference a gc'd tombstone. The protocol handles this by requesting gap recovery, which will fetch the current state.

---

### 2.4 Version Vector Interpretation

#### What Version Vectors Tell You

```
version_vector = { cloud_seq: 5, truck_seq: 2 }
```

This record was:

- Modified 5 times by the cloud
- Modified 2 times by the vehicle
- Total 7 edits

#### Dominance and Convergence

```
Local:  { cloud_seq: 5, truck_seq: 2 }
Remote: { cloud_seq: 5, truck_seq: 1 }

Local dominates: YES (both cloud seqs equal, but local truck_seq is higher)
Action: Remote accepts local version
```

```
Local:  { cloud_seq: 5, truck_seq: 2 }
Remote: { cloud_seq: 4, truck_seq: 3 }

Neither dominates: CONFLICT
Action: Both versions preserved in conflict table
```

#### Operator Queries

```bash
# Find all records modified by cloud more than 100 times
adapter.list_dirty_records(query)
  .filter { |r| r.version_vector.cloud_seq > 100 }

# Find concurrent updates (both sides modified same record)
adapter.query_conflicts({conflict_class: CONCURRENT_UPDATE})
```

---

## Part 3: Migration and Coexistence

### 3.1 v1 Scope Boundaries

This is **pilot/prototype** sync for evaluation. In production, the following are NOT handled:

| Capability                       | v1 Status     | Notes                                                         |
| -------------------------------- | ------------- | ------------------------------------------------------------- |
| Single record stream per vehicle | Supported     | One namespace at a time                                       |
| Multi-vehicle fleet scaling      | **NOT IN v1** | Partition/sharding required for 1000+ vehicles                |
| Automatic conflict resolution    | **NOT IN v1** | Conflicts surfaced; application/operator decides              |
| Cross-vehicle peer sync          | **NOT IN v1** | Only cloud-vehicle sync                                       |
| Deployment automation            | **NOT IN v1** | Manual service startup, config management not included        |
| Multi-cloud failover             | **NOT IN v1** | Single cloud endpoint                                         |
| Transport negotiation            | **NOT IN v1** | Protocol is transport-agnostic; v1 reference bridge uses MQTT |

### 3.2 Coexisting with Existing Sync Bridges

During rollout, you may have:

- **Old scheduler-sync-bridge** (scheduler jobs only)
- **New cloud-vehicle-sync-bridge** (generic data)

They can coexist because:

1. Different content IDs on MQTT (scheduler = 202, cloud-vehicle = 203+)
2. Different storage backends (scheduler has its DB, sync has adapters)
3. Different namespace scopes (scheduler records in "scheduler-jobs", sync in "jobs", etc.)

#### Migration Path (Recommended)

**Phase 1: Parallel Run (2-4 weeks)**

- Deploy cloud-vehicle-sync for _new_ data records (different namespace)
- Old scheduler-sync continues for existing job definitions
- Verify conflict surfacing, checkpoint advance, tombstone GC

**Phase 2: Cutover (if successful)**

- Migrate job definitions to cloud-vehicle-sync adapter
- Archive old scheduler-sync database
- Update code to use cloud-vehicle-sync interface

**Phase 3: Decommission (optional)**

- Remove scheduler-sync code if not needed for audit trail
- Consolidate all sync through cloud-vehicle-sync

#### Avoiding Cross-Interference

Use explicit namespaces:

```cpp
// Old scheduler sync
record.locator.namespace_name = "scheduler-jobs";

// New generic sync
record.locator.namespace_name = "cloud-jobs";  // different!
```

Checksums, conflicts, and ACKs are per-namespace, so they won't collide.

---

### 3.3 Operational Checklist for Rollout

#### Pre-Deployment

- [ ] Adapter implementation tested with unit tests
- [ ] Checksum determinism verified (same records -> same hash across runs)
- [ ] Tombstone GC tested locally
- [ ] Conflict query format validated
- [ ] Document ownership matrix for your record types

#### During Deployment

- [ ] Cloud-vehicle-sync service starts, connects to MQTT
- [ ] First vehicle connects, performs initial sync
- [ ] Checkpoint advances after first batch (log shows sequence_number increments)
- [ ] No conflicts in query (if business logic is correct)
- [ ] Checksums converge (vehicle and cloud report same hash)

#### Post-Deployment Monitoring

- [ ] **Lag metric:** No more than 100 dirty records at any time (normal)
- [ ] **Conflict rate:** Zero for normal workload; investigate any non-zero
- [ ] **Gap recovery frequency:** Should be rare (< 1 per day); investigate clusters
- [ ] **Checkpoint sequence:** Monotonically increasing; no reset
- [ ] **Tombstone count:** Stable (GC running, not accumulating indefinitely)

#### Troubleshooting

| Symptom                   | Likely Cause                                       | Action                                   |
| ------------------------- | -------------------------------------------------- | ---------------------------------------- |
| Checkpoint not advancing  | Adapter bug in `write_checkpoint()`                | Check logs, verify atomicity             |
| Conflicts every sync      | Ownership matrix not followed                      | Verify record origin_node_id             |
| Checksum always different | Non-deterministic hasher or schema mismatch        | Verify payload encoding consistency      |
| Lag grows unbounded       | Adapter `list_dirty_records()` bug or network down | Check adapter limits, network status     |
| Tombstones not cleaned    | GC not running or retention policy too new         | Verify GC job scheduled, check cutoff_ms |

---

## Part 4: Command Reference

### Building and Testing

#### Build the sync library

```bash
cd build
cmake -DGRPC_CPP_PLUGIN_EXECUTABLE=/usr/bin/grpc_cpp_plugin ..
make -j$(nproc) ifex-sync-core ifex-sync-adapters-database
```

**Prerequisite:** CMake must find Protobuf development files and gRPC.

#### Run end-to-end sync integration tests

```bash
ctest --test-dir build -R "database_sync_e2e" --output-on-failure
ctest --test-dir build -R "gap_recovery_database_sync_e2e" --output-on-failure
ctest --test-dir build -R "stale_ack_database_sync_e2e" --output-on-failure
ctest --test-dir build -R "malformed_envelope_database_sync_e2e" --output-on-failure
```

**What these test:** Full sync session lifecycle with SQLite adapter backend, including reconnect, gap recovery, ACK durability, and conflict detection.

**Environment:** Requires Docker daemon for MQTT broker. See `.sisyphus/notepads/cloud-truck-sync-protocol/issues.md` for environment limitations.

### Runtime Operations

#### Query conflicts from your adapter

**Pseudo-code (adapt to your adapter API):**

```cpp
ConflictQuery query;
query.namespace_name = "jobs";
query.include_resolved = false;

auto unresolved = adapter->query_conflicts(query);
for (const auto& c : unresolved) {
    std::cout << "Conflict: " << c.locator.record_id
              << " Local v" << c.local_version.cloud_seq
              << " vs Remote v" << c.remote_version.cloud_seq << "\n";
}
```

#### Check sync progress

```cpp
SyncSessionKey session;
session.local_node_id = "truck-001";
session.remote_node_id = "cloud";
session.namespace_name = "jobs";

auto checkpoint = adapter->read_checkpoint(session);
if (checkpoint.found) {
    std::cout << "Last synced: seq " << checkpoint.checkpoint.sequence_number
              << ", record: " << checkpoint.checkpoint.last_record.record_id << "\n";
} else {
    std::cout << "No checkpoint yet (first sync)\n";
}

auto dirty = adapter->list_dirty_records({session, 1000, true});
std::cout << "Pending sends: " << dirty.size() << " records\n";
```

#### Trigger gap recovery (manual, for testing)

```cpp
// Get current state checksum
auto local_checksum = adapter->compute_state_checksum({
    namespace_name: "jobs",
    include_tombstones: true
});

// If this differs from remote's reported checksum, protocol will auto-trigger gap recovery
// No manual step needed; protocol core detects it and requests IDs
```

---

## Summary

**Adapter SPI:** One interface, eleven methods (`list_dirty_records`, `apply_record`, `read_checkpoint`, `write_checkpoint`, `persist_remote_acks`, `list_remote_acks`, `compute_state_checksum`, `list_record_ids`, `persist_conflict`, `query_conflicts`, `list_tombstones_for_gc`). Implement once, get conflict surfacing and resumable sync for free.

**Onboarding:** Reference implementations show in-memory and SQLite patterns. Your domain-specific adapter reuses protocol core logic.

**Operations:** Conflicts, checkpoints, and tombstones are transparent observables. Query them to monitor sync health and detect issues early.

**Migration:** Coexist with old sync bridges during rollout. v1 is prototype; production scale requires additional strategies for fleet partitioning and automatic conflict resolution.

---

## References

- **Protocol Spec:** `reference-specs/protocols/cloud-vehicle-sync-protocol-v1.md`
- **Adapter Headers:** `reference-services/sync/common/include/cloud_vehicle_sync_types.hpp`, `cloud_vehicle_db_adapter.hpp`
- **Reference Adapter:** `reference-services/sync/adapters/database/include/sqlite_cloud_vehicle_db_adapter.hpp`
- **Integration Tests:** `tests/integration/test_cloud_vehicle_sync_bridge_integration.cpp`
- **Learnings & Issues:** `.sisyphus/notepads/cloud-truck-sync-protocol/learnings.md`, `.sisyphus/notepads/cloud-truck-sync-protocol/issues.md`
