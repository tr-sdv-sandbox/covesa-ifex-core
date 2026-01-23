# Scheduler Sync Protocol v2 Specification

## Status: DRAFT

**Version:** 2.5
**Date:** 2026-01-18
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

**Note:** Authority is mandatory - every job must have one set at creation.

**Rules:**
- Job created by cloud → `AUTHORITY_CLOUD`
- Job created by vehicle → `AUTHORITY_VEHICLE`
- Job created by phone (via vehicle) → `AUTHORITY_VEHICLE`

Authority is **immutable** after creation.

### 2.4 Sync State (Derived, for UI)

Each side can derive a sync state for display purposes:

```protobuf
enum SyncState {
    SYNC_PENDING = 0;       // My version differs from last confirmed remote version
    SYNC_SYNCED = 1;        // My version matches last confirmed remote version
}
```

**Note:** This is a local derived field, not transmitted. Computed by comparing local version with last confirmed remote version.

### 2.5 Job Record

```protobuf
message JobRecord {
    // Identity (included in checksum)
    string job_id = 1;
    JobAuthority authority = 2;

    // Version (included in checksum)
    JobVersion version = 3;

    // Content - synced state (included in checksum)
    bool deleted = 5;                    // Soft delete (tombstone)
    string title = 10;
    string service = 11;
    string method = 12;
    string parameters_json = 13;
    uint64 scheduled_time_ms = 14;       // Epoch milliseconds
    string recurrence_rule = 15;         // iCal RRULE
    uint64 end_time_ms = 16;
    bool paused = 17;                    // User intent: "don't schedule this job"
    WakePolicy wake_policy = 18;         // Whether to wake vehicle for this job
    SleepPolicy sleep_policy = 19;       // Sleep behavior during execution
    uint32 wake_lead_time_s = 20;        // Seconds before scheduled_time to wake

    // Execution state (vehicle-authoritative, NOT in checksum)
    JobStatus status = 25;               // What's happening now
    uint64 next_run_time_ms = 26;
    uint64 last_executed_ms = 27;

    // Metadata (NOT in checksum)
    uint64 created_at_ms = 30;
    uint64 updated_at_ms = 31;
    uint64 deleted_at_ms = 6;            // When deleted (for GC timing only)
    string created_by = 32;
}

enum JobStatus {
    JOB_STATUS_PENDING = 0;     // Waiting to execute
    JOB_STATUS_RUNNING = 1;     // Currently executing
    JOB_STATUS_COMPLETED = 2;   // Finished successfully
    JOB_STATUS_FAILED = 3;      // Execution failed
    JOB_STATUS_CANCELLED = 4;   // Cancelled by user/system
}

enum WakePolicy {
    WAKE_NO_WAKE = 0;           // Only run if vehicle already awake
    WAKE_REQUIRED = 1;          // Wake vehicle via RTC to run job
}

enum SleepPolicy {
    SLEEP_NORMAL = 0;           // Normal sleep after job
    SLEEP_INHIBIT = 1;          // Prevent sleep until job complete
}
```

**Notes:**
- `deleted` is synced content (in checksum); `deleted_at_ms` is metadata (not in checksum)
- `paused` is user intent (synced), `status` is runtime state (vehicle-authoritative)
- Tombstones (`deleted=true`) retain all content fields for conflict resolution

### 2.6 Execution Record

Executions are append-only facts, separate from job state:

```protobuf
message ExecutionRecord {
    string execution_id = 1;             // Globally unique
    string job_id = 2;
    uint64 executed_at_ms = 3;
    uint64 duration_ms = 4;
    JobStatus status = 5;                // COMPLETED or FAILED
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

3. **Increment resolver's sequence:**
   ```
   if resolver is cloud:  merged.cloud_seq++
   if resolver is vehicle: merged.vehicle_seq++
   ```
   This ensures the resolved version dominates both inputs.

4. **Result:**
   - Content from winner (authority)
   - Version is merged + incremented
   - Other side sees REMOTE_DOMINATES and accepts

### 4.2 Resolution Example

```
Job X created by cloud (AUTHORITY_CLOUD)
Last sync: {cloud: 5, vehicle: 3}

Cloud offline: modifies to {cloud: 7, vehicle: 3}, content = "10am"
Vehicle offline: modifies to {cloud: 5, vehicle: 5}, content = "2pm"

On reconnect (vehicle sends first):
  1. Vehicle sends V2C with {5, 5}, "2pm"
  2. Cloud receives, compares {7, 3} vs {5, 5}
     Neither dominates → CONFLICT
     Authority = CLOUD → cloud content wins
     Merged = {max(7,5), max(3,5)} = {7, 5}
     Cloud increments its seq → {8, 5}
  3. Cloud sends C2V with {8, 5}, "10am"
  4. Vehicle receives, compares {5, 5} vs {8, 5}
     {8, 5} dominates {5, 5} → accept remote
  5. Vehicle now has {8, 5}, "10am"

Both sides converge to: {8, 5} with "10am"
```

**Note:** The resolver always increments their sequence after merge. This ensures the
resolved version dominates both inputs, so the other side accepts it without conflict.

### 4.3 Tombstone Deletion Protocol

Deletion requires coordination to ensure both sides agree before physical removal.
The protocol ensures crash recovery without losing deletion intent or resurrecting
deleted jobs.

#### 4.3.1 Key Invariant

**The initiator must persist the deletion intent before sending.**

| Initiator | Must persist before sending | Confirmation |
|-----------|---------------------------|--------------|
| Cloud | `JobRecord{deleted=true}` in PostgreSQL | Vehicle echoes same version in V2C |
| Vehicle | `JobRecord{deleted=true}` to local storage | Cloud echoes same version in C2V |

**Confirmation is implicit:** When both sides have the same `{job_id, version, deleted=true}`,
the tombstone is confirmed. No separate acknowledgment message needed.

#### 4.3.2 Cloud-Initiated Delete

```
CLOUD                                           VEHICLE
  │                                                │
  │ 1. UPDATE job SET deleted=true,                │
  │    version.cloud_seq++                         │
  │    (PERSISTED)                                 │
  │                                                │
  │ ─── C2V: JobRecord{deleted=true, v={6,3}} ───► │
  │                                                │
  │                          2. Apply tombstone    │
  │                             (version dominates │
  │                              or merge)         │
  │                             (PERSISTED)        │
  │                                                │
  │ ◄── V2C: JobRecord{deleted=true, v={6,3}} ─── │
  │                                                │
  │ 3. Received confirmation                       │
  │    (versions match = confirmed)                │
  │    Can GC after retention period               │
  │                                                │
```

**Crash Recovery Scenarios:**

| Crash Point | Cloud State | Vehicle State | Recovery |
|-------------|-------------|---------------|----------|
| After step 1, before C2V sent | `deleted=true, {6,3}` | Has job `{5,3}` | Cloud re-sends tombstone |
| After C2V sent, before step 2 | `deleted=true, {6,3}` | Has job `{5,3}` | Cloud re-sends, vehicle applies |
| After step 2, before V2C sent | `deleted=true, {6,3}` | Has tombstone `{6,3}` | Vehicle re-sends tombstone |
| After V2C sent, before step 3 | `deleted=true, {6,3}` | Has tombstone `{6,3}` | Vehicle re-sends, cloud sees match |

#### 4.3.3 Vehicle-Initiated Delete

```
VEHICLE                                         CLOUD
  │                                                │
  │ 1. User/system deletes job                     │
  │    SET deleted=true, version.vehicle_seq++    │
  │    (PERSISTED)                                 │
  │                                                │
  │ ─── V2C: JobRecord{deleted=true, v={5,4}} ───► │
  │                                                │
  │                          2. Apply tombstone    │
  │                             (version dominates │
  │                              or merge)         │
  │                                                │
  │ ◄── C2V: JobRecord{deleted=true, v={5,4}} ─── │
  │                                                │
  │ 3. Received confirmation                       │
  │    (versions match = confirmed)                │
  │    Can GC after retention period               │
  │                                                │
```

**Crash Recovery Scenarios:**

| Crash Point | Vehicle State | Cloud State | Recovery |
|-------------|---------------|-------------|----------|
| After step 1, before V2C sent | Tombstone `{5,4}` | Has job `{5,3}` | Vehicle re-sends tombstone |
| After V2C sent, before step 2 | Tombstone `{5,4}` | Has job `{5,3}` | Vehicle re-sends, cloud applies |
| After step 2, before C2V sent | Tombstone `{5,4}` | Tombstone `{5,4}` | Cloud re-sends confirmation |
| After C2V sent, before step 3 | Tombstone `{5,4}` | Tombstone `{5,4}` | Vehicle receives, versions match |

#### 4.3.4 Handling Unknown Deletions

When receiving a tombstone for an unknown job:

**Cloud receives tombstone `JobRecord{deleted=true}` for unknown job:**
- Check if tombstone exists in retention storage
- If yes: include in C2V response (re-confirm)
- If no: create tombstone record, include in C2V response

**Vehicle receives `JobRecord{deleted=true}` for unknown job:**
- Create tombstone with version and authority from received record
- Include in next V2C sync
- This handles: vehicle restart after delete command sent but before persistence

#### 4.3.5 Tombstone Record Structure

Tombstones are regular `JobRecord` entries with `deleted=true`. This unified model
ensures version vectors are always available for conflict resolution.

**Vehicle side (local storage):**
```json
{
    "job_id": "cloud-abc123",
    "deleted": true,
    "version": {"cloud_seq": 6, "vehicle_seq": 3},
    "deleted_at_ms": 1705123456000,
    "authority": "AUTHORITY_CLOUD"
}
```

**Cloud side (PostgreSQL):**
```sql
jobs (
    job_id VARCHAR PRIMARY KEY,
    vehicle_id VARCHAR,
    deleted BOOLEAN DEFAULT false,
    deleted_at TIMESTAMP,
    cloud_seq BIGINT,
    vehicle_seq BIGINT,
    authority VARCHAR,           -- 'cloud' or 'vehicle'
    -- ... other job fields
)
```

**Confirmation detection:** Tombstone is confirmed when both sides have
matching `{job_id, cloud_seq, vehicle_seq, deleted=true}`.

#### 4.3.6 Garbage Collection

Tombstones must be confirmed before garbage collection. Time alone is not sufficient.

```
Tombstone GC Rules:
1. Tombstone confirmed by other side (required)
2. AND tombstone age > RETENTION_PERIOD (default: 7 days)
3. THEN safe to physically delete
```

**Confirmation is required** because:
- Vehicle may be offline for weeks (parked, stored, shipped)
- Time-only GC would cause job resurrection on reconnect
- Cloud storage is cheap; resurrection bugs are expensive

**Retention period after confirmation** because:
- Prevents issues from delayed/reordered messages
- Allows safe replay of old sync logs for debugging
- 7 days is minimum; production may use 30+ days

**Cloud-side implementation:**
```sql
-- Safe to GC when:
--   1. Vehicle confirmed (echoed same version)
--   2. AND retention period elapsed
DELETE FROM jobs
WHERE deleted = true
  AND vehicle_confirmed_at IS NOT NULL
  AND vehicle_confirmed_at < NOW() - INTERVAL '7 days';
```

**Vehicle-side implementation:**
- Keep tombstones until cloud confirms (echoes in C2V)
- After confirmation, apply local retention period
- On storage pressure, confirmed tombstones are first to evict

#### 4.3.7 Conflict: Delete vs Modify

If delete conflicts with modify (neither version dominates):

1. Check job's `authority` field
2. Authority determines winner:
   - If delete wins: job stays deleted
   - If modify wins: job is "resurrected" (delete tombstone removed)
3. Merged version computed as usual

**Example:**
```
Job X (AUTHORITY_CLOUD)
Cloud: deletes (cloud_seq: 7)
Vehicle: modifies (vehicle_seq: 5)

Conflict: {7, 3} vs {5, 5}
Authority = CLOUD → delete wins
Merged version: {7, 5} with deleted=true
```

## 5. Sync Protocol

### 5.1 Message Types

```protobuf
// Cloud → Vehicle
message C2V_SyncMessage {
    string vehicle_id = 1;
    repeated JobRecord jobs = 2;         // All jobs: active AND tombstones (deleted=true)
    uint64 sync_timestamp_ms = 3;

    // Checksum-based quiescence detection
    uint64 state_checksum = 10;          // Hash of current state
    uint64 last_seen_v2c_checksum = 11;  // "I've seen your state up to this checksum"
}

// Vehicle → Cloud
message V2C_SyncMessage {
    string vehicle_id = 1;
    string bridge_instance_id = 2;
    repeated JobRecord jobs = 3;         // All jobs: active AND tombstones (deleted=true)
    repeated ExecutionRecord executions = 4;  // New executions since last sync
    uint64 sync_timestamp_ms = 5;

    // Checksum-based quiescence detection
    uint64 state_checksum = 10;          // Hash of current state
    uint64 last_seen_c2v_checksum = 11;  // "I've seen your state up to this checksum"
}

```

**Notes:**
- Tombstones are `JobRecord{deleted=true}` with version vectors
- V2C serves as implicit acknowledgment (no separate SyncAck needed)

### 5.2 Sync Flow (Event-Driven)

Messages only sent when checksum differs. Silence = agreement.

```
CLOUD                                    VEHICLE
  │                                         │
  │◄──── V2C{jobs, checksum=0xAAAA} ────────│  (vehicle changed)
  │                                         │
  │  [compare, resolve, update]             │
  │                                         │
  │──── C2V{jobs, checksum=0xBBBB,  ───────►│  (cloud responds)
  │         last_seen_v2c=0xAAAA}           │
  │                                         │
  │                     [apply updates]     │
  │                                         │
  │◄─── V2C{checksum=0xBBBB,        ────────│  (vehicle confirms)
  │         last_seen_c2v=0xBBBB}           │
  │                                         │
  │  [checksums match - QUIESCENT]          │
  │                                         │
  ═══════════ NO TRAFFIC UNTIL STATE CHANGES ═══════════
```

**Sync trigger:** Checksum differs from last confirmed.

### 5.3 Per-Job Sync Logic

On receiving a job from remote:

| Condition | Action |
|-----------|--------|
| Unknown job_id | Accept remote |
| Same version | No-op |
| Remote dominates | Accept remote |
| Local dominates | Keep local |
| Neither dominates (conflict) | Resolve by authority, merge versions |

**Conflict resolution:** Winner is the side matching `authority`. Merged version = `{max(cloud_seq), max(vehicle_seq)}`.

### 5.4 Idempotency

All sync operations are idempotent:

- Receiving same version twice → no-op
- Receiving older version → ignored (local dominates)
- Receiving same conflict resolution twice → same result

This allows safe retry on network failure.

### 5.5 State Checksum

**Hash function:** xxHash64

**Checksum includes (per job, sorted by job_id):**
- job_id, authority, version (cloud_seq, vehicle_seq), deleted
- Content fields: title, service, method, parameters_json, scheduled_time_ms, recurrence_rule, end_time_ms, paused, wake_policy, sleep_policy, wake_lead_time_s

**Checksum excludes:**
- Metadata: created_at_ms, updated_at_ms, deleted_at_ms, created_by
- Execution state: status, next_run_time_ms, last_executed_ms
- Execution history

**Rationale:** Version vectors track modification count, not content. Content hashing detects data corruption.

### 5.6 Quiescence Detection

**Quiescent when:** Both sides have confirmed each other's current checksum.

**Send sync when:** Remote hasn't confirmed my current checksum (i.e., remote's `last_seen_*_checksum` ≠ my `state_checksum`).

**No messages sent when quiescent.** MQTT keepalive maintains connection.

### 5.7 Bootstrap and Initiation

#### Initial State

On first connection (no prior sync history):
- `last_seen_remote_checksum = 0` (unknown)
- `state_checksum` = hash of current jobs (may be empty or have local jobs)

#### Bootstrap Flow

```
VEHICLE (new)                              CLOUD
  │                                          │
  │  [no prior sync, last_seen_c2v = 0]      │
  │                                          │
  │── V2C{jobs=[], checksum=0x0000,    ─────►│  (empty vehicle)
  │       last_seen_c2v=0}                   │
  │                                          │
  │                    [cloud has jobs]      │
  │                                          │
  │◄── C2V{jobs=[...], checksum=0xABCD, ────│  (cloud sends all)
  │        last_seen_v2c=0x0000}             │
  │                                          │
  │  [vehicle applies jobs]                  │
  │                                          │
  │── V2C{jobs=[...], checksum=0xABCD, ────►│  (vehicle confirms)
  │       last_seen_c2v=0xABCD}              │
  │                                          │
  │  [checksums match - QUIESCENT]           │
```

#### Sync Trigger

Either side sends when: `remote.last_seen_my_checksum ≠ my_checksum`

This means:
- On first connect: both sides send (neither has confirmed the other)
- After state change: changed side sends
- On reconnect with same state: no messages (already quiescent)

### 5.8 Execution Records

- Append-only facts, no versioning
- Cloud deduplicates by `execution_id`
- No acknowledgment needed
- Retained independently of job (survives job GC)

## 6. Per-Job Sync State (Derived)

```
SYNC_PENDING ◄──(local changes)──► SYNC_SYNCED
     │                                  ▲
     │                                  │
     └──(remote confirms my version)────┘
```

Derived by comparing local version with last confirmed remote version.

Executions: Append-only, no state. Vehicle creates → V2C → Cloud stores.

## 7. Edge Cases

| Scenario | Behavior |
|----------|----------|
| **Bootstrap (new vehicle)** | Vehicle sends empty state, cloud sends all jobs, converge |
| **Delete during execution** | Execution recorded, delete applied. Both facts preserved |
| **Long offline (30+ days)** | Works correctly - tombstones kept until confirmed (see 4.3.6) |
| **Vehicle clock wrong** | No impact - wall clock is metadata only, not used for ordering |
| **Network failure mid-sync** | Idempotent - retry is no-op |
| **Sync drift (job missing)** | Checksum mismatch triggers re-sync, re-send missing jobs |

## 8. Implementation Requirements

### 8.1 Both Sides

- Store jobs with version vectors (`cloud_seq`, `vehicle_seq`)
- Persist tombstones (`deleted=true`) as regular JobRecords
- Compute state checksum on any change
- Track checksums for quiescence detection

### 8.2 Cloud-Specific

- Database columns: `cloud_seq`, `vehicle_seq`, `authority`, `deleted`, `deleted_at`
- Keep tombstones for 7 days (retention period)

### 8.3 Vehicle-Specific

- Sync bridge must persist version vectors (survives restart)
- Handle `deleted=true` for unknown jobs (create tombstone)

### 8.4 Garbage Collection

Remove tombstones where:
1. `deleted=true`
2. AND remote has confirmed (echoed same `{job_id, version, deleted=true}`)
3. AND `confirmation_age > RETENTION_PERIOD` (7 days minimum)

**Critical:** Never GC based on time alone. Confirmation is required to prevent
resurrection of deleted jobs when vehicles reconnect after extended offline periods.

## 9. Future Considerations

### 9.1 Not In Scope (v2)

- Multi-writer within a side (would need full vector clocks)
- Partial sync / delta compression
- Conflict notification to users
- Manual conflict resolution UI

### 9.2 Potential Extensions

- **Conflict callbacks:** Notify application layer of conflicts
- **Merge strategies:** Per-field merge for non-conflicting changes
- **Compression:** Delta encoding for large job sets
- **Batching:** Combine multiple changes into single sync

---

## Appendix A: Test Scenarios

### A.1 Sync

| Cloud | Vehicle | Expected |
|-------|---------|----------|
| {1,0} | none | Vehicle accepts {1,0} |
| none | {0,1} | Cloud accepts {0,1} |
| {2,0} | {1,0} | Vehicle updates to {2,0} (cloud dominates) |
| {2,0} (cloud auth) | {1,1} | Conflict: cloud resolves → {3,1}, vehicle accepts |
| {2,0} (vehicle auth) | {1,1} | Conflict: cloud resolves → {3,1} with vehicle content* |

*When cloud resolves a conflict where vehicle is authoritative, cloud still increments
`cloud_seq` but uses vehicle's content. Vehicle then accepts the dominating version.

### A.2 Tombstones

| Action | Expected |
|--------|----------|
| Cloud deletes {5,3} → {6,3} | Vehicle echoes {6,3} tombstone |
| Vehicle deletes {5,3} → {5,4} | Cloud echoes {5,4} tombstone |
| Delete for unknown job | Receiver creates tombstone, echoes |

### A.3 Quiescence

| Scenario | Expected |
|----------|----------|
| Both checksums match, confirmed | No messages |
| One side changes | Exchange until checksums match |
| Reconnect, same state | No messages (already quiescent) |
