# Cloud-Truck Synchronization Protocol v1 Specification

**Version:** 1.0  
**Date:** 2026-03-17  
**Status:** DRAFT  

## 1. Overview

### 1.1 Purpose

This specification defines a bidirectional synchronization protocol between cloud and truck systems that tolerates opportunistic connectivity, unsynchronized clocks, and partial data loss. The protocol maintains eventual consistency via logical ordering and explicit conflict surfacing rather than wall-clock authority.

**Key design:** Logical version vectors determine correctness; wall-clock timestamps are diagnostic only.

### 1.2 Design Principles

| Principle | Approach |
|-----------|----------|
| Opportunistic connectivity | No requirement for continuous connectivity; sync resumes on reconnect |
| Clock independence | Logical version vectors (per-origin sequence) drive all ordering; wall-clock is observational |
| Explicit conflict handling | Conflicts are detected and surfaced, never silently overwritten |
| Idempotent replay | At-least-once delivery with idempotency keys prevents state duplication |
| Domain ownership | Cloud owns planning/config/reference data; truck owns observed operational facts; shared mutations surface conflicts |
| Resumable sessions | Checkpoints enable resumption from last-known state without full rescan |
| Deterministic recovery | Gap detection via checksum mismatch with no dirty records; explicit ID list exchange |

### 1.3 Scope

**In v1:**
- Bidirectional sync of single record type (canonical envelope-wrapped records)
- Per-origin logical versioning with causal comparison
- Idempotent apply with replay detection
- Checkpoint-based resumption
- Conflict surfacing (detection and queryability)
- Gap recovery for checksum mismatch
- Tombstone retention with explicit GC preconditions

**Out of Scope (v1):**
- Cross-truck peer-to-peer replication
- Production fleet scaling (partition fanout, fleet topology)
- Automatic conflict resolution policy (application-specific)
- Exact-once delivery (at-least-once with idempotency is the model)
- Deployment automation or operational orchestration
- Multi-protocol or transport-specific tuning (protocol is transport-agnostic)

## 2. Core Concepts

### 2.1 Canonical Record Identity

Every record synchronized via this protocol is uniquely identified by:

```
Canonical Identity = (record_id, origin_node_id)
```

Where:
- **record_id**: Opaque bytes or string, unique per record type within scope (e.g., job ID, config key)
- **origin_node_id**: Stable identifier for the node that created the record (e.g., "cloud", "truck-001")

### 2.2 Per-Origin Logical Versioning

Each record carries a version vector with per-origin sequence numbers:

```
Version Vector = {
  cloud_seq: uint64,      // Sequence of updates from cloud for this record
  vehicle_seq: uint64,    // Sequence of updates from vehicle for this record
  // (may extend to >2 origins in future)
}
```

**Invariant:** For a given `(record_id, origin)` pair, the sequence is strictly monotonic increasing. Only the origin that created the record can increment its own sequence.

**Example:**
- Record created by cloud: version = {cloud_seq: 1, vehicle_seq: 0}
- Vehicle modifies it: version = {cloud_seq: 1, vehicle_seq: 1}
- Cloud re-modifies: version = {cloud_seq: 2, vehicle_seq: 1}

### 2.3 Authority and Ownership Matrix

```
Record Type                  Authority (Owner)    Can Modify
─────────────────────────────────────────────────────────────
Planning/Config/Reference    CLOUD                Cloud only
Job definition               CLOUD                Cloud only
Job deletion (tombstone)     CLOUD                Cloud only

Observed Operational State   VEHICLE              Vehicle only
Job execution result         VEHICLE              Vehicle only
Job status/last_run_time     VEHICLE              Vehicle only

Shared (rare)                BOTH                 Either (surfaces conflict)
```

When non-owner attempts write on an owned record, a conflict is surfaced.

### 2.4 Canonical Record Envelope

Every record exchanged via the protocol is wrapped in a canonical envelope:

```
Record Envelope = {
  record_id: bytes,                    // Unique identifier
  origin_node_id: string,              // "cloud" or "truck-{id}"
  namespace: string,                   // Scope/type (e.g., "jobs", "config")
  version_vector: VersionVector,       // {cloud_seq, vehicle_seq, ...}
  operation: enum (CREATE | UPDATE | DELETE),  // Intent
  payload: bytes,                      // Opaque record data
  schema_version: uint32,              // For versioning payload format
  
  // Metadata (NOT used for correctness, diagnostic only)
  wall_clock_ms: uint64,               // Sender's wall-clock at send time
  created_at_ms: uint64,               // When record was initially created (reference only)
  updated_at_ms: uint64,               // When record was last changed (reference only)
  
  // Idempotency and correlation
  idempotency_key: string,             // Global dedup key for apply operation
  correlation_id: string,              // For request/response if applicable
  
  // Optional metadata
  checksum_payload: uint64,            // xxHash64 of payload (for integrity check)
  tombstone_at_ms: uint64,             // When record was soft-deleted (if operation=DELETE)
  tombstone_reason: string,            // Why deleted (optional)
}
```

### 2.5 Non-Authoritative Wall-Clock Timestamps

**Critical invariant:** Wall-clock timestamps (`wall_clock_ms`, `created_at_ms`, `updated_at_ms`) NEVER participate in:
- Conflict winner selection
- Version dominance comparison
- Ordering decisions
- Deduplication

These fields exist for:
- Observability (dashboards, logs, audit trails)
- Metadata queries ("When was this record last modified?")
- Diagnostic troubleshooting

**Justification:** Vehicle and cloud clocks are not reliably synchronized. Relying on them creates state divergence under clock skew scenarios.

## 3. Sync Invariants

### 3.1 Logical Ordering Invariant

For any record `(record_id, origin_node_id)`:

```
Local Version ≤ Remote Version  OR  Local Version ≥ Remote Version
```

At any point in time, one side has a version that dominates the other. They converge via:

1. One side accepts the other's version (copy and store)
2. One side updates and increments its own sequence (now dominates)
3. Repeat until local_version == remote_version for all records

### 3.2 Causal Comparison

Given two versions of the same record:

```
Local  = {cloud_seq: A, vehicle_seq: B}
Remote = {cloud_seq: C, vehicle_seq: D}
```

**Dominance:**
- Remote dominates if: (C > A OR D > B) AND NOT(C < A OR D < B)
- Local dominates if: (A > C OR B > D) AND NOT(A < C OR B < D)
- Concurrent if: (A > C AND B < D) OR (A < C AND B > D)

**Outcome:**
- Remote dominates → Accept remote version, store, checkpoint reached
- Local dominates → Send local version; remote will accept and return ack
- Concurrent → CONFLICT (both sides have non-comparable updates)

### 3.3 Quiescence Condition

The protocol reaches quiescence (full sync) when:

```
For all records in scope:
  local_version == remote_version
  AND no dirty records exist
  AND state_checksum(local) == state_checksum(remote)
```

Dirty record = any record that has not been acked by the remote side yet.

## 4. Envelope and Message Types

### 4.1 Sync Exchange Envelope

All sync traffic is wrapped in a directional envelope with explicit type discrimination:

```protobuf
// Direction marker (used for context only; not transmitted)
enum Direction {
  CLOUD_TO_VEHICLE = 0;
  VEHICLE_TO_CLOUD = 1;
}

// Wrapper envelope (direction-neutral payload)
message SyncMessage {
  string origin_node_id = 1;           // Sender identity
  string recipient_node_id = 2;        // Target node (for routing)
  
  // Sync state
  repeated RecordEnvelope records = 3; // Dirty records being sent
  repeated VersionAck acked_records = 4; // ACKs for received records
  uint64 state_checksum = 5;           // Checksum of sender's full state
  
  // Metadata
  uint64 sent_at_ms = 10;              // Sender's wall-clock at send time
  uint32 checkpoint_token = 11;        // Opaque token for resume
  string correlation_id = 12;          // For linking request/response if needed
}

// Version acknowledgment (without full record payload)
message VersionAck {
  bytes record_id = 1;
  string namespace = 2;
  VersionVector version_vector = 3;
  // Acknowledges: "I received and stored your version of this record"
}

// Gap detection message (recovery path only)
message GapDetectMessage {
  string origin_node_id = 1;
  repeated bytes my_record_ids = 2;    // All IDs I have
  repeated bytes requested_record_ids = 3; // IDs I need from you
  
  // Trigger condition
  uint64 my_checksum = 4;              // My current checksum
  uint64 your_last_checksum = 5;       // Checksum you reported last time
  string reason = 6;                   // Diagnostic (e.g., "checksum mismatch")
}

// Conflict record (persisted in storage, surfaced to application)
message ConflictRecord {
  bytes record_id = 1;
  string namespace = 2;
  VersionVector local_version = 3;
  VersionVector remote_version = 4;
  bytes local_payload = 5;             // What we have locally
  bytes remote_payload = 6;            // What remote side sent
  ConflictClass conflict_class = 7;    // Categorization (see below)
  uint64 detected_at_ms = 8;           // When conflict was first seen
  string resolver_note = 9;            // Application-set resolution hint
}

enum ConflictClass {
  CONCURRENT_UPDATE = 0;   // Both sides updated independently (versions incomparable)
  NON_OWNER_MUTATION = 1;  // Non-owner tried to modify owned record
  STALE_REPLAY = 2;        // Received an older version (should not apply)
}

message VersionVector {
  uint64 cloud_seq = 1;
  uint64 vehicle_seq = 2;
}
```

### 4.2 Checkpoint Token

Opaque token incremented after each successful batch of applies. Used to resume sync without full state re-scan:

```
Checkpoint = {
  last_applied_record_id: bytes,       // Last record successfully applied
  last_applied_origin: string,         // Origin of that record
  last_applied_version: VersionVector, // Version at that point
  sequence_number: uint32,             // Monotonic counter
}
```

Receiver persists checkpoint after ACKing records. On reconnect, sender can resume from `checkpoint_token` instead of rescanning all records.

### 4.3 Message Correlation

If the protocol is used in a request-response pattern (e.g., "send me all records for vehicle X"):

```
Request: SyncMessage { correlation_id = "abc-123", ... }
Response: SyncMessage { correlation_id = "abc-123", ... }
```

The `correlation_id` allows callers to match async responses.

## 5. Sync Flow

### 5.1 Fast Path (Dirty Exchange)

Most common case: both sides have some dirty records; one round converges.

```
CLOUD                                           VEHICLE
  │                                               │
  │ dirty: [job-A]                               │ dirty: [job-X]
  │ checksum: 0xAAAA                             │ checksum: 0xBBBB
  │                                               │
  ├──── SyncMessage ─────────────────────────────▶
  │     records: []                               │
  │     acked_records: []                         │
  │     state_checksum: 0xAAAA                    │
  │                                               │
  │                          Mismatch, has dirty  │
  │                          Send dirty (fast)    │
  │                                               │
  ◀──── SyncMessage ─────────────────────────────┤
  │     records: [job-X@{0,1}]  (dirty)           │
  │     acked_records: []                         │
  │     state_checksum: 0xBBBB                    │
  │                                               │
  │ Apply job-X                                  │
  │ job-A still dirty                             │
  │ Send dirty job-A + ACK job-X                  │
  │                                               │
  ├──── SyncMessage ─────────────────────────────▶
  │     records: [job-A@{1,0}]  (dirty)           │
  │     acked_records: [job-X@{0,1}]  (ack)       │
  │     state_checksum: 0xCCCC                    │
  │                                               │
  │                          Apply job-A          │
  │                          Store ACK for job-X  │
  │                                               │
  ◀──── SyncMessage ─────────────────────────────┤
  │     records: []                               │
  │     acked_records: [job-A@{1,0}]  (ack)       │
  │     state_checksum: 0xCCCC                    │
  │                                               │
  │ Store ACK for job-A                          │
  │ All versions acked, checksums match!         │
  │                                               │
  ═══════════════════ QUIESCENT ═══════════════════
```

**Key:** ACKs allow both sides to update their `remote_version` bookkeeping. Convergence happens when `local_version == remote_version` for all records and checksums match.

### 5.2 Recovery Path (Gap Detection)

Triggered when checksums differ but no dirty records exist (indicates data loss or schema mismatch).

```
CLOUD                                           VEHICLE
  │                                               │
  │ checksum: 0xAAAA                             │ checksum: 0xBBBB
  │ no dirty records                              │ no dirty records
  │                                               │
  ├──── GapDetectMessage ────────────────────────▶
  │     my_record_ids: [id1, id2, id3]            │
  │     requested_record_ids: []                  │
  │     my_checksum: 0xAAAA                       │
  │                                               │
  │                          Compare IDs          │
  │                          Missing: [id4]       │
  │                          Extra: [id5]         │
  │                                               │
  ◀──── GapDetectMessage ────────────────────────┤
  │     my_record_ids: [id1, id2, id4, id5]       │
  │     requested_record_ids: [id3]               │
  │     my_checksum: 0xBBBB                       │
  │                                               │
  │ Send missing records: id4, id5                │
  │ (full re-sync of missing)                     │
  │                                               │
  ├──── SyncMessage ─────────────────────────────▶
  │     records: [record-id4, record-id5]         │
  │     ...                                       │
  │                                               │
  │                          Apply, recompute     │
  │                          checksums now match  │
  │                                               │
  ═══════════════════ QUIESCENT ═══════════════════
```

**Trigger:** Checksum mismatch + no dirty records = suspected gap.  
**Action:** Exchange full ID lists, identify missing records, re-sync.

## 6. Idempotency and Replay Semantics

### 6.1 Idempotency Key

Every record apply operation has a globally unique idempotency key:

```
idempotency_key = "{origin_node_id}:{record_id}:{version_vector_hash}"
```

The storage adapter deduplicates applies by this key:
- First apply with key K → accept, store, increment checkpoint
- Duplicate apply with key K → reject as duplicate, return ack without re-applying

### 6.2 Replay Detection and ACKing

```
Receive SyncMessage with records R1, R2:
  1. For each record in R1, R2:
       a. Compute idempotency_key
       b. Check if already applied (lookup in storage)
       c. If yes: mark as acked (no state change)
       d. If no: apply, store new state, mark as acked
  2. After all records processed:
       a. Increment checkpoint_token
       b. Compute new state_checksum
       c. Send back VersionAck for each record
       d. Include new checkpoint_token and state_checksum
```

**Invariant:** Multiple applies of the same record with the same version do not advance state beyond first apply.

### 6.3 ACK Retry Semantics

If receiver sent ACKs but sender did not receive them:

```
Sender still sees record as "dirty" → sends again
Receiver's storage adapter recognizes idempotency_key as duplicate
→ Returns ack without re-applying
Sender advances checkpoint after receiving ack
```

No state corruption, no duplicate records.

## 7. Checkpoint and Resumption

### 7.1 Checkpoint Persistence

After successful batch apply and ack exchange:

```
Checkpoint {
  sequence_number: N+1,
  last_applied_record_id: bytes,
  last_applied_version: VersionVector,
  timestamp_ms: now,
}
```

Receiver stores checkpoint persistently (in adapter storage).

### 7.2 Resume from Checkpoint

On reconnect:

```
Sender (cloud) queries receiver (vehicle) for current checkpoint
Receiver returns: checkpoint_token = N
Sender looks up: "Give me all records changed since checkpoint N"
Sender builds SyncMessage with only dirty records since N
→ Much faster than full state rescan
```

### 7.3 Checkpoint Monotonicity

Checkpoints are monotonically increasing. Receiver never goes backward in checkpoint sequence.

**Invariant:** A checkpoint N+1 implies all records up to N have been durably applied.

## 8. Dirty Records and Changesets

### 8.1 Definition: Dirty Record

A record is "dirty" if:

```
local_version != remote_version_ack
```

Where `remote_version_ack` = the version of this record that the remote side has confirmed receipt of.

### 8.2 Dirty Enumeration (Storage Adapter Contract)

The storage adapter provides method:

```
GetDirtyRecords(namespace: string, limit: int) 
  → List<RecordEnvelope>
```

Returns records where `local_version > remote_version_ack`, up to `limit`.

### 8.3 Fast Path Optimization

If dirty list is small (< N records):
- Send all dirty records in one SyncMessage
- Receiver applies, sends acks
- Converges in 1-2 round trips

If dirty list is large (> N records):
- Send first N, get acks
- Send next N, get acks
- Continue until no more dirty records

## 9. Tombstones and Deletion

### 9.1 Soft Delete via Tombstone

To delete a record, instead of hard-delete:

```
RecordEnvelope {
  operation: DELETE,
  tombstone_at_ms: now,
  tombstone_reason: "user requested deletion",
  // ... other fields same
}
```

Sender increments its sequence: `cloud_seq` or `vehicle_seq`.

### 9.2 Tombstone Visibility and Querying

Tombstones are queryable/visible until retention rules expire:

```
GetAllRecords(include_tombstones: true)
  → returns active records + tombstones

GetTombstonesForGC(max_age_ms: X)
  → returns tombstones older than X milliseconds
```

### 9.3 Tombstone Garbage Collection Safety

Tombstone can be safely garbage-collected only when:

```
1. Tombstone age > retention_period (e.g., 30 days)
   AND
2. All remote nodes have acknowledged the tombstone version
   AND
3. No pending recovery operations exist
```

**Rationale:** If a late-reconnecting peer has not seen the tombstone yet, it needs the full version history to detect the deletion.

### 9.4 Tombstone Retention Period

Default: 30 days (configurable per deployment).

Justification: Allows vehicles disconnected for ~4 weeks to reconnect and learn of deletions.

## 10. Conflict Detection and Surfacing

### 10.1 Conflict Classes

#### 10.1.1 Concurrent Update Conflict

Both cloud and vehicle independently updated the same record:

```
Local  = {cloud_seq: 1, vehicle_seq: 2}  (vehicle updated last)
Remote = {cloud_seq: 2, vehicle_seq: 1}  (cloud updated, but vehicle has old seq)

Neither dominates → CONFLICT
```

**Surfacing:** Create ConflictRecord with both payloads. Application must resolve manually or via policy.

#### 10.1.2 Non-Owner Mutation Conflict

Attempt to modify a record owned by the other party:

```
Record authority = CLOUD (cloud owns it)
Received update from VEHICLE
→ NON_OWNER_MUTATION conflict
```

**Surfacing:** Reject update, create ConflictRecord with reason, do not apply.

#### 10.1.3 Stale Replay Conflict

Received an older version than what we already have:

```
Local  = {cloud_seq: 2, vehicle_seq: 1}
Remote = {cloud_seq: 1, vehicle_seq: 1}  (older cloud_seq)

Remote is dominated by Local → stale, do not apply
```

**Handling:** Return ack (for idempotency), do not change state. May log as diagnostic.

### 10.2 Conflict Record Persistence

After detecting a conflict:

```
INSERT INTO conflicts (
  record_id,
  namespace,
  local_version,
  remote_version,
  local_payload,
  remote_payload,
  conflict_class,
  detected_at_ms
);
```

Application queries conflict table to:
- Identify disputes
- Understand what happened on each side
- Apply manual resolution or merge logic

### 10.3 Conflict Queryability

Adapter exposes:

```
GetConflicts(namespace: string, since_ms: uint64)
  → List<ConflictRecord>

MarkConflictResolved(record_id: bytes, resolution: string)
  → Marks conflict as handled (soft delete in conflict table)
```

## 11. Ownership Invariants and Conflict Classes

### 11.1 Ownership Matrix

| Domain | Owner | Mutators | Conflict Action |
|--------|-------|----------|-----------------|
| Job definition (title, schedule, params) | CLOUD | Cloud only | Reject vehicle updates; surface NON_OWNER_MUTATION |
| Job deleted flag | CLOUD | Cloud only | Reject vehicle tombstones; surface NON_OWNER_MUTATION |
| Job execution status | VEHICLE | Vehicle only | Reject cloud updates; surface NON_OWNER_MUTATION |
| Job last_run_time | VEHICLE | Vehicle only | Reject cloud updates; surface NON_OWNER_MUTATION |
| Shared (if any) | N/A | Both | Surface CONCURRENT_UPDATE; application resolves |

### 11.2 Conflict Resolution Workflow

```
1. Receive record update from remote
2. Check ownership:
     If owner is remote → proceed to version check
     If owner is local → check if remote is origin
        If remote is NOT origin → NON_OWNER_MUTATION conflict
        Create ConflictRecord, do NOT apply
3. Check version:
     If remote dominates local → ACCEPT
     If local dominates remote → REJECT (stale), ack anyway
     If concurrent (incomparable) → CONCURRENT_UPDATE conflict
        Create ConflictRecord, DO NOT apply (wait for resolution)
4. If accepted: apply, increment checkpoint, send ack
5. If conflicted: create ConflictRecord, application resolves, checkpoint advances
```

## 12. State Checksum Computation

### 12.1 Checksum Scope

Checksums cover logical record state only; exclude:
- Wall-clock timestamps (created_at_ms, updated_at_ms, wall_clock_ms)
- Idempotency keys
- Correlation IDs
- Conflict records (independent of active records)

### 12.2 Checksum Algorithm

```
For each record (sorted by record_id, namespace):
  1. Extract: {record_id, origin, version_vector, operation, payload}
  2. Compute: hash = xxHash64(serialized_record)
  3. Combine: running_checksum = xxHash64(running_checksum ^ hash)

Final checksum = running_checksum
```

**Invariant:** Identical logical state → identical checksum, regardless of application order or wall-clock values.

### 12.3 Checksum Mismatch Triggers

When `local_checksum != remote_checksum`:

```
If dirty records exist:
  → Fast path: send dirty records, receiver applies, recompute
  
If NO dirty records exist:
  → Recovery path: exchange ID lists (GapDetectMessage)
     Identify missing/extra records
     Re-sync missing ones
```

## 13. Out of Scope (v1)

Explicitly NOT included in v1 (planned for future versions or different protocols):

- **Cross-truck replication:** Truck-to-truck sync not addressed; only cloud-truck
- **Fleet partition scaling:** No orchestration of sync fanout across thousands of vehicles
- **Automatic conflict resolution policies:** Each deployment chooses its own policy; this spec only surfaces conflicts
- **Exactly-once semantics:** Model is at-least-once with idempotency; some ordering edge cases may allow re-apply of benign operations
- **Multi-protocol transport:** Spec is transport-agnostic (gRPC, MQTT, AMQP, etc. all valid); specific bindings in separate docs
- **Encryption or signing:** Security assumed at transport layer (TLS, MQTT TLS, etc.)
- **Rate limiting or throttling:** Deployment-specific; not addressed in protocol
- **Metrics and observability APIs:** Monitoring/alerting is storage adapter responsibility

## 14. Adapter SPI Contract (Preview)

The storage adapter implements:

```cpp
class StorageAdapter {
public:
  // Apply a record with idempotency check
  ApplyResult ApplyRecord(
    const RecordEnvelope& record,
    const std::string& idempotency_key);
  
  // Get dirty records since last checkpoint
  std::vector<RecordEnvelope> GetDirtyRecords(
    const std::string& namespace,
    const CheckpointToken& from_checkpoint,
    int limit);
  
  // Compute checksum of all records in namespace
  uint64_t ComputeChecksum(const std::string& namespace);
  
  // Get all record IDs in namespace (for gap detection)
  std::vector<bytes> GetAllRecordIds(const std::string& namespace);
  
  // Persist a conflict record
  void StoreConflict(const ConflictRecord& conflict);
  
  // Query conflicts
  std::vector<ConflictRecord> GetConflicts(
    const std::string& namespace,
    uint64_t since_ms);
  
  // Checkpoint ops
  CheckpointToken GetCurrentCheckpoint();
  void AdvanceCheckpoint(const CheckpointToken& new_checkpoint);
  
  // Tombstone queries
  std::vector<RecordEnvelope> GetTombstones(
    const std::string& namespace,
    uint64_t max_age_ms);
};
```

Full adapter SPI defined in Task 2 (cloud-truck-db-adapter-spec-v1.md).

## 15. Summary: Protocol Invariants

1. **Logical Ordering Invariant:** All order decisions use version vectors, never wall-clock timestamps
2. **Ownership Invariant:** Non-owner mutations surface as conflicts; not silently accepted
3. **Monotonic Versioning Invariant:** For each (record_id, origin), sequence is strictly increasing
4. **Idempotency Invariant:** Multiple applies with same idempotency key do not duplicate state
5. **Quiescence Invariant:** Sync is complete when all versions are acked and checksums match
6. **Checkpoint Invariant:** Checkpoints are monotonic; no backward movement
7. **Tombstone Invariant:** Tombstones queryable until retention preconditions met
8. **Conflict Surfacing Invariant:** No silent overwrites; all conflicts explicitly recorded and queryable

---

## References

- **Version Vector Semantics:** Inspired by Lamport logical clocks and vector clocks (Mattern, Fidge)
- **Checksum Design:** xxHash64 for speed and determinism (no cryptographic guarantee needed; integrity via transport)
- **Conflict Handling:** Authority-based resolution (similar to CRDT approach with ownership domains)
- **At-Least-Once Delivery:** Idempotency keys ensure safe replay (Kafka, RabbitMQ pattern)
- **Soft Delete via Tombstones:** Standard eventual-consistency technique (Dynamo, Cassandra)
