# Scheduler Sync Protocol v2 Specification

## Status: DRAFT

**Version:** 2.0
**Date:** 2026-01-11
**Authors:** Claude + Human

## 1. Overview

### 1.1 Purpose

This specification defines a bidirectional synchronization protocol for scheduled jobs between cloud (offboard) and vehicle (onboard) systems. The protocol supports:

- Job creation from either side (cloud, vehicle, or phone via vehicle)
- Offline operation on both sides
- Automatic conflict resolution without manual intervention
- Execution history as append-only facts

### 1.2 Design Goals

| Goal | Approach |
|------|----------|
| Offline support | Both sides can operate independently for extended periods |
| Automatic resolution | No manual conflict resolution required |
| Clock independence | Uses logical clocks, not wall-clock time |
| Deterministic | Same inputs always produce same outputs |
| Convergent | Both sides eventually reach identical state |
| Simple | Two-participant version vector (not full vector clocks) |

### 1.3 Non-Goals

- Real-time synchronization (eventual consistency is acceptable)
- Multi-master within a single side (single writer per side assumed)
- Backward compatibility (clean-slate protocol)

## 2. Data Model

### 2.1 Job Identity

Jobs are identified by globally unique IDs with source namespace:

```
job_id ::= <source>-<uuid>

source ::= "cloud" | "veh-<vin>" | "phone"
uuid   ::= UUID v4 (lowercase, no hyphens)
```

Examples:
- `cloud-a1b2c3d4e5f6789012345678`
- `veh-WDB1234567F123456-b2c3d4e5f6789012345678`
- `phone-c3d4e5f6789012345678abcd`

**Rationale:** Namespaced IDs prevent collision when both sides create jobs offline.

### 2.2 Version Vector

Each job has a two-component version vector:

```protobuf
message JobVersion {
    uint64 cloud_seq = 1;    // Incremented by cloud on any change
    uint64 vehicle_seq = 2;  // Incremented by vehicle on any change
}
```

**Initial version:** `{cloud_seq: 0, vehicle_seq: 0}`

**On creation:**
- Cloud creates: `{cloud_seq: 1, vehicle_seq: 0}`
- Vehicle creates: `{cloud_seq: 0, vehicle_seq: 1}`

**On modification:**
- Cloud modifies: `cloud_seq++`
- Vehicle modifies: `vehicle_seq++`

### 2.3 Source Authority

Each job has an authoritative source determined at creation:

```protobuf
enum JobAuthority {
    AUTHORITY_CLOUD = 0;    // Cloud is authoritative for conflicts
    AUTHORITY_VEHICLE = 1;  // Vehicle is authoritative for conflicts
}
```

**Rules:**
- Job created by cloud → `AUTHORITY_CLOUD`
- Job created by vehicle → `AUTHORITY_VEHICLE`
- Job created by phone (via vehicle) → `AUTHORITY_VEHICLE`

Authority is **immutable** after creation.

### 2.4 Sync State

Each job has a sync state tracking cloud-vehicle agreement:

```protobuf
enum SyncState {
    SYNC_UNKNOWN = 0;
    SYNC_PENDING = 1;       // Change made, not yet confirmed by other side
    SYNC_SYNCED = 2;        // Both sides agree (steady state)
    SYNC_CONFLICT = 3;      // Conflict detected, resolution in progress
}
```

### 2.5 Job Record

Complete job record structure:

```protobuf
message JobRecord {
    // Identity
    string job_id = 1;
    JobAuthority authority = 2;

    // Version
    JobVersion version = 3;
    SyncState sync_state = 4;

    // Lifecycle
    bool deleted = 5;                    // Soft delete (tombstone)
    uint64 deleted_at_ms = 6;            // When deleted (for tombstone expiry)

    // Content
    string title = 10;
    string service = 11;
    string method = 12;
    string parameters_json = 13;
    uint64 scheduled_time_ms = 14;       // Epoch milliseconds
    string recurrence_rule = 15;         // iCal RRULE
    uint64 end_time_ms = 16;             // Optional: stop recurring after this

    // Execution state (vehicle-authoritative)
    JobStatus status = 20;               // PENDING, RUNNING, COMPLETED, etc.
    uint64 next_run_time_ms = 21;
    uint64 last_executed_ms = 22;

    // Metadata
    uint64 created_at_ms = 30;
    uint64 updated_at_ms = 31;
    string created_by = 32;              // User/system that created
}
```

### 2.6 Execution Record

Executions are append-only facts, separate from job state:

```protobuf
message ExecutionRecord {
    string execution_id = 1;             // Globally unique
    string job_id = 2;
    uint64 executed_at_ms = 3;
    uint64 duration_ms = 4;
    ExecutionStatus status = 5;          // SUCCESS, FAILED, TIMEOUT
    string result_json = 6;
    string error_message = 7;
}
```

**Key property:** Executions have NO version. They are immutable facts.

## 3. Version Vector Semantics

### 3.1 Dominance

Version A **dominates** version B if and only if:

```
A.cloud_seq >= B.cloud_seq AND
A.vehicle_seq >= B.vehicle_seq AND
(A.cloud_seq > B.cloud_seq OR A.vehicle_seq > B.vehicle_seq)
```

In other words: A is at least as advanced in both components, and strictly more advanced in at least one.

### 3.2 Comparison Outcomes

Given versions A and B:

| Condition | Outcome |
|-----------|---------|
| A dominates B | A is newer, B should accept A |
| B dominates A | B is newer, A should accept B |
| Neither dominates | **Conflict** - concurrent modifications |
| A equals B | Same version, no action needed |

### 3.3 Examples

```
A = {cloud: 5, vehicle: 3}
B = {cloud: 3, vehicle: 3}
→ A dominates B (5>3, 3=3)

A = {cloud: 5, vehicle: 3}
B = {cloud: 5, vehicle: 5}
→ B dominates A (5=5, 5>3)

A = {cloud: 6, vehicle: 3}
B = {cloud: 5, vehicle: 5}
→ CONFLICT (6>5 but 3<5)

A = {cloud: 5, vehicle: 3}
B = {cloud: 5, vehicle: 3}
→ Equal (no action)
```

## 4. Conflict Resolution

### 4.1 Resolution Rules

When neither version dominates (true conflict):

1. **Check authority:** Job's `authority` field determines winner
   - `AUTHORITY_CLOUD` → cloud's version wins
   - `AUTHORITY_VEHICLE` → vehicle's version wins

2. **Compute merged version:**
   ```
   merged.cloud_seq = max(A.cloud_seq, B.cloud_seq)
   merged.vehicle_seq = max(A.vehicle_seq, B.vehicle_seq)
   ```

3. **Result:**
   - Content from winner
   - Version is merged version
   - Both sides converge to same state

### 4.2 Resolution Example

```
Job X created by cloud (AUTHORITY_CLOUD)
Last sync: {cloud: 5, vehicle: 3}

Cloud offline: modifies to {cloud: 7, vehicle: 3}, content = "10am"
Vehicle offline: modifies to {cloud: 5, vehicle: 5}, content = "2pm"

On reconnect:
  Neither dominates → CONFLICT
  Authority = CLOUD → cloud wins

  Merged version: {cloud: 7, vehicle: 5}
  Content: "10am" (from cloud)

Both sides now have: {7, 5} with "10am"
```

### 4.3 Delete Handling

Delete is just another state change:

- Delete sets `deleted = true` and increments version
- Compared using same dominance rules
- If delete conflicts with modify:
  - Authority determines winner
  - If delete wins: job stays deleted
  - If modify wins: job is "resurrected"

**Tombstone expiry:** Deleted jobs kept for 30 days, then purged.

## 5. Sync Protocol

### 5.1 Message Types

```protobuf
// Cloud → Vehicle
message C2V_SyncMessage {
    string vehicle_id = 1;
    repeated JobRecord jobs = 2;         // Full job states
    repeated string deleted_job_ids = 3; // Tombstones (job_id only)
    uint64 sync_timestamp_ms = 4;
}

// Vehicle → Cloud
message V2C_SyncMessage {
    string vehicle_id = 1;
    string bridge_instance_id = 2;
    repeated JobRecord jobs = 3;
    repeated string deleted_job_ids = 4;
    repeated ExecutionRecord executions = 5;  // New executions since last sync
    uint64 sync_timestamp_ms = 6;
}

// Acknowledgment (bidirectional)
message SyncAck {
    bool success = 1;
    repeated ConflictResolution resolutions = 2;  // How conflicts were resolved
    uint64 ack_timestamp_ms = 3;
}

message ConflictResolution {
    string job_id = 1;
    JobVersion winning_version = 2;
    string winner = 3;  // "cloud" or "vehicle"
}
```

### 5.2 Sync Flow

```
CLOUD                                    VEHICLE
  │                                         │
  │ ◄──────── V2C_SyncMessage ───────────── │  (vehicle sends its state)
  │                                         │
  │  [compare versions]                     │
  │  [detect conflicts]                     │
  │  [resolve by authority]                 │
  │  [merge versions]                       │
  │                                         │
  │ ─────────── SyncAck ──────────────────► │  (cloud confirms)
  │                                         │
  │ ─────────── C2V_SyncMessage ──────────► │  (cloud sends its state)
  │                                         │
  │                     [compare versions]  │
  │                     [apply updates]     │
  │                                         │
  │ ◄──────────── SyncAck ───────────────── │  (vehicle confirms)
  │                                         │
```

### 5.3 Per-Job Sync Logic

On receiving a job record from remote:

```python
def handle_remote_job(remote_job, local_jobs):
    job_id = remote_job.job_id

    if job_id not in local_jobs:
        # New job from remote - accept it
        local_jobs[job_id] = remote_job
        return "accepted"

    local_job = local_jobs[job_id]

    if remote_job.version == local_job.version:
        # Same version - no action
        return "unchanged"

    if dominates(remote_job.version, local_job.version):
        # Remote is newer - accept it
        local_jobs[job_id] = remote_job
        return "updated"

    if dominates(local_job.version, remote_job.version):
        # Local is newer - keep local (remote will get update on next sync)
        return "local_newer"

    # Neither dominates - CONFLICT
    winner = resolve_conflict(local_job, remote_job)
    merged_version = merge_versions(local_job.version, remote_job.version)

    result = winner.copy()
    result.version = merged_version
    result.sync_state = SYNC_SYNCED

    local_jobs[job_id] = result
    return "conflict_resolved"

def resolve_conflict(local, remote):
    if local.authority == AUTHORITY_CLOUD:
        return remote if is_cloud else local
    else:
        return local if is_vehicle else remote

def merge_versions(a, b):
    return JobVersion(
        cloud_seq = max(a.cloud_seq, b.cloud_seq),
        vehicle_seq = max(a.vehicle_seq, b.vehicle_seq)
    )
```

### 5.4 Idempotency

All sync operations are idempotent:

- Receiving same version twice → no-op
- Receiving older version → ignored (local dominates)
- Receiving same conflict resolution twice → same result

This allows safe retry on network failure.

## 6. State Machine

### 6.1 Job Lifecycle States

```
                           ┌─────────────────────────────────┐
                           │                                 │
    ┌──────────────────────▼────────────────────────┐        │
    │               SYNC_PENDING                    │        │
    │  Local change made, not confirmed by remote   │        │
    └──────────────────────┬────────────────────────┘        │
                           │                                 │
              remote confirms (version matches)              │
                           │                                 │
    ┌──────────────────────▼────────────────────────┐        │
    │                SYNC_SYNCED                    │        │
    │  Both sides have same version (steady state)  │◄───────┤
    └──────────────────────┬────────────────────────┘        │
                           │                                 │
              local OR remote changes                        │
                           │                                 │
    ┌──────────────────────▼────────────────────────┐        │
    │                  compare                      │        │
    └───────┬──────────────┬────────────────┬───────┘        │
            │              │                │                │
    one dominates    neither dominates   equal              │
            │              │                │                │
            │    ┌─────────▼─────────┐      │                │
            │    │   SYNC_CONFLICT   │      │                │
            │    │  resolve by       │      │                │
            │    │  authority        │      │                │
            │    └─────────┬─────────┘      │                │
            │              │                │                │
            └──────────────┴────────────────┴────────────────┘
                           │
                      merge versions
                           │
                           ▼
                      SYNC_SYNCED
```

### 6.2 Execution Handling

Executions follow a separate path (no state machine, append-only):

```
Vehicle executes job
        │
        ▼
ExecutionRecord created
        │
        ▼
Added to V2C_SyncMessage.executions
        │
        ▼
Cloud receives and stores
        │
        ▼
Done (no confirmation needed - facts are immutable)
```

## 7. Edge Cases

### 7.1 Job Deleted While Executing

```
Timeline:
  t=100: Cloud deletes job (cloud_seq: 7)
  t=150: Vehicle executes job (didn't know about delete)
  t=200: Reconnect
```

**Behavior:**
- Execution record is stored (it happened)
- Delete is applied (job is deleted)
- Final state: deleted job with execution history

### 7.2 Same Job Modified Many Times Offline

```
Cloud:   v{5,3} → {6,3} → {7,3} → {8,3}
Vehicle: v{5,3} → {5,4} → {5,5} → {5,6}
```

**On reconnect:**
- Cloud: {8, 3}
- Vehicle: {5, 6}
- Neither dominates → CONFLICT
- Resolution by authority
- Merged version: {8, 6}

**Key point:** Only final states compared, not intermediate history.

### 7.3 Reconnect After Very Long Offline

```
Vehicle offline for 30 days
Cloud: {cloud: 50, vehicle: 3}
Vehicle: {cloud: 5, vehicle: 40}
```

**Behavior:** Same as any conflict:
- Neither dominates
- Authority determines winner
- Versions merged

No special handling for "large" divergence.

### 7.4 Clock Considerations

**Wall clock is NOT used for conflict resolution.**

Wall clock (`updated_at_ms`) is metadata only:
- Used for display ("last modified 5 minutes ago")
- Used for tombstone expiry
- NOT used to determine version ordering

**Vehicle clock can be wrong** and protocol still works correctly.

### 7.5 Sync Bridge Restart

If vehicle sync bridge restarts:

1. Bridge reads current state from vehicle scheduler
2. Bridge has persisted `{cloud_seq, vehicle_seq}` per job
3. Resume normal sync

**Requirement:** Sync bridge must persist version vectors.

### 7.6 Network Failure During Sync

```
1. Cloud sends C2V_SyncMessage
2. Vehicle receives, updates local state
3. Vehicle sends SyncAck
4. Ack lost!
5. Cloud retries C2V_SyncMessage
6. Vehicle receives again
```

**Behavior:** Idempotent - same version received twice is no-op.

## 8. Implementation Requirements

### 8.1 Cloud Side (Offboard)

| Component | Requirement |
|-----------|-------------|
| scheduler_api | Single writer (coordinate via database) |
| scheduler_mirror | Store JobRecord with version vectors |
| Database | Add `cloud_seq`, `vehicle_seq`, `authority`, `sync_state` columns |
| MQTT handler | Parse V2C_SyncMessage, send C2V_SyncMessage |

### 8.2 Vehicle Side (Onboard)

| Component | Requirement |
|-----------|-------------|
| Scheduler service | Store jobs with version vectors |
| Sync bridge | Persist version vectors (survives restart) |
| Sync bridge | Compare versions, apply sync logic |
| Backend transport | Send/receive sync messages via MQTT |

### 8.3 Content ID

Scheduler sync uses **content_id = 202** for both directions.

### 8.4 Sync Frequency

- Vehicle → Cloud: On change + periodic (configurable, default 60s)
- Cloud → Vehicle: On change + periodic (configurable, default 60s)
- Full sync on reconnect after offline period

## 9. Migration

Since we have no backward compatibility requirement:

1. Clear all existing job sync state
2. Deploy new protocol to both sides
3. Full sync establishes baseline

## 10. Future Considerations

### 10.1 Not In Scope (v2)

- Multi-writer within a side (would need full vector clocks)
- Partial sync / delta compression
- Conflict notification to users
- Manual conflict resolution UI

### 10.2 Potential Extensions

- **Conflict callbacks:** Notify application layer of conflicts
- **Merge strategies:** Per-field merge for non-conflicting changes
- **Compression:** Delta encoding for large job sets
- **Batching:** Combine multiple changes into single sync

---

## Appendix A: Proto Definitions

See `proto/scheduler-sync-v2.proto` (to be created)

## Appendix B: Test Scenarios

| Scenario | Cloud | Vehicle | Expected |
|----------|-------|---------|----------|
| Cloud creates | {1,0} | none | Vehicle gets {1,0} |
| Vehicle creates | none | {0,1} | Cloud gets {0,1} |
| Cloud updates | {2,0} | {1,0} | Vehicle updates to {2,0} |
| Vehicle updates | {1,0} | {1,1} | Cloud updates to {1,1} |
| Both update (cloud auth) | {2,0} | {1,1} | Both get {2,1} with cloud content |
| Both update (vehicle auth) | {2,0} | {1,1} | Both get {2,1} with vehicle content |
| Cloud deletes | {2,0} deleted | {1,1} | Conflict, resolve by authority |
| Execute during delete | {2,0} deleted | executed | Delete applies, execution recorded |
