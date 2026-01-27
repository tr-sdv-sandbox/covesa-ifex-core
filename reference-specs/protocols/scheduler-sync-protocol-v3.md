# Scheduler Sync Protocol v3 Specification

## Status: DRAFT

**Version:** 3.0
**Date:** 2026-01-21
**Authors:** Claude + Human

## 1. Overview

### 1.1 Purpose

This specification defines a bandwidth-efficient bidirectional synchronization protocol for scheduled jobs between cloud (offboard) and vehicle (onboard) systems.

**Key improvements over v2:**
- Hash-first sync: Exchange checksums/hashes before full data
- Separate message types: Clear purpose for each message
- Independent execution stream: Fire-and-forget execution reporting
- Optimized reconnect: Minimize data when state unchanged

### 1.2 Design Goals

| Goal | Approach |
|------|----------|
| Bandwidth efficiency | Hash-first comparison, only transfer diffs |
| Offline support | Both sides operate independently |
| Automatic resolution | Authority-based conflict resolution |
| Clock independence | Logical clocks (version vectors) |
| Low latency executions | Independent stream, immediate send |
| Simple reconnect | Single checksum comparison first |

### 1.3 Bandwidth Comparison

| Scenario | v2 (full sync) | v3 (hash-first) |
|----------|----------------|-----------------|
| 100 jobs, 5 differ | ~30 KB | ~6 KB |
| 10,000 jobs, 50 differ | ~3 MB | ~500 KB |
| Reconnect, no changes | ~30 KB | ~100 bytes |

## 2. Message Types

### 2.1 Overview

All messages use **content_id = 202**. Message type is discriminated by protobuf type.

**Vehicle → Cloud (V2C):**

| Message | Purpose | When sent |
|---------|---------|-----------|
| `V2C_Hello` | Announce state checksum | On connect, after applying changes |
| `V2C_HashManifest` | Send all job hashes | When cloud requests |
| `V2C_JobData` | Send full job records | When cloud requests specific jobs |
| `V2C_Executions` | Report execution results | Immediately when jobs complete |

**Cloud → Vehicle (C2V):**

| Message | Purpose | When sent |
|---------|---------|-----------|
| `C2V_RequestHashes` | Request hash manifest | When vehicle checksum unknown |
| `C2V_SyncDelta` | Send jobs + request jobs | After comparing hashes |

### 2.2 Protocol Buffers

```protobuf
syntax = "proto3";
package swdv.scheduler_sync_v3;

// ============================================================================
// Vehicle → Cloud
// ============================================================================

// Initial handshake / state announcement
// Sent on connect and after applying any changes
message V2C_Hello {
    string vehicle_id = 1;
    string bridge_instance_id = 2;      // Detect bridge restarts
    uint64 state_checksum = 3;           // xxHash64 of all jobs
    uint64 last_seen_c2v_checksum = 4;   // Last cloud checksum we processed
}

// Job hash manifest (sent when cloud requests)
message V2C_HashManifest {
    string vehicle_id = 1;
    repeated JobHashEntry job_hashes = 2;
    uint64 state_checksum = 3;
}

// Full job data (sent when cloud requests specific jobs)
message V2C_JobData {
    string vehicle_id = 1;
    repeated JobRecord jobs = 2;
    uint64 state_checksum = 3;
}

// Execution results (independent stream - fire and forget)
message V2C_Executions {
    string vehicle_id = 1;
    repeated ExecutionRecord executions = 2;
}

// ============================================================================
// Cloud → Vehicle
// ============================================================================

// Request hash manifest (when vehicle checksum is unknown/changed)
message C2V_RequestHashes {
    string vehicle_id = 1;
    uint64 cloud_state_checksum = 2;     // Inform vehicle of cloud state
}

// Sync delta - request specific jobs and/or send jobs
message C2V_SyncDelta {
    string vehicle_id = 1;
    repeated string request_job_ids = 2;  // Jobs cloud needs from vehicle
    repeated JobRecord jobs = 3;           // Jobs vehicle needs from cloud
    uint64 state_checksum = 4;
    uint64 last_seen_v2c_checksum = 5;
}

// ============================================================================
// Shared Types
// ============================================================================

message JobHashEntry {
    string job_id = 1;
    uint64 content_hash = 2;              // xxHash64 of job content
    JobVersion version = 3;                // For quick dominance check
    bool deleted = 4;                      // Tombstone flag
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
    string execution_id = 1;              // Globally unique (for dedup)
    string job_id = 2;
    uint64 executed_at_ms = 3;
    uint64 duration_ms = 4;
    JobStatus status = 5;                 // COMPLETED or FAILED
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

### 3.1 Reconnect - Vehicle Unchanged, Cloud Has Changes

Most common case: vehicle reconnects, only cloud has new jobs.

```
VEHICLE                                           CLOUD
   │                                                │
   │ state=0xAAAA                                   │ state=0xBBBB
   │                                                │ last_seen_v2c=0xAAAA
   │                                                │
   │─── V2C_Hello ─────────────────────────────────►│
   │    state_checksum: 0xAAAA                      │
   │                                                │
   │                      0xAAAA == last_seen_v2c ✓ │
   │                      Vehicle unchanged!        │
   │                      Just send our changes     │
   │                                                │
   │◄─── C2V_SyncDelta ─────────────────────────────│
   │     request_job_ids: []                        │
   │     jobs: [job-X, job-Y]  ← new cloud jobs     │
   │     state_checksum: 0xBBBB                     │
   │                                                │
   │ Apply jobs                                     │
   │ new state=0xBBBB                               │
   │                                                │
   │─── V2C_Hello ─────────────────────────────────►│
   │    state_checksum: 0xBBBB                      │
   │    last_seen_c2v: 0xBBBB                       │
   │                                                │
   │                      Checksums match!          │
   │                      QUIESCENT                 │
   │                                                │
   ═══════════════ NO TRAFFIC UNTIL CHANGE ═════════
```

**Bandwidth:** ~200 bytes total (just checksums + delta jobs)

### 3.2 Reconnect - Both Sides Changed

Vehicle and cloud both modified jobs while disconnected.

```
VEHICLE                                           CLOUD
   │                                                │
   │ state=0xCCCC (changed offline)                 │ state=0xBBBB
   │                                                │ last_seen_v2c=0xAAAA
   │                                                │
   │─── V2C_Hello ─────────────────────────────────►│
   │    state_checksum: 0xCCCC                      │
   │                                                │
   │                      0xCCCC != last_seen_v2c   │
   │                      Vehicle changed!          │
   │                      Need hash manifest        │
   │                                                │
   │◄─── C2V_RequestHashes ─────────────────────────│
   │     cloud_state_checksum: 0xBBBB               │
   │                                                │
   │─── V2C_HashManifest ──────────────────────────►│
   │    job_hashes: [                               │
   │      {A, 0x111, v:{3,4}},                      │
   │      {B, 0x222, v:{2,5}},  ← vehicle modified  │
   │      {C, 0x333, v:{1,1}},                      │
   │      ... 100 entries                           │
   │    ]                                           │
   │    state_checksum: 0xCCCC                      │
   │                                                │
   │                      Compare each hash:        │
   │                      A: same                   │
   │                      B: differs (veh)          │
   │                      C: same                   │
   │                      X: missing (cloud has)    │
   │                                                │
   │◄─── C2V_SyncDelta ─────────────────────────────│
   │     request_job_ids: ["B"]  ← need from veh    │
   │     jobs: [job-X]           ← veh needs        │
   │     state_checksum: 0xDDDD                     │
   │                                                │
   │ Apply job-X                                    │
   │                                                │
   │─── V2C_JobData ───────────────────────────────►│
   │    jobs: [job-B]                               │
   │    state_checksum: 0xEEEE                      │
   │                                                │
   │                      Apply job-B               │
   │                      Cloud state now 0xEEEE   │
   │                                                │
   │◄─── C2V_SyncDelta ─────────────────────────────│
   │     request_job_ids: []                        │
   │     jobs: []                                   │
   │     state_checksum: 0xEEEE                     │
   │     last_seen_v2c: 0xEEEE                      │
   │                                                │
   │                      Checksums match!          │
   │                                                │
   ═══════════════════ QUIESCENT ═══════════════════
```

### 3.3 Reconnect - No Changes (Quiescent)

Vehicle reconnects, neither side changed.

```
VEHICLE                                           CLOUD
   │                                                │
   │ state=0xAAAA                                   │ state=0xAAAA
   │                                                │ last_seen_v2c=0xAAAA
   │                                                │
   │─── V2C_Hello ─────────────────────────────────►│
   │    state_checksum: 0xAAAA                      │
   │    last_seen_c2v: 0xAAAA                       │
   │                                                │
   │                      0xAAAA == last_seen_v2c ✓ │
   │                      0xAAAA == our state ✓     │
   │                      Already in sync!          │
   │                                                │
   │◄─── C2V_SyncDelta ─────────────────────────────│
   │     request_job_ids: []                        │
   │     jobs: []                                   │
   │     state_checksum: 0xAAAA                     │
   │     last_seen_v2c: 0xAAAA                      │
   │                                                │
   ═══════════════════ QUIESCENT ═══════════════════
```

**Bandwidth:** ~100 bytes (just checksums)

### 3.4 Execution Reporting (Independent Stream)

Executions are sent immediately, independent of sync state.

```
VEHICLE                                           CLOUD
   │                                                │
   │ Job X completes                                │
   │                                                │
   │─── V2C_Executions ────────────────────────────►│
   │    executions: [{                              │
   │      execution_id: "exec-123",                 │
   │      job_id: "job-X",                          │
   │      status: COMPLETED,                        │
   │      duration_ms: 1500,                        │
   │      result_json: "{...}"                      │
   │    }]                                          │
   │                                                │
   │                      Store execution           │
   │                      (dedup by execution_id)   │
   │                                                │
   │ Job Y fails                                    │
   │                                                │
   │─── V2C_Executions ────────────────────────────►│
   │    executions: [{                              │
   │      execution_id: "exec-124",                 │
   │      job_id: "job-Y",                          │
   │      status: FAILED,                           │
   │      error_message: "Service unavailable"      │
   │    }]                                          │
   │                                                │
```

**Key properties:**
- Fire and forget (no ack)
- Sent immediately when job completes
- Cloud deduplicates by `execution_id`
- Independent of job sync state
- Can batch multiple executions

## 4. Cloud Decision Logic

### 4.1 On Receiving V2C_Hello

```python
def handle_v2c_hello(msg):
    vehicle_checksum = msg.state_checksum
    known_checksum = get_last_seen_v2c_checksum(msg.vehicle_id)
    our_checksum = get_cloud_state_checksum(msg.vehicle_id)

    if vehicle_checksum == known_checksum:
        # We know vehicle's state - just send our changes
        if vehicle_checksum == our_checksum:
            # Already in sync - send empty delta (confirm quiescent)
            send_c2v_sync_delta(
                request_job_ids=[],
                jobs=[],
                state_checksum=our_checksum
            )
        else:
            # Vehicle unchanged, but we have changes
            changed_jobs = get_jobs_changed_since(known_checksum)
            send_c2v_sync_delta(
                request_job_ids=[],
                jobs=changed_jobs,
                state_checksum=our_checksum
            )
    else:
        # Vehicle changed - we need their hash manifest
        send_c2v_request_hashes(
            cloud_state_checksum=our_checksum
        )
```

### 4.2 On Receiving V2C_HashManifest

```python
def handle_v2c_hash_manifest(msg):
    request_jobs = []
    send_jobs = []

    vehicle_hashes = {h.job_id: h for h in msg.job_hashes}
    cloud_jobs = get_all_jobs(msg.vehicle_id)

    # Check each cloud job
    for job in cloud_jobs:
        if job.job_id in vehicle_hashes:
            vh = vehicle_hashes[job.job_id]
            if vh.content_hash != job.content_hash:
                # Content differs - compare versions
                if vehicle_dominates(vh.version, job.version):
                    request_jobs.append(job.job_id)
                elif cloud_dominates(job.version, vh.version):
                    send_jobs.append(job)
                else:
                    # Conflict - resolve by authority
                    winner = resolve_conflict(job, vh)
                    if winner == 'vehicle':
                        request_jobs.append(job.job_id)
                    else:
                        send_jobs.append(job)
        else:
            # Cloud has job vehicle doesn't
            send_jobs.append(job)

    # Check for jobs vehicle has that cloud doesn't
    for job_id in vehicle_hashes:
        if job_id not in cloud_jobs:
            request_jobs.append(job_id)

    send_c2v_sync_delta(
        request_job_ids=request_jobs,
        jobs=send_jobs,
        state_checksum=compute_checksum()
    )
```

## 5. Version Vectors & Conflict Resolution

(Unchanged from v2 - see scheduler-sync-protocol-v2.md sections 3-4)

### 5.1 Dominance

Version A **dominates** B if:
```
A.cloud_seq >= B.cloud_seq AND
A.vehicle_seq >= B.vehicle_seq AND
(A.cloud_seq > B.cloud_seq OR A.vehicle_seq > B.vehicle_seq)
```

### 5.2 Conflict Resolution

When neither dominates:
1. Check job's `authority` field
2. `AUTHORITY_CLOUD` → cloud content wins
3. `AUTHORITY_VEHICLE` → vehicle content wins
4. Merged version: `{max(cloud_seq)+1, max(vehicle_seq)}` (cloud resolving)

## 6. Checksum Computation

### 6.1 State Checksum

**Algorithm:** xxHash64

**Input:** All jobs sorted by `job_id`, each job contributing:
- job_id, authority, version.cloud_seq, version.vehicle_seq, deleted
- title, service, method, parameters_json
- scheduled_time_ms, recurrence_rule, end_time_ms
- paused, wake_policy, sleep_policy, wake_lead_time_s

**Excluded:** status, next_run_time_ms, created_at_ms, updated_at_ms, deleted_at_ms

### 6.2 Job Content Hash

Same fields as state checksum, but for a single job.

## 7. Tombstone Deletion

(Unchanged from v2 - see scheduler-sync-protocol-v2.md section 4.3)

- Tombstones are `JobRecord{deleted=true}`
- Included in hash manifest with `deleted=true`
- GC after confirmed + retention period (7 days)

## 8. Execution Records

### 8.1 Properties

- **Append-only:** Immutable facts, never modified
- **Independent:** Not part of job sync, own message type
- **Immediate:** Sent as soon as job completes
- **Deduplicated:** Cloud stores by `execution_id`
- **No ack:** Fire and forget

### 8.2 Vehicle Behavior

```python
def on_job_completed(job, result):
    execution = ExecutionRecord(
        execution_id=generate_uuid(),
        job_id=job.job_id,
        executed_at_ms=now_ms(),
        duration_ms=result.duration,
        status=result.status,
        result_json=result.output,
        error_message=result.error
    )

    # Send immediately
    send_v2c_executions([execution])

    # Also queue for retry if offline
    queue_for_retry(execution)
```

### 8.3 Cloud Behavior

```python
def handle_v2c_executions(msg):
    for exec in msg.executions:
        # Deduplicate by execution_id
        if not execution_exists(exec.execution_id):
            store_execution(msg.vehicle_id, exec)
```

## 9. Implementation Notes

### 9.1 Message Discrimination

All messages on content_id 202. Discriminate by attempting parse:

```cpp
bool handle_v2c_message(const bytes& payload) {
    // Try each message type
    V2C_Hello hello;
    if (hello.ParseFromString(payload) && !hello.vehicle_id().empty()) {
        return handle_hello(hello);
    }

    V2C_HashManifest manifest;
    if (manifest.ParseFromString(payload) && manifest.job_hashes_size() > 0) {
        return handle_hash_manifest(manifest);
    }

    V2C_JobData job_data;
    if (job_data.ParseFromString(payload) && job_data.jobs_size() > 0) {
        return handle_job_data(job_data);
    }

    V2C_Executions executions;
    if (executions.ParseFromString(payload) && executions.executions_size() > 0) {
        return handle_executions(executions);
    }

    return false;  // Unknown message
}
```

### 9.2 Vehicle State Machine

```
                    ┌──────────────────────────────────────┐
                    │                                      │
                    ▼                                      │
            ┌───────────────┐                              │
    ───────►│   SEND_HELLO  │                              │
   connect  └───────┬───────┘                              │
                    │                                      │
                    │ send V2C_Hello                       │
                    ▼                                      │
            ┌───────────────┐                              │
            │ WAIT_RESPONSE │                              │
            └───────┬───────┘                              │
                    │                                      │
        ┌───────────┼───────────┐                          │
        │           │           │                          │
        ▼           ▼           ▼                          │
   C2V_Request  C2V_Sync    C2V_Sync                       │
   Hashes       Delta       Delta                          │
   (need hash)  (has jobs)  (empty)                        │
        │           │           │                          │
        ▼           │           ▼                          │
┌───────────────┐   │   ┌───────────────┐                  │
│ SEND_MANIFEST │   │   │  QUIESCENT    │◄─────────────────┤
└───────┬───────┘   │   └───────┬───────┘                  │
        │           │           │                          │
        │           │           │ local change             │
        │           ▼           │                          │
        │   ┌───────────────┐   │                          │
        │   │ APPLY_CHANGES │───┘                          │
        │   └───────┬───────┘                              │
        │           │                                      │
        │           │ if request_job_ids not empty         │
        │           ▼                                      │
        │   ┌───────────────┐                              │
        └──►│ SEND_JOBDATA  │──────────────────────────────┘
            └───────────────┘
```

## 10. Migration from v2

### 10.1 Compatibility

v3 is NOT backward compatible with v2. Both sides must upgrade.

### 10.2 Migration Steps

1. Deploy cloud with v3 support (accept both v2 and v3)
2. Deploy vehicles with v3
3. Vehicles send V2C_Hello (v3 format)
4. Cloud responds with v3 messages
5. After all vehicles upgraded, remove v2 support

### 10.3 Version Detection

Cloud can detect v2 vs v3 by message format:
- v2: `V2C_SyncMessage` has `jobs` field populated
- v3: `V2C_Hello` has only checksums, no jobs

---

## Appendix A: Comparison with Discovery Sync

| Aspect | Discovery Sync | Scheduler Sync v3 |
|--------|----------------|-------------------|
| Data type | Static schemas | Dynamic jobs |
| Hash level | Schema content | Job content |
| Conflict | N/A (immutable) | Version vectors + authority |
| Direction | Mostly V2C | Bidirectional |
| First message | Hash list | Single checksum |
| Request granularity | By hash | By job_id |

## Appendix B: Test Scenarios

| Scenario | Messages | Bandwidth |
|----------|----------|-----------|
| Reconnect, no change | Hello → SyncDelta(empty) | ~100 B |
| Reconnect, cloud +5 jobs | Hello → SyncDelta(5 jobs) | ~2 KB |
| Reconnect, both +5 jobs | Hello → RequestHashes → Manifest → SyncDelta → JobData | ~8 KB |
| 10K jobs, 50 differ | Hello → RequestHashes → Manifest → SyncDelta → JobData | ~500 KB |
| Execution report | Executions | ~200 B |
