# Cloud-Vehicle Database Adapter Specification v1

**Version:** 1.0  
**Date:** 2026-03-17  
**Status:** DRAFT

## 1. Overview

### 1.1 Purpose

This specification defines the persistence contract for a cloud-vehicle synchronization adapter. The adapter owns durable storage, replay safety, checkpoint persistence, conflict persistence, tombstone retention, and deterministic state queries while remaining independent of transport bindings and database engine details.

### 1.2 Scope

The adapter contract covers:

- Canonical record persistence for cloud-vehicle sync
- Dirty-record enumeration for fast-path sync
- Idempotent apply of inbound records
- Durable checkpoint read/write per sync session
- Deterministic checksum and record-ID listing for quiescence and gap recovery
- Durable conflict persistence and queryability
- Tombstone visibility and garbage-collection eligibility
- Contract-test obligations for every implementation

The adapter contract does not define:

- Transport behavior or topic/routing metadata
- Storage-engine tuning or schema dialect choices
- Conflict resolution policy beyond durable surfacing
- Runtime orchestration outside the persistence boundary

## 2. Responsibilities

An implementation of the database adapter SPI MUST:

1. Persist canonical records keyed by logical identity, not wall-clock recency.
2. Persist enough bookkeeping to enumerate records whose local version is not yet acknowledged by the remote session.
3. Make inbound applies idempotent by idempotency key.
4. Persist conflict records durably and expose query APIs for them.
5. Persist checkpoints durably and never move a checkpoint backward.
6. Compute checksums from logical record state only.
7. List record identifiers deterministically for gap recovery.
8. Retain tombstones until garbage-collection preconditions are satisfied.

## 3. Canonical Persistence Model

The adapter MUST persist the following logical field groups. These are correctness-critical fields, not a prescribed table layout.

### 3.1 Canonical Record Fields

Each logical record MUST retain:

| Field | Purpose |
|-------|---------|
| `record_id` | Opaque record identifier |
| `namespace` | Record scope/type |
| `origin_node_id` | Stable creator identity |
| `version_vector.cloud_seq` | Cloud logical sequence |
| `version_vector.truck_seq` | Truck logical sequence |
| `operation` | `CREATE`, `UPDATE`, or `DELETE` |
| `payload` | Opaque application bytes |
| `schema_version` | Payload schema discriminator |
| `payload_checksum` | Optional payload-integrity helper |
| `wall_clock_ms` | Diagnostic only |
| `created_at_ms` | Diagnostic only |
| `updated_at_ms` | Diagnostic only |
| `tombstone_at_ms` | Soft-delete timestamp for retention timing |
| `tombstone_reason` | Optional delete context |

### 3.2 Session Bookkeeping Fields

For each sync session `(local_node_id, remote_node_id, namespace)` the adapter MUST retain:

| Field | Purpose |
|-------|---------|
| `remote_ack_cloud_seq` | Last cloud sequence confirmed by the remote |
| `remote_ack_truck_seq` | Last truck sequence confirmed by the remote |
| `checkpoint.sequence_number` | Monotonic durable resume position |
| `checkpoint.last_record_id` | Last durably applied record at that checkpoint |
| `checkpoint.last_origin_node_id` | Origin of that record |
| `checkpoint.last_namespace` | Namespace covered by checkpoint |
| `checkpoint.last_version_vector` | Last applied logical version |

The adapter MAY store additional local indexes or caches, but those fields MUST NOT change logical behavior.

### 3.3 Idempotency Ledger Fields

The adapter MUST persist enough information to deduplicate an inbound apply by `idempotency_key` across reconnects and process restarts:

| Field | Purpose |
|-------|---------|
| `idempotency_key` | Replay-safe dedupe key |
| `record_id` / `namespace` / `origin_node_id` | Logical record locator |
| `applied_version_vector` | Version tied to that key |
| `apply_outcome` | Applied, duplicate, stale, or conflict-persisted |

### 3.4 Conflict Fields

Each persisted conflict MUST retain:

| Field | Purpose |
|-------|---------|
| `record_id` | Conflicted record |
| `namespace` | Scope/type |
| `origin_node_id` | Canonical identity component |
| `local_version` | Version already stored locally |
| `remote_version` | Version presented by remote |
| `local_payload` | Local bytes at conflict time |
| `remote_payload` | Remote bytes at conflict time |
| `conflict_class` | Concurrent update, non-owner mutation, stale replay, or implementation-defined future extension |
| `detected_at_ms` | Diagnostic timestamp |
| `correlation_id` | Optional request/response correlation |
| `resolution_state` | Open or resolved |

## 4. Dirty Enumeration

### 4.1 Definition

A record is dirty for a given sync session when its locally stored version is not equal to the last version acknowledged by that remote session.

### 4.2 Contract

`list_dirty_records` MUST:

- filter by sync session and optional limit
- return records in deterministic order
- include tombstones until they are garbage-collected
- exclude records fully acknowledged by the remote session
- remain stable across repeated reads when state has not changed

Dirty enumeration MUST depend on logical version bookkeeping, not wall-clock update times.

## 5. Idempotent Apply

### 5.1 Inbound Apply Rules

`apply_record` MUST:

1. Check the `idempotency_key` before mutating durable state.
2. Return a duplicate-safe result when the same key is seen again.
3. Persist canonical state only when the incoming record should be accepted.
4. Persist a conflict record instead of silently overwriting when the incoming record surfaces a conflict.
5. Avoid advancing checkpoints implicitly; checkpoint advancement is a separate explicit durable action.

### 5.2 Accepted Outcomes

An apply result MUST distinguish at least these outcomes:

- `APPLIED`: inbound record became the durable canonical state
- `DUPLICATE`: same idempotency key was already processed
- `STALE_REJECTED`: inbound version was dominated by local state
- `NON_OWNER_REJECTED`: inbound write violated ownership rules
- `CONFLICT_PERSISTED`: inbound record was not applied and a conflict was stored

## 6. Checkpoint Handling

### 6.1 Durability Rules

Checkpoint storage MUST be separate from record apply so the core protocol can:

- apply a batch of records
- persist ACK bookkeeping
- write a single durable checkpoint after the batch becomes safe to resume from

### 6.2 Monotonicity

`write_checkpoint` MUST reject or ignore backward movement. A checkpoint with a lower `sequence_number` than the current durable checkpoint MUST NOT replace it.

### 6.3 Read Semantics

`read_checkpoint` MUST return the last durable checkpoint for the requested sync session, or no value when the session has no prior progress.

## 7. ACK Persistence (Separate from Checkpoint)

Durable ACK tracking is distinct from checkpoints. The adapter MUST support persistent storage and retrieval of remote acknowledgments:

- `persist_remote_acks(session, acks)` MUST durably store which record versions have been confirmed by the remote peer.
- `list_remote_acks(session)` MUST return all persisted acks for a session on restart, enabling the bridge to rebuild in-memory state without losing acknowledgment history.

ACKs and checkpoints are managed separately so the bridge can detect and skip already-acked records across process restarts.

## 8. Checksum and Record-ID Listing

### 7.1 Checksum Scope

`compute_state_checksum` MUST include only logical state:

- record identity
- namespace
- origin node
- version vector
- operation
- payload
- schema version
- tombstone state

It MUST exclude:

- wall-clock metadata
- idempotency ledger contents
- checkpoint values
- conflict records
- correlation metadata

### 8.1 Determinism

The checksum result MUST be deterministic for identical logical state regardless of read order or storage layout.

### 8.2 Record-ID Listing

`list_record_ids` MUST:

- return canonical record locators in deterministic order
- support inclusion of tombstones when requested
- expose the full logical set used during gap recovery

## 9. Conflict Persistence and Query

### 9.1 Persistence

When a conflict is surfaced by the protocol core or adapter-owned ownership validation, the adapter MUST store the conflict durably before reporting success to the caller.

### 9.2 Queryability

`query_conflicts` MUST support querying by namespace and time range and MAY allow additional filters such as resolution state or record locator.

Conflict query results MUST include enough detail for an application or test harness to verify:

- which record conflicted
- what each side believed the version to be
- what payloads were in dispute
- why the conflict was surfaced

## 10. Tombstone Lifecycle and Garbage Collection

### 10.1 Tombstone Creation

Delete operations MUST be represented as canonical records with `operation=DELETE`. The adapter MUST preserve the tombstone record rather than removing the logical row immediately.

### 10.2 Visibility

Tombstones MUST remain visible to:

- dirty enumeration
- checksum computation
- record-ID listing when tombstones are included
- contract-test queries that verify retention behavior

### 10.3 GC Eligibility

A tombstone is GC-eligible only when all of the following are true:

1. the tombstone age is past the configured retention cutoff
2. every relevant remote session has acknowledged the tombstone version
3. no active gap-recovery workflow still depends on it for reconciliation

### 10.4 GC Contract

The SPI only exposes candidate enumeration. An implementation MAY provide a separate purge mechanism later, but v1 requires at minimum that the adapter can list tombstones that are safe to collect without guessing.

## 11. SPI Contract Summary

The v1 SPI MUST expose operations for:

- dirty-record enumeration
- idempotent apply (with sender validation via sender_node_id)
- checkpoint read/write
- durable ACK persistence and retrieval
- checksum computation
- record-ID listing
- conflict persistence/query
- tombstone candidate listing for GC

The SPI MUST remain transport-neutral and storage-engine-neutral.

## 12. Contract-Test Obligations

Every adapter implementation MUST pass the same contract suite. The suite MUST verify at least:

1. **Dirty enumeration** returns only records whose local version differs from the stored remote acknowledgment.
2. **Idempotent apply** applies once and returns duplicate-safe results on replay.
3. **Checkpoint monotonicity** never allows a lower sequence checkpoint to replace a higher one.
4. **Checksum determinism** returns identical values for identical logical state across repeated reads.
5. **Record-ID listing** returns deterministic locator sets and includes tombstones when requested.
6. **Conflict persistence** stores exactly one durable conflict record for each surfaced conflict event.
7. **Conflict query** returns enough detail to inspect local and remote versions and payloads.
8. **Tombstone retention** keeps tombstones visible until acknowledgment and retention preconditions are both satisfied.
9. **Tombstone GC candidacy** never returns an unacknowledged tombstone as safe to collect.
10. **No transport leakage** confirms the contract does not require topics, partitions, brokers, or storage-engine-specific fields.

## 13. Out of Scope

The following remain out of scope for v1:

- storage-engine-specific schema migrations
- partitioning, sharding, or fleet fanout design
- transport retry behavior
- automatic conflict-resolution policy
- physical purge API for tombstones beyond candidate discovery

## 14. References

- `reference-specs/protocols/cloud-vehicle-sync-protocol-v1.md`
- `proto/internal/cloud-vehicle-sync-envelope.proto`
- `reference-services/sync/common/include/cloud_vehicle_sync_types.hpp`
- `reference-services/sync/common/include/cloud_vehicle_db_adapter.hpp`
