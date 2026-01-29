# Scheduler Sync Protocol v3 Specification

## Status: DRAFT

**Version:** 3.2
**Date:** 2026-01-28
**Authors:** Claude + Human

## 1. Overview

### 1.1 Purpose

This specification defines a bandwidth-efficient bidirectional synchronization protocol for scheduled jobs between cloud (offboard) and vehicle (onboard) systems.

**Key improvements over v3.1:**
- Dirty-first sync: Exchange only dirty jobs first (fast path)
- Gap detection: Exchange job_id lists only when needed (recovery path)
- Eliminated hash manifest: No longer needed for normal sync
- Symmetric protocol: Both sides use identical logic

### 1.2 Design Goals

| Goal | Approach |
|------|----------|
| Bandwidth efficiency | Send only dirty jobs, gap detection on demand |
| Offline support | Both sides operate independently |
| Automatic resolution | Authority-based conflict resolution |
| Clock independence | Logical clocks (version vectors) |
| Low latency executions | Independent stream, immediate send |
| Simple reconnect | Checksum comparison, dirty exchange |
| Data loss recovery | Job ID list exchange for gap detection |
| Type safety | Envelope messages with explicit type discrimination |

### 1.3 Bandwidth Comparison

| Scenario | v2 (full sync) | v3.1 (hash-first) | v3.2 (dirty-first) |
|----------|----------------|-------------------|-------------------|
| 100 jobs, 5 changed | ~30 KB | ~6 KB | ~1.5 KB |
| 10,000 jobs, 50 changed | ~3 MB | ~500 KB | ~15 KB |
| Reconnect, no changes | ~30 KB | ~100 bytes | ~100 bytes |
| Data loss recovery | ~3 MB | ~500 KB | ~500 KB |

### 1.4 Protocol Philosophy

**Two-phase sync:**
1. **Phase 1 (Fast Path):** Exchange dirty jobs only. Handles 99% of cases.
2. **Phase 2 (Recovery Path):** Exchange job ID lists for gap detection. Only used when dirty exchange doesn't converge (data loss scenario).

Both phases can happen simultaneously from both sides - the protocol is symmetric.

## 2. Message Types

### 2.1 Overview

All messages use **content_id = 202**. Messages are wrapped in envelope types (`V2C_Envelope` or `C2V_Envelope`) with explicit type discrimination via protobuf `oneof`.

**Shared messages (same payload, direction determined by envelope):**

| Message | Purpose | When sent |
|---------|---------|-----------|
| `SyncMessage` | Dirty jobs + acks + checksum | Normal sync (99% of traffic) |
| `GapDetect` | Job ID lists + requests | Data loss recovery only |

**Direction-specific messages:**

| Message | Direction | Purpose |
|---------|-----------|---------|
| `Executions` | V2C only | Report execution results |
| `ExecutionAck` | C2V only | Acknowledge received executions |
| `TriggerJob` | C2V only | Request immediate job execution |
| `TriggerResponse` | V2C only | Response to trigger request |

**Removed from v3.1:**
- `V2C_HashManifest` - No longer needed
- `C2V_RequestHashes` - No longer needed
- `V2C_Hello`, `V2C_JobData`, `C2V_SyncDelta` - Consolidated into `SyncMessage`
- `V2C_JobAck`, `C2V_JobAck` - Integrated into `SyncMessage.acked_jobs`

### 2.2 Protocol Buffers

```protobuf
syntax = "proto3";
package swdv.scheduler_sync_v3;

option cc_enable_arenas = true;

// ============================================================================
// Envelope Messages (direction discrimination)
// ============================================================================

// Vehicle → Cloud envelope
message V2C_Envelope {
    oneof message {
        SyncMessage sync = 1;            // Normal sync (jobs + acks + checksum)
        GapDetect gap_detect = 2;        // Gap detection (recovery only)
        Executions executions = 3;       // Execution reports
        TriggerResponse trigger_response = 4;
    }
}

// Cloud → Vehicle envelope
message C2V_Envelope {
    oneof message {
        SyncMessage sync = 1;            // Normal sync (jobs + acks + checksum)
        GapDetect gap_detect = 2;        // Gap detection (recovery only)
        ExecutionAck execution_ack = 3;  // Acknowledge executions
        TriggerJob trigger_job = 4;      // Request immediate execution
    }
}

// ============================================================================
// Shared Sync Messages (used in both directions)
// ============================================================================

// Primary sync message - handles 99% of sync traffic
// Combines: job sending, job acknowledgment, state announcement
message SyncMessage {
    string vehicle_id = 1;
    repeated JobRecord jobs = 2;           // Dirty jobs I'm sending to you
    repeated JobVersionAck acked_jobs = 3; // Jobs I received from you (ACK)
    uint64 state_checksum = 4;             // My current state (xxHash64)
}

// Gap detection - used only for data loss recovery
// Triggered when checksums differ but no dirty jobs exist
message GapDetect {
    string vehicle_id = 1;
    repeated string job_ids = 2;           // All my job IDs
    repeated string request_job_ids = 3;   // Jobs I need from you
}

// ============================================================================
// Direction-Specific Messages
// ============================================================================

// V2C only: Execution results (independent stream)
message Executions {
    string vehicle_id = 1;
    repeated ExecutionRecord executions = 2;
}

// C2V only: Acknowledge received executions
message ExecutionAck {
    string vehicle_id = 1;
    repeated string execution_ids = 2;     // Executions stored
}

// C2V only: Request immediate job execution (imperative command)
message TriggerJob {
    string vehicle_id = 1;
    string job_id = 2;
    string request_id = 3;                 // For correlation with response
    string requester_id = 4;               // Who requested (for audit)
    uint64 timestamp_ms = 5;
    uint64 expires_at_ms = 6;              // 0 = no expiry
}

// V2C only: Response to trigger request
message TriggerResponse {
    string vehicle_id = 1;
    string job_id = 2;
    string request_id = 3;                 // Correlation ID from TriggerJob
    bool accepted = 4;
    string error_message = 5;              // If !accepted
    uint64 timestamp_ms = 6;
}

// ============================================================================
// Shared Types
// ============================================================================

// Version acknowledgment for a single job
message JobVersionAck {
    string job_id = 1;
    uint64 cloud_seq = 2;
    uint64 vehicle_seq = 3;
}

message JobVersion {
    uint64 cloud_seq = 1;
    uint64 vehicle_seq = 2;
}

message JobRecord {
    // Identity
    string job_id = 1;
    JobAuthority authority = 2;
    JobVersion version = 3;

    // Content (included in hash)
    bool deleted = 5;
    string title = 10;
    string service = 11;
    string method = 12;
    string parameters_json = 13;
    uint64 scheduled_time_ms = 14;
    string recurrence_rule = 15;
    uint64 end_time_ms = 16;
    bool paused = 17;
    WakePolicy wake_policy = 18;
    SleepPolicy sleep_policy = 19;
    uint32 wake_lead_time_s = 20;

    // Execution state (vehicle-authoritative, NOT in hash)
    JobStatus status = 25;
    uint64 next_run_time_ms = 26;
    uint64 last_executed_ms = 27;

    // Metadata (NOT in hash)
    uint64 created_at_ms = 30;
    uint64 updated_at_ms = 31;
    uint64 deleted_at_ms = 6;
    string created_by = 32;
}

message ExecutionRecord {
    string execution_id = 1;               // Globally unique (for dedup)
    string job_id = 2;
    uint64 executed_at_ms = 3;
    uint64 duration_ms = 4;
    JobStatus status = 5;                  // COMPLETED or FAILED
    string result_json = 6;
    string error_message = 7;
}

enum JobAuthority {
    AUTHORITY_CLOUD = 0;
    AUTHORITY_VEHICLE = 1;
}

enum JobStatus {
    JOB_STATUS_PENDING = 0;
    JOB_STATUS_RUNNING = 1;
    JOB_STATUS_COMPLETED = 2;
    JOB_STATUS_FAILED = 3;
    JOB_STATUS_CANCELLED = 4;
}

enum WakePolicy {
    WAKE_NO_WAKE = 0;
    WAKE_REQUIRED = 1;
}

enum SleepPolicy {
    SLEEP_NORMAL = 0;
    SLEEP_INHIBIT = 1;
}
```

## 3. Sync Flows

### 3.1 Fast Path - Dirty Exchange Only

Most common case: Both sides have some dirty jobs, exchange converges in one round.

```
VEHICLE                                           CLOUD
   │                                                │
   │ dirty: [job-A]                                 │ dirty: [job-X]
   │ state=0xAAAA                                   │ state=0xBBBB
   │                                                │
   │─── V2C SyncMessage ───────────────────────────►│
   │    jobs: []                                    │
   │    acked_jobs: []                              │
   │    state_checksum: 0xAAAA                      │
   │                                                │
   │                      Mismatch, has dirty jobs  │
   │                      Send dirty (fast path)    │
   │                                                │
   │◄─── C2V SyncMessage ──────────────────────────│
   │     jobs: [job-X@{1,0}]    ← dirty             │
   │     acked_jobs: []                             │
   │     state_checksum: 0xBBBB                     │
   │                                                │
   │ Apply job-X                                    │
   │ Send dirty job-A + ACK job-X                   │
   │                                                │
   │─── V2C SyncMessage ───────────────────────────►│
   │    jobs: [job-A@{0,1}]     ← dirty             │
   │    acked_jobs: [job-X@{1,0}]  ← ACK            │
   │    state_checksum: 0xCCCC                      │
   │                                                │
   │                      Update remote_version     │
   │                      for job-X (from ACK)      │
   │                      Apply job-A               │
   │                                                │
   │◄─── C2V SyncMessage ──────────────────────────│
   │     jobs: []                                   │
   │     acked_jobs: [job-A@{0,1}]  ← ACK           │
   │     state_checksum: 0xCCCC                     │
   │                                                │
   │ Update remote_version                          │
   │ for job-A (from ACK)                           │
   │ Checksums match!                               │
   │                                                │
   ═══════════════════ QUIESCENT ═══════════════════
```

**Key insight:** `SyncMessage.acked_jobs` allows each side to update `remote_version` when the other side confirms receipt. Quiescence is achieved when all `local_version == remote_version` (no dirty jobs).

### 3.2 Quiescent - No Traffic

Once quiescent, **no sync traffic flows**. Both sides are silent until:
- A local change occurs (job created/modified/deleted)
- Reconnection after disconnect

```
VEHICLE                                           CLOUD
   │                                                │
   │ dirty: []                                      │ dirty: []
   │ state=0xAAAA                                   │ state=0xAAAA
   │                                                │
   │              (silence - no traffic)            │
   │                                                │
   ═══════════════════ QUIESCENT ═══════════════════
```

**Bandwidth:** 0 bytes (steady state)

### 3.3 Reconnection - State Verification

On reconnect, vehicle sends initial SyncMessage to verify state agreement.

```
VEHICLE                                           CLOUD
   │                                                │
   │ (reconnect)                                    │
   │                                                │
   │─── V2C SyncMessage ───────────────────────────►│
   │    jobs: []                                    │
   │    acked_jobs: []                              │
   │    state_checksum: 0xAAAA                      │
   │                                                │
   │                      Checksums match!          │
   │                                                │
   │◄─── C2V SyncMessage ──────────────────────────│
   │     jobs: []                                   │
   │     acked_jobs: []                             │
   │     state_checksum: 0xAAAA                     │
   │                                                │
   ═══════════════════ QUIESCENT ═══════════════════
```

**Bandwidth:** ~100 bytes (one-time on reconnect)

### 3.4 Recovery Path - Gap Detection

When dirty exchange doesn't converge (data loss scenario).

```
VEHICLE                                           CLOUD
   │                                                │
   │ has: [A, B, C]                                 │ has: [A, B]
   │ dirty: []  (all synced before)                 │ dirty: []
   │ state=0xCCCC                                   │ state=0xBBBB (lost C)
   │                                                │
   │─── V2C SyncMessage ───────────────────────────►│
   │    jobs: []                                    │
   │    acked_jobs: []                              │
   │    state_checksum: 0xCCCC                      │
   │                                                │
   │                      Mismatch, but no dirty!   │
   │                      Need gap detection        │
   │                      Send job_ids list         │
   │                                                │
   │◄─── C2V GapDetect ────────────────────────────│
   │     job_ids: [A, B]                            │
   │     request_job_ids: []                        │
   │                                                │
   │ Compare: I have [A,B,C], cloud has [A,B]       │
   │ Cloud missing: C                               │
   │ I'm missing: nothing                           │
   │                                                │
   │─── V2C GapDetect ─────────────────────────────►│
   │    job_ids: [A, B, C]                          │
   │    request_job_ids: []                         │
   │                                                │
   │─── V2C SyncMessage ───────────────────────────►│
   │    jobs: [C]               ← gap fill          │
   │    acked_jobs: []                              │
   │    state_checksum: 0xCCCC                      │
   │                                                │
   │                      Apply C                   │
   │                      ACK C                     │
   │                                                │
   │◄─── C2V SyncMessage ──────────────────────────│
   │     jobs: []                                   │
   │     acked_jobs: [C@version]  ← ACK             │
   │     state_checksum: 0xCCCC                     │
   │                                                │
   ═══════════════════ QUIESCENT ═══════════════════
```

### 3.5 Symmetric Gap Detection - Both Sides Missing

Both sides lost data.

```
VEHICLE                                           CLOUD
   │                                                │
   │ has: [B, C]  (lost A)                          │ has: [A, B]  (lost C)
   │ dirty: []                                      │ dirty: []
   │                                                │
   │─── V2C SyncMessage ───────────────────────────►│
   │    jobs: []                                    │
   │    acked_jobs: []                              │
   │    state_checksum: 0xVVVV                      │
   │                                                │
   │                      Mismatch, no dirty        │
   │                                                │
   │◄─── C2V GapDetect ────────────────────────────│
   │     job_ids: [A, B]                            │
   │     request_job_ids: []                        │
   │                                                │
   │ Compare lists:                                 │
   │   Cloud has A, I don't → request A            │
   │   I have C, cloud doesn't → send C            │
   │                                                │
   │─── V2C GapDetect ─────────────────────────────►│
   │    job_ids: [B, C]                             │
   │    request_job_ids: [A]    ← request gap      │
   │                                                │
   │─── V2C SyncMessage ───────────────────────────►│
   │    jobs: [C]                                   │
   │    acked_jobs: []                              │
   │    state_checksum: 0xVVV2                      │
   │                                                │
   │                      Apply C                   │
   │                      Vehicle wants A           │
   │                      Send A + ACK C            │
   │                                                │
   │◄─── C2V SyncMessage ──────────────────────────│
   │     jobs: [A]              ← requested         │
   │     acked_jobs: [C@version]  ← ACK             │
   │     state_checksum: 0xFINAL                    │
   │                                                │
   │ Apply A                                        │
   │ ACK A                                          │
   │                                                │
   │─── V2C SyncMessage ───────────────────────────►│
   │    jobs: []                                    │
   │    acked_jobs: [A@version]  ← ACK              │
   │    state_checksum: 0xFINAL                     │
   │                                                │
   ═══════════════════ QUIESCENT ═══════════════════
```

### 3.6 Execution Reporting (With Acknowledgment)

Unchanged from v3.1.

```
VEHICLE                                           CLOUD
   │                                                │
   │ Job X completes                                │
   │                                                │
   │─── V2C_Executions ───────────────────────────►│
   │    executions: [{execution_id, job_id, ...}]   │
   │                                                │
   │                      Store (dedup by exec_id)  │
   │                                                │
   │◄─── C2V_ExecutionAck ────────────────────────│
   │     execution_ids: ["exec-123"]                │
   │                                                │
   │ Remove from retry queue                        │
   │                                                │
```

### 3.7 Job Trigger (Imperative Command)

Unchanged from v3.1.

```
VEHICLE                                           CLOUD
   │                                                │
   │                      User clicks "Run Now"     │
   │                                                │
   │◄─── C2V_TriggerJob ─────────────────────────│
   │     job_id: "job-X"                            │
   │     request_id: "req-456"                      │
   │                                                │
   │ Execute job immediately                        │
   │                                                │
   │─── V2C_TriggerResponse ─────────────────────►│
   │    job_id: "job-X"                             │
   │    request_id: "req-456"                       │
   │    accepted: true                              │
   │                                                │
```

## 4. Per-Job Dirty Tracking

### 4.1 State

Each job tracks whether it needs to be synced to the remote side.

```cpp
struct Job {
    VersionVector local_version;   // My current version (transmitted in JobRecord as "version")
    VersionVector remote_version;  // Last known remote version (LOCAL ONLY - what I believe remote has)

    bool is_dirty() const { return local_version != remote_version; }
};
```

**Note:** `remote_version` is **local state only** - it is NOT transmitted in `JobRecord`. When sending, `local_version` is serialized as the wire `version`. When receiving, the wire `version` represents what the remote believes is its current state.

### 4.2 Update Rules

| Event | local_version | remote_version | is_dirty() |
|-------|---------------|----------------|------------|
| Local create | `{1,0}` or `{0,1}` | `{0,0}` | true |
| Local modify | increment our side | unchanged | true |
| Accept remote | incoming version | incoming version | false |
| Reject remote (we dominate) | unchanged | incoming version | true (remote needs ours) |
| Conflict, we win | merged + increment | incoming version | true (remote needs merged) |
| Conflict, they win | merged + increment | merged version | false |

**Key insight:** `remote_version` is always set to what the remote sent us (incoming version), because
that's what we KNOW they have. When we receive a job with version V, the sender has version V.

**Quiescence:** The system is quiescent when `local_version == remote_version` for ALL jobs.
This is a detected state, not an event. The sync process naturally converges to quiescence
through job exchanges - no explicit "mark all synced" operation is needed.

### 4.3 Why This Works

**Normal sync:** Jobs modified locally have `remote_version != local_version`, so they're dirty and get sent.

**Data loss recovery:** If remote loses data, previously-synced jobs are not dirty (`remote_version == local_version`). But the checksum mismatch with no dirty jobs triggers gap detection, where job ID lists reveal the missing jobs.

## 5. Sync Decision Logic

### 5.1 Cloud: On Receiving V2C SyncMessage

```python
def handle_v2c_sync_message(msg):
    # 1. Process ACKs from vehicle
    for ack in msg.acked_jobs:
        set_job_remote_version(msg.vehicle_id, ack.job_id, ack.cloud_seq, ack.vehicle_seq)

    # 2. Apply received jobs
    acked_jobs = []
    for job in msg.jobs:
        apply_job(msg.vehicle_id, job)  # Sets remote_version = job.version internally
        acked_jobs.append((job.job_id, job.version))

    # 3. Check state
    vehicle_checksum = msg.state_checksum
    cloud_checksum = compute_cloud_checksum(msg.vehicle_id)
    dirty_jobs = get_dirty_jobs(msg.vehicle_id)

    if vehicle_checksum == cloud_checksum and not dirty_jobs:
        # QUIESCENT - checksums match, send ACKs only if any
        send_sync_message(
            jobs=[],
            acked_jobs=acked_jobs,
            state_checksum=cloud_checksum
        )
        return

    if dirty_jobs or acked_jobs:
        # Fast path - send dirty jobs + ACKs
        send_sync_message(
            jobs=dirty_jobs,
            acked_jobs=acked_jobs,
            state_checksum=cloud_checksum
        )
    else:
        # No dirty but mismatch - need gap detection
        all_job_ids = get_all_job_ids(msg.vehicle_id)
        send_gap_detect(
            job_ids=all_job_ids,
            request_job_ids=[]
        )
```

### 5.2 Cloud: On Receiving V2C GapDetect

```python
def handle_v2c_gap_detect(msg):
    # Compare job ID lists
    our_job_ids = set(get_all_job_ids(msg.vehicle_id))
    vehicle_job_ids = set(msg.job_ids)

    # Jobs we need from vehicle
    jobs_to_request = []
    for job_id in vehicle_job_ids:
        if job_id not in our_job_ids:
            jobs_to_request.append(job_id)

    # Jobs vehicle needs from us (send via SyncMessage)
    jobs_to_send = []
    for job_id in our_job_ids:
        if job_id not in vehicle_job_ids:
            job = get_job(msg.vehicle_id, job_id)
            if job:
                jobs_to_send.append(job)

    # Send requested jobs from vehicle
    for job_id in msg.request_job_ids:
        job = get_job(msg.vehicle_id, job_id)
        if job:
            jobs_to_send.append(job)

    # Send gap detection response if we need anything
    if jobs_to_request:
        send_gap_detect(
            job_ids=list(our_job_ids),
            request_job_ids=jobs_to_request
        )

    # Send jobs via SyncMessage
    if jobs_to_send:
        send_sync_message(
            jobs=jobs_to_send,
            acked_jobs=[],
            state_checksum=compute_cloud_checksum(msg.vehicle_id)
        )
```

### 5.3 Vehicle: On Receiving C2V SyncMessage

```python
def handle_c2v_sync_message(msg):
    # 1. Process ACKs from cloud
    for ack in msg.acked_jobs:
        set_job_remote_version(ack.job_id, ack.cloud_seq, ack.vehicle_seq)

    # 2. Apply received jobs
    acked_jobs = []
    for job in msg.jobs:
        apply_job(job)  # Sets remote_version = job.version internally
        acked_jobs.append((job.job_id, job.version))

    # 3. Check state
    cloud_checksum = msg.state_checksum
    our_checksum = compute_checksum()
    dirty_jobs = get_dirty_jobs()

    if our_checksum == cloud_checksum and not dirty_jobs:
        # QUIESCENT - send ACKs only if any
        if acked_jobs:
            send_sync_message(
                jobs=[],
                acked_jobs=acked_jobs,
                state_checksum=our_checksum
            )
        return

    if dirty_jobs or acked_jobs:
        # Fast path - send dirty jobs + ACKs
        send_sync_message(
            jobs=dirty_jobs,
            acked_jobs=acked_jobs,
            state_checksum=our_checksum
        )
    else:
        # No dirty but mismatch - need gap detection
        all_job_ids = get_all_job_ids()
        send_gap_detect(
            job_ids=all_job_ids,
            request_job_ids=[]
        )
```

### 5.4 Vehicle: On Receiving C2V GapDetect

```python
def handle_c2v_gap_detect(msg):
    # Compare job ID lists
    our_job_ids = set(get_all_job_ids())
    cloud_job_ids = set(msg.job_ids)

    # Jobs we need from cloud
    jobs_to_request = []
    for job_id in cloud_job_ids:
        if job_id not in our_job_ids:
            jobs_to_request.append(job_id)

    # Jobs cloud needs from us (send via SyncMessage)
    jobs_to_send = []
    for job_id in our_job_ids:
        if job_id not in cloud_job_ids:
            job = get_job(job_id)
            if job:
                jobs_to_send.append(job)

    # Send requested jobs from cloud
    for job_id in msg.request_job_ids:
        job = get_job(job_id)
        if job:
            jobs_to_send.append(job)

    # Send gap detection response if we need anything
    if jobs_to_request:
        send_gap_detect(
            job_ids=list(our_job_ids),
            request_job_ids=jobs_to_request
        )

    # Send jobs via SyncMessage
    if jobs_to_send:
        send_sync_message(
            jobs=jobs_to_send,
            acked_jobs=[],
            state_checksum=compute_checksum()
        )
```

## 6. Version Vectors & Conflict Resolution

### 6.1 Dominance

Version A **dominates** B if:
```
A.cloud_seq >= B.cloud_seq AND
A.vehicle_seq >= B.vehicle_seq AND
(A.cloud_seq > B.cloud_seq OR A.vehicle_seq > B.vehicle_seq)
```

### 6.2 Conflict Resolution

When neither dominates:
1. Check job's `authority` field
2. `AUTHORITY_CLOUD` → cloud content wins
3. `AUTHORITY_VEHICLE` → vehicle content wins
4. Merged version: `{max(cloud_seq), max(vehicle_seq)}` then increment resolver's side

### 6.3 Partial Ordering

Version vectors are a **partial order**, not a total order:

```
{cloud: 0, vehicle: 2} vs {cloud: 10, vehicle: 0}
```

Neither dominates → **CONFLICT** → resolved by authority.

## 7. Checksum Computation

### 7.1 State Checksum

**Algorithm:** xxHash64

**Input:** All jobs sorted by `job_id`, each job contributing:
- job_id, authority, version.cloud_seq, version.vehicle_seq, deleted
- title, service, method, parameters_json
- scheduled_time_ms, recurrence_rule, end_time_ms
- paused, wake_policy, sleep_policy, wake_lead_time_s

**Excluded:** status, next_run_time_ms, created_at_ms, updated_at_ms, deleted_at_ms

## 8. Tombstone Deletion

- Tombstones are `JobRecord{deleted=true}`
- Included in job ID list and dirty tracking
- GC after confirmed + retention period (7 days)

## 9. Vehicle State Machine

```
                    ┌──────────────────────────────────────┐
                    │                                      │
                    ▼                                      │
            ┌───────────────┐                              │
    ───────►│  SEND_SYNC    │                              │
   connect  └───────┬───────┘                              │
                    │                                      │
                    │ send V2C SyncMessage                 │
                    ▼                                      │
            ┌───────────────┐                              │
            │ WAIT_RESPONSE │                              │
            └───────┬───────┘                              │
                    │                                      │
        ┌───────────┴───────────┐                          │
        │                       │                          │
        ▼                       ▼                          │
   SyncMessage             SyncMessage/GapDetect           │
   (checksums match)       (checksums differ)              │
        │                       │                          │
        ▼                       ▼                          │
┌───────────────┐       ┌───────────────┐                  │
│  QUIESCENT    │       │ PROCESS_MSG   │                  │
└───────┬───────┘       └───────┬───────┘                  │
        │                       │                          │
        │ local change          │ apply jobs, process ACKs │
        │                       │ collect dirty + gaps     │
        │                       ▼                          │
        │               ┌───────────────┐                  │
        │               │  SEND_SYNC    │──────────────────┘
        │               └───────────────┘
        │                       │
        └───────────────────────┘
```

## 10. Implementation Notes

### 10.1 Message Handling

```cpp
void handle_v2c_message(const bytes& payload) {
    V2C_Envelope envelope;
    if (!envelope.ParseFromString(payload)) {
        LOG(WARNING) << "Failed to parse V2C_Envelope";
        return;
    }

    switch (envelope.message_case()) {
        case V2C_Envelope::kSync:
            handle_sync_message(envelope.sync());
            break;
        case V2C_Envelope::kGapDetect:
            handle_gap_detect(envelope.gap_detect());
            break;
        case V2C_Envelope::kExecutions:
            handle_executions(envelope.executions());
            break;
        case V2C_Envelope::kTriggerResponse:
            handle_trigger_response(envelope.trigger_response());
            break;
        case V2C_Envelope::MESSAGE_NOT_SET:
            LOG(WARNING) << "Empty V2C_Envelope";
            break;
    }
}

void handle_c2v_message(const bytes& payload) {
    C2V_Envelope envelope;
    if (!envelope.ParseFromString(payload)) {
        LOG(WARNING) << "Failed to parse C2V_Envelope";
        return;
    }

    switch (envelope.message_case()) {
        case C2V_Envelope::kSync:
            handle_sync_message(envelope.sync());
            break;
        case C2V_Envelope::kGapDetect:
            handle_gap_detect(envelope.gap_detect());
            break;
        case C2V_Envelope::kExecutionAck:
            handle_execution_ack(envelope.execution_ack());
            break;
        case C2V_Envelope::kTriggerJob:
            handle_trigger_job(envelope.trigger_job());
            break;
        case C2V_Envelope::MESSAGE_NOT_SET:
            LOG(WARNING) << "Empty C2V_Envelope";
            break;
    }
}
```

### 10.2 Dirty Job Collection

```cpp
std::vector<Job> get_dirty_jobs() {
    std::vector<Job> dirty;
    for (const auto& [job_id, job] : jobs_) {
        if (job.is_dirty()) {
            dirty.push_back(job);
        }
    }
    return dirty;
}
```

### 10.3 Gap Detection

```cpp
void process_gap_detection(
    const std::vector<std::string>& remote_job_ids,
    std::vector<std::string>& request_from_remote,
    std::vector<Job>& send_to_remote) {

    std::set<std::string> remote_set(remote_job_ids.begin(), remote_job_ids.end());
    std::set<std::string> local_set;
    for (const auto& [job_id, _] : jobs_) {
        local_set.insert(job_id);
    }

    // Jobs remote has that we don't
    for (const auto& job_id : remote_set) {
        if (local_set.find(job_id) == local_set.end()) {
            request_from_remote.push_back(job_id);
        }
    }

    // Jobs we have that remote doesn't
    for (const auto& job_id : local_set) {
        if (remote_set.find(job_id) == remote_set.end()) {
            send_to_remote.push_back(jobs_[job_id]);
        }
    }
}
```

## 11. Migration from v3.1

### 11.1 Breaking Changes

**Removed messages:**
- `V2C_HashManifest` - eliminated
- `C2V_RequestHashes` - eliminated
- `V2C_Hello` - consolidated into `SyncMessage`
- `V2C_JobData` - consolidated into `SyncMessage`
- `C2V_SyncDelta` - consolidated into `SyncMessage`
- `V2C_JobAck` / `C2V_JobAck` - integrated into `SyncMessage.acked_jobs`

**New messages:**
- `SyncMessage` - shared type for jobs + acks + checksum (99% of traffic)
- `GapDetect` - shared type for data loss recovery (job_id lists only, no checksum)

**Key semantic changes:**
- ACKs are integrated into `SyncMessage.acked_jobs`, not separate messages
- Gap detection is separate from normal sync (`GapDetect` vs `SyncMessage`)
- `GapDetect` has no `state_checksum` since we already know states differ
- Quiescence is a detected state, not an event that triggers actions

### 11.2 Migration Path

v3.2 is NOT backward compatible with v3.1. Both sides must upgrade simultaneously.

---

## Appendix A: Test Scenarios

| Scenario | Messages | Bandwidth |
|----------|----------|-----------|
| Quiescent (no changes) | SyncMessage(empty) → SyncMessage(empty) | ~100 B |
| Cloud +5 dirty jobs | SyncMessage → SyncMessage(5 jobs) → SyncMessage(5 acks) | ~2.5 KB |
| Both +5 dirty jobs | SyncMessage → SyncMessage(5 jobs) → SyncMessage(5 jobs+acks) → SyncMessage(acks) | ~5 KB |
| Data loss (gap detection) | SyncMessage → GapDetect(job_ids) → GapDetect + SyncMessage(gaps) → SyncMessage(acks) | ~3.5 KB |
| Execution report | Executions → ExecutionAck | ~250 B |

## Appendix B: Changes from v3.1 to v3.2

| Change | v3.1 | v3.2 |
|--------|------|------|
| Sync approach | Hash-first | Dirty-first |
| Hash manifest | Required for changes | Eliminated |
| Job ID list | Not used | Gap detection only (`GapDetect`) |
| Request direction | Cloud → Vehicle only | Bidirectional |
| Version tracking | Implicit at quiescence | Explicit via `SyncMessage.acked_jobs` |
| Quiescence | Event that marks all synced | Detected state (no action) |
| Message structure | Separate `V2C_Hello`, `V2C_JobData`, etc. | Unified `SyncMessage` + `GapDetect` |
| ACK messages | Separate `V2C_JobAck`, `C2V_JobAck` | Integrated into `SyncMessage.acked_jobs` |
| Round trips (normal) | 3-4 | 2-3 |
| Round trips (data loss) | 3-4 | 3-4 |
