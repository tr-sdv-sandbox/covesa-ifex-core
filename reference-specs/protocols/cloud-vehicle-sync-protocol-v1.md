# Cloud-Vehicle Synchronization Protocol v1 Specification

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
Canonical Identity = (record_id, namespace_name, origin_node_id)
```

Where:
- **record_id**: Opaque bytes or string, unique per record type within scope (e.g., job ID, config key)
- **namespace_name**: Logical scope for the record stream (e.g., "jobs", "config")
- **origin_node_id**: Stable identifier for the node that created the record (e.g., "cloud", "truck-001")

### 2.2 Per-Origin Logical Versioning

Each record carries a version vector with per-origin sequence numbers:

```
Version Vector = {
  cloud_seq: uint64,      // Sequence of updates from cloud for this record
  truck_seq: uint64,      // Sequence of updates from vehicle for this record
  // (may extend to >2 origins in future)
}
```

**Invariant:** For a given `(record_id, origin)` pair, the sequence is strictly monotonic increasing. Only the origin that created the record can increment its own sequence.

**Example:**
- Record created by cloud: version = {cloud_seq: 1, truck_seq: 0}
- Vehicle modifies it: version = {cloud_seq: 1, truck_seq: 1}
- Cloud re-modifies: version = {cloud_seq: 2, truck_seq: 1}

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
  locator: {
    record_id: bytes,
    namespace_name: string,
    origin_node_id: string,
  },
  version_vector: VersionVector,       // {cloud_seq, truck_seq}
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
  payload_checksum: uint64,            // Optional payload-integrity helper
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
Local  = {cloud_seq: A, truck_seq: B}
Remote = {cloud_seq: C, truck_seq: D}
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

### 4.1 Top-Level Envelope: CloudVehicleSyncEnvelope

All sync traffic on the wire is wrapped in a single top-level envelope message with explicit **oneof** type discrimination. This ensures:
- Type-safe message routing (receiver knows message category before unmarshalling)
- Backward compatibility (new message types can be added as new oneof cases)
- Clear protocol state machine (each case has distinct semantics and handlers)

**Canonical wire envelope structure (from proto/internal/cloud-vehicle-sync-envelope.proto):**

```protobuf
message CloudVehicleSyncEnvelope {
  oneof message {
    SyncExchange sync_exchange = 1;              // Fast path: dirty records + acks + checkpoint
    CheckpointAdvance checkpoint_advance = 2;   // Durable ACK persistence (separate from sync)
    GapRecoveryRequest gap_recovery_request = 3;  // Recovery path: ID list exchange
    GapRecoveryResponse gap_recovery_response = 4; // Recovery path: ID list response
  }
}
```

**Reception logic:**
- Receiver unmarshals outer CloudVehicleSyncEnvelope
- Inspects which oneof case is set
- Dispatches to appropriate handler (SyncExchange handler, CheckpointAdvance handler, etc.)
- Each message type carries its own sender_node_id and correlation_id for routing and validation

The constituent message types are:

```protobuf
// Canonical wire envelope structure (from proto/internal/cloud-vehicle-sync-envelope.proto)

message CheckpointToken {
  uint64 sequence_number = 1;  // Monotonic batch counter
  RecordLocator last_record = 2;  // Last record processed at this checkpoint
  VersionVector last_version = 3;  // Version vector of that record
}

message RecordLocator {
  bytes record_id = 1;
  string namespace_name = 2;
  string origin_node_id = 3;  // Creator identity (cloud or vehicle)
}

message RecordEnvelope {
  RecordLocator locator = 1;
  VersionVector version_vector = 2;
  RecordOperation operation = 3;  // CREATE, UPDATE, DELETE
  bytes payload = 4;
  uint32 schema_version = 5;
  string idempotency_key = 6;
  string correlation_id = 7;
  uint64 payload_checksum = 8;
  uint64 wall_clock_ms = 9;
  uint64 created_at_ms = 10;
  uint64 updated_at_ms = 11;
  uint64 tombstone_at_ms = 12;
  string tombstone_reason = 13;
}

message VersionAck {
  RecordLocator locator = 1;  // Which record is acked
  VersionVector version_vector = 2;  // Up to which version
  string correlation_id = 3;
  string idempotency_key = 4;
}

message SyncExchange {
  string sender_node_id = 1;  // Authenticated peer identity (validated at bridge boundary)
  string recipient_node_id = 2;
  repeated RecordEnvelope records = 3;  // Dirty records being sent
  repeated VersionAck acked_records = 4;  // Acknowledgments of received records
  uint64 state_checksum = 5;  // Checksum of sender's full state
  CheckpointToken checkpoint = 6;  // Sync progress for resume
  string correlation_id = 7;
  string idempotency_key = 8;
}

message CheckpointAdvance {
  string sender_node_id = 1;  // Authenticated peer identity
  string recipient_node_id = 2;
  repeated VersionAck durable_acks = 3;  // Persisted ACK set
  CheckpointToken durable_checkpoint = 4;  // Durable checkpoint state
  uint64 state_checksum = 5;
  string correlation_id = 6;
  string idempotency_key = 7;
}

message GapRecoveryRequest {
  string sender_node_id = 1;  // Authenticated peer identity
  string recipient_node_id = 2;
  repeated RecordLocator record_ids = 3;  // All IDs sender has
  repeated RecordLocator requested_records = 4;  // IDs sender needs from recipient
  uint64 local_state_checksum = 5;  // Sender's current checksum
  uint64 remote_state_checksum = 6;  // Last known remote checksum
  string reason = 7;  // Diagnostic context
  string correlation_id = 8;
}

message GapRecoveryResponse {
  string sender_node_id = 1;  // Authenticated peer identity
  string recipient_node_id = 2;
  repeated RecordLocator record_ids = 3;  // All IDs sender has
  repeated RecordLocator requested_records = 4;  // IDs recipient requested
  uint64 local_state_checksum = 5;  // Sender's current checksum
  uint64 remote_state_checksum = 6;  // Last known remote checksum
  string correlation_id = 7;
}

message CloudVehicleSyncEnvelope {
  oneof message {
    SyncExchange sync_exchange = 1;
    CheckpointAdvance checkpoint_advance = 2;
    GapRecoveryRequest gap_recovery_request = 3;
    GapRecoveryResponse gap_recovery_response = 4;
  }
}

enum RecordOperation {
  RECORD_OPERATION_CREATE = 0;
  RECORD_OPERATION_UPDATE = 1;
  RECORD_OPERATION_DELETE = 2;
}

enum ConflictClass {
  CONFLICT_CLASS_CONCURRENT_UPDATE = 0;   // Both sides updated independently
  CONFLICT_CLASS_NON_OWNER_MUTATION = 1;  // Non-owner attempted write
  CONFLICT_CLASS_STALE_REPLAY = 2;        // Older version than local state
}

message VersionVector {
  uint64 cloud_seq = 1;
  uint64 truck_seq = 2;  // truck sequence
}
```

### 4.2 sender_node_id: Peer Identity at Bridge Boundary

The `sender_node_id` field present in **all** message types (SyncExchange, CheckpointAdvance, GapRecoveryRequest, GapRecoveryResponse) represents the **authenticated peer identity** established at the bridge boundary. It is **not** derived from or validated against record content.

**Key distinction:**
- **`sender_node_id`**: Authenticated peer identity (e.g., mTLS certificate CN, OAuth2 subject). Validated at bridge entry. Same for all messages from that peer in a session.
- **`record.locator.origin_node_id`**: Record creator identity (immutable metadata attached to record). May differ from sender_node_id if records flow through intermediaries or caches.

**Bridge validation logic:**
1. Incoming message `sender_node_id` MUST match authenticated peer identity (reject if mismatch)
2. Ownership checks compare `record.locator.origin_node_id` against authority matrix (not against sender_node_id)
3. Example: Cloud bridge receives SyncExchange with sender_node_id="truck-001", records include origin_node_id="truck-001" → passes ownership
4. Example: Cloud bridge receives SyncExchange with sender_node_id="truck-001", records include origin_node_id="cloud" → cloud record from non-cloud source, fails ownership check

**Implication:** A truck can forward a cloud-created record (origin_node_id="cloud") if authorized, but sender_node_id still reflects the truck as the immediate peer sending the message.

### 4.3 Message Types: Fast Path vs. Durability

The proto model distinguishes two primary message types by concern:

**SyncExchange** (fast path):
- **Purpose:** Bidirectional record exchange in normal operation
- **Contents:** Dirty records (to send), acked records (confirmations), state_checksum, checkpoint (for resume)
- **Checkpoint field:** Included but optional; checkpoint_advance message used for explicit durable persistence
- **Delivery model:** At-least-once (best effort, may be lost)
- **When used:** Fast sync when connectivity is available; sending/receiving dirty records

**CheckpointAdvance** (durable persistence):
- **Purpose:** Explicitly persist acknowledgments and checkpoints to storage
- **Contents:** durable_acks (persisted ACK set), durable_checkpoint (persisted checkpoint), state_checksum
- **Separation:** ACK durability is decoupled from SyncExchange messages
- **Delivery model:** Must be durably persisted by receiver before acknowledging
- **When used:** After a burst of SyncExchange messages, to ensure ACK/checkpoint state survives restart

**RecoveryMessages** (GapRecoveryRequest/Response):
- **Purpose:** Resume after suspected data loss or checksum mismatch
- **Contents:** Full record ID lists from both peers, checksums for validation
- **Trigger:** Checksum mismatch + no dirty records = gap detection
- **Flow:** Request ID lists → compare → identify missing → re-sync via SyncExchange

This separation ensures:
- Fast path doesn't force synchronous durable writes on every message
- ACK/checkpoint durability is explicit and testable
- Recovery is orthogonal to normal sync flow

### 4.4 Checkpoint Token Fields

Checkpoint token contains explicit fields (not opaque):

```
CheckpointToken {
  sequence_number,     // Monotonic batch counter (never decreases)
  last_record,         // Locator of last applied record
  last_version,        // Version vector at that point
}
```

This enables resume from exact position without full state rescan.

### 4.5 Message Correlation

All message types carry a `correlation_id` field enabling async request-response matching when used in request-response patterns (e.g., "send me all records for vehicle X"):

```
Request: SyncExchange { correlation_id = "abc-123", sender_node_id = "cloud", ... }
Response: SyncExchange { correlation_id = "abc-123", sender_node_id = "truck-001", ... }
```

The `correlation_id` allows callers to match async responses across network boundaries.

## 5. Sync Flow

### 5.1 Fast Path (Dirty Exchange)

Most common case: both sides have some dirty records; one round converges.

```
CLOUD                                           VEHICLE
  │                                               │
  │ dirty: [job-A]                               │ dirty: [job-X]
  │ checksum: 0xAAAA                             │ checksum: 0xBBBB
  │                                               │
  ├──── SyncExchange ────────────────────────────▶
  │     sender_node_id: "cloud"                   │
  │     records: []                               │
  │     acked_records: []                         │
  │     state_checksum: 0xAAAA                    │
  │                                               │
  │                          Mismatch, has dirty  │
  │                          Send dirty (fast)    │
  │                                               │
  ◀──── SyncExchange ────────────────────────────┤
  │     sender_node_id: "truck-001"               │
  │     records: [job-X@{0,1}]  (dirty)           │
  │     acked_records: []                         │
  │     state_checksum: 0xBBBB                    │
  │                                               │
  │ Apply job-X                                  │
  │ job-A still dirty                             │
  │ Send dirty job-A + ACK job-X                  │
  │                                               │
  ├──── SyncExchange ────────────────────────────▶
  │     sender_node_id: "cloud"                   │
  │     records: [job-A@{1,0}]  (dirty)           │
  │     acked_records: [job-X@{0,1}]  (ack)       │
  │     state_checksum: 0xCCCC                    │
  │                                               │
  │                          Apply job-A          │
  │                          Store ACK for job-X  │
  │                                               │
  ◀──── SyncExchange ────────────────────────────┤
  │     sender_node_id: "truck-001"               │
  │     records: []                               │
  │     acked_records: [job-A@{1,0}]  (ack)       │
  │     state_checksum: 0xCCCC                    │
  │                                               │
  │ Store ACK for job-A                          │
  │ All versions acked, checksums match!         │
  │                                               │
  ═══════════════════ QUIESCENT ═══════════════════
```

**Key:** ACKs allow both sides to persist remote acknowledgments. Convergence happens when `local_version == remote_version` for all records and checksums match.

### 5.2 Recovery Path (Gap Detection)

Triggered when checksums differ but no dirty records exist (indicates data loss or schema mismatch).

```
CLOUD                                           VEHICLE
  │                                               │
  │ checksum: 0xAAAA                             │ checksum: 0xBBBB
  │ no dirty records                              │ no dirty records
  │                                               │
  ├──── GapRecoveryRequest ──────────────────────▶
  │     sender_node_id: "cloud"                   │
  │     record_ids: [id1, id2, id3]               │
  │     requested_records: []                     │
  │     local_state_checksum: 0xAAAA              │
  │                                               │
  │                          Compare IDs          │
  │                          Missing: [id4]       │
  │                          Extra: [id5]         │
  │                                               │
  ◀──── GapRecoveryResponse ─────────────────────┤
  │     sender_node_id: "truck-001"               │
  │     record_ids: [id1, id2, id4, id5]          │
  │     requested_records: [id3]                  │
  │     local_state_checksum: 0xBBBB              │
  │                                               │
  │ Send missing records: id4, id5                │
  │ (full re-sync of missing)                     │
  │                                               │
  ├──── SyncExchange ────────────────────────────▶
  │     sender_node_id: "cloud"                   │
  │     records: [record-id4, record-id5]         │
  │     ...                                       │
  │                                               │
  │                          Apply, recompute     │
  │                          checksums now match  │
  │                                               │
  ═══════════════════ QUIESCENT ═══════════════════
```

**Trigger:** Checksum mismatch + no dirty records = suspected gap.  
**Action:** Exchange full ID lists via GapRecoveryRequest/Response, identify missing records, re-sync via SyncExchange.

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
Receive SyncExchange with records R1, R2:
  1. For each record in R1, R2:
       a. Compute idempotency_key
       b. Check if already applied (lookup in storage)
       c. If yes: mark as acked (no state change)
       d. If no: apply, store new state, mark as acked
  2. After all records processed:
       a. Increment CheckpointToken.sequence_number
       b. Compute new state_checksum
       c. Send back VersionAck for each record
       d. Include new CheckpointToken and state_checksum in next SyncExchange message
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
  last_record: RecordLocator,
  last_version: VersionVector,
}
```

Receiver stores checkpoint persistently (in adapter storage).

### 7.2 Resume from Checkpoint

On reconnect:

```
Sender (cloud) queries receiver (vehicle) for current checkpoint
Receiver returns: CheckpointToken with sequence_number = N
Sender looks up: "Give me all records changed since CheckpointToken.sequence_number N"
Sender builds SyncExchange with only dirty records (those not acked by receiver) since N
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
list_dirty_records(query) 
  → List<RecordEnvelope>
```

Returns records where `local_version > remote_version_ack`, up to `limit`.

### 8.3 Fast Path Optimization

If dirty list is small (< N records):
- Send all dirty records in one SyncExchange
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

Sender increments its sequence: `cloud_seq` or `truck_seq`.

### 9.2 Tombstone Visibility and Querying

Tombstones are queryable/visible until retention rules expire:

```
list_tombstones_for_gc(TombstoneGcQuery{...})
  → returns tombstones eligible for garbage collection

query policy can be configured to include namespace filters and age thresholds
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
Local  = {cloud_seq: 1, truck_seq: 2}  (vehicle updated last)
Remote = {cloud_seq: 2, truck_seq: 1}  (cloud updated, but vehicle has old seq)

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
Local  = {cloud_seq: 2, truck_seq: 1}
Remote = {cloud_seq: 1, truck_seq: 1}  (older cloud_seq)

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
query_conflicts(ConflictQuery{namespace_name, include_resolved, ...})
  → List<ConflictRecord>

persist_conflict(conflict_with_resolved_flag)
  → Upserts conflict state, including resolved lifecycle updates
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
2. Compute and combine using deterministic FNV-1a style mixing over logical fields
3. Continue in deterministic order for all scoped records

Final checksum = running_checksum
```

**Invariant:** Identical logical state → identical checksum, regardless of application order or wall-clock values.

### 12.3 Checksum Mismatch Triggers

When `local_checksum != remote_checksum`:

```
If dirty records exist:
  → Fast path: send dirty records, receiver applies, recompute
  
If NO dirty records exist:
  → Recovery path: exchange ID lists (GapRecoveryRequest/GapRecoveryResponse)
     Identify missing/extra records
     Re-sync missing ones
```

## 13. Out of Scope (v1)

Explicitly NOT included in v1 (planned for future versions or different protocols):

- **Cross-vehicle replication:** Vehicle-to-vehicle sync not addressed; only cloud-vehicle
- **Fleet partition scaling:** No orchestration of sync fanout across thousands of vehicles
- **Automatic conflict resolution policies:** Each deployment chooses its own policy; this spec only surfaces conflicts
- **Exactly-once semantics:** Model is at-least-once with idempotency; some ordering edge cases may allow re-apply of benign operations
- **Multi-protocol transport:** Spec is transport-agnostic (gRPC, MQTT, AMQP, etc. all valid); specific bindings in separate docs. MQTT is a reference bridge runtime for v1, not the canonical protocol.
- **Encryption or signing:** Security assumed at transport layer (TLS, MQTT TLS, etc.)
- **Rate limiting or throttling:** Deployment-specific; not addressed in protocol
- **Metrics and observability APIs:** Monitoring/alerting is storage adapter responsibility

## 14. Adapter SPI Contract (Preview)

The storage adapter implements:

```cpp
class CloudVehicleDbAdapter {
public:
  // Apply a record with idempotency check and sender validation
  ApplyResult apply_record(
    const CanonicalRecord& record,
    const std::string& idempotency_key,
    const std::string& sender_node_id = "");
   
   // Get records not yet acked by remote peer (dirty records)
   std::vector<CanonicalRecord> list_dirty_records(
     const DirtyRecordQuery& query);
  
  // Compute checksum of all records in namespace
  uint64_t compute_state_checksum(const StateScope& scope);
  
  // Get all record IDs in namespace (for gap detection)
  std::vector<RecordLocator> list_record_ids(
    const RecordIdQuery& query);
  
  // Persist a conflict record
  void persist_conflict(const ConflictRecord& conflict);
  
  // Query conflicts
  std::vector<ConflictRecord> query_conflicts(
    const ConflictQuery& query);
  
  // Checkpoint ops
  CheckpointReadResult read_checkpoint(const SyncSessionKey& session);
  void write_checkpoint(const SyncSessionKey& session,
    const CheckpointToken& checkpoint);
  
  // Durable ACK persistence (separate from checkpoints)
  void persist_remote_acks(const SyncSessionKey& session,
    const std::vector<VersionAck>& acks);
  std::vector<VersionAck> list_remote_acks(
    const SyncSessionKey& session);
  
  // Tombstone queries
  std::vector<CanonicalRecord> list_tombstones_for_gc(
    const TombstoneGcQuery& query);
};
```

Full adapter SPI defined in `cloud-vehicle-db-adapter-spec-v1.md`.

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
- **Checksum Design:** deterministic FNV-1a style logical-state hashing (no cryptographic guarantee needed; integrity via transport)
- **Conflict Handling:** Authority-based resolution (similar to CRDT approach with ownership domains)
- **At-Least-Once Delivery:** Idempotency keys ensure safe replay (Kafka, RabbitMQ pattern)
- **Soft Delete via Tombstones:** Standard eventual-consistency technique (Dynamo, Cassandra)
