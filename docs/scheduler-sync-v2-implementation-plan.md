# Scheduler Sync Protocol v2 - Implementation Plan

## Overview

Implement the bidirectional sync protocol as specified in `scheduler-sync-protocol-v2.md`.

## Phase 1: Proto Definitions

### 1.1 Create new proto file

**File:** `proto/scheduler-sync-v2.proto`

```protobuf
syntax = "proto3";
package swdv.scheduler_sync_v2;

// Version vector for conflict detection
message JobVersion {
    uint64 cloud_seq = 1;
    uint64 vehicle_seq = 2;
}

enum JobAuthority {
    AUTHORITY_CLOUD = 0;
    AUTHORITY_VEHICLE = 1;
}

enum SyncState {
    SYNC_UNKNOWN = 0;
    SYNC_PENDING = 1;
    SYNC_SYNCED = 2;
    SYNC_CONFLICT = 3;
}

enum JobStatus {
    JOB_PENDING = 0;
    JOB_RUNNING = 1;
    JOB_COMPLETED = 2;
    JOB_FAILED = 3;
    JOB_CANCELLED = 4;
    JOB_PAUSED = 5;
}

message JobRecord {
    string job_id = 1;
    JobAuthority authority = 2;
    JobVersion version = 3;
    SyncState sync_state = 4;
    bool deleted = 5;
    uint64 deleted_at_ms = 6;

    // Content
    string title = 10;
    string service = 11;
    string method = 12;
    string parameters_json = 13;
    uint64 scheduled_time_ms = 14;
    string recurrence_rule = 15;
    uint64 end_time_ms = 16;

    // Execution state
    JobStatus status = 20;
    uint64 next_run_time_ms = 21;
    uint64 last_executed_ms = 22;

    // Metadata
    uint64 created_at_ms = 30;
    uint64 updated_at_ms = 31;
    string created_by = 32;
}

message ExecutionRecord {
    string execution_id = 1;
    string job_id = 2;
    uint64 executed_at_ms = 3;
    uint64 duration_ms = 4;
    JobStatus status = 5;
    string result_json = 6;
    string error_message = 7;
}

// Cloud → Vehicle
message C2V_SyncMessage {
    string vehicle_id = 1;
    repeated JobRecord jobs = 2;
    repeated string deleted_job_ids = 3;
    uint64 sync_timestamp_ms = 4;
}

// Vehicle → Cloud
message V2C_SyncMessage {
    string vehicle_id = 1;
    string bridge_instance_id = 2;
    repeated JobRecord jobs = 3;
    repeated string deleted_job_ids = 4;
    repeated ExecutionRecord executions = 5;
    uint64 sync_timestamp_ms = 6;
}

message ConflictResolution {
    string job_id = 1;
    JobVersion winning_version = 2;
    string winner = 3;
}

message SyncAck {
    bool success = 1;
    repeated ConflictResolution resolutions = 2;
    uint64 ack_timestamp_ms = 3;
}
```

### 1.2 Update generate_proto.sh

Add new proto to generation.

### 1.3 Generate and verify

```bash
./generate_proto.sh
cmake --build build
```

---

## Phase 2: Core Sync Library

### 2.1 Create sync logic library

**Files:**
- `core/sync/include/version_vector.hpp`
- `core/sync/include/sync_engine.hpp`
- `core/sync/src/version_vector.cpp`
- `core/sync/src/sync_engine.cpp`

**Key classes:**

```cpp
namespace ifex::sync {

// Version vector operations
class VersionVector {
public:
    uint64_t cloud_seq = 0;
    uint64_t vehicle_seq = 0;

    bool dominates(const VersionVector& other) const;
    bool equals(const VersionVector& other) const;
    static VersionVector merge(const VersionVector& a, const VersionVector& b);
};

enum class CompareResult {
    EQUAL,
    LOCAL_DOMINATES,
    REMOTE_DOMINATES,
    CONFLICT
};

CompareResult compare(const VersionVector& local, const VersionVector& remote);

// Sync engine
class SyncEngine {
public:
    struct SyncResult {
        enum Action { ACCEPT, REJECT, CONFLICT_RESOLVED };
        Action action;
        JobRecord resolved_job;
        std::string winner; // "cloud" or "vehicle"
    };

    // Process incoming job from remote
    SyncResult processRemoteJob(
        const JobRecord& remote,
        const std::optional<JobRecord>& local,
        bool is_cloud_side);

    // Resolve conflict
    JobRecord resolveConflict(
        const JobRecord& local,
        const JobRecord& remote,
        bool is_cloud_side);
};

}  // namespace ifex::sync
```

### 2.2 Unit tests

**File:** `core/sync/tests/version_vector_test.cpp`

Test cases:
- Dominance detection
- Conflict detection
- Version merging
- Authority-based resolution

---

## Phase 3: Vehicle Side (Onboard)

### 3.1 Update Scheduler Service

**Files to modify:**
- `reference-services/scheduler/include/scheduler_server.hpp`
- `reference-services/scheduler/src/scheduler_server.cpp`

**Changes:**
- Add `JobVersion version` to internal job storage
- Add `JobAuthority authority` field
- Increment `vehicle_seq` on local modifications
- Accept `cloud_seq` updates from sync bridge

### 3.2 Update Scheduler Sync Bridge

**Files to modify:**
- `reference-services/scheduler-sync-bridge/include/scheduler_sync_bridge.hpp`
- `reference-services/scheduler-sync-bridge/src/scheduler_sync_bridge.cpp`

**Changes:**
- Use new `V2C_SyncMessage` format
- Parse `C2V_SyncMessage` from cloud
- Implement sync logic using `SyncEngine`
- Persist version vectors (add to config: `persistence_dir`)
- Track `sync_state` per job

### 3.3 Integration test

**File:** `reference-services/scheduler-sync-bridge/tests/sync_v2_test.cpp`

Test scenarios from spec Appendix B.

---

## Phase 4: Cloud Side (Offboard)

### 4.1 Update Cloud Scheduler Service

**Files to modify:**
- `cloud/cloud-scheduler-service/include/cloud_scheduler_service.hpp`
- `cloud/cloud-scheduler-service/src/cloud_scheduler_service.cpp`

**Changes:**
- Store `JobRecord` with version vectors
- Increment `cloud_seq` on modifications
- Implement `HandleSyncMessage` using `SyncEngine`
- Send `C2V_SyncMessage` on changes

### 4.2 Update proto

**File:** `proto/cloud-scheduler-service.proto`

Add version vector fields to `JobInfo`:
```protobuf
message JobInfo {
    // ... existing fields ...

    // Version vector (new)
    uint64 cloud_seq = 40;
    uint64 vehicle_seq = 41;
    SyncState sync_state = 42;
    JobAuthority authority = 43;
}
```

### 4.3 Integration test

Update `cloud/cloud-scheduler-service/tests/cloud_scheduler_integration_test.cpp`:
- Verify sync_state transitions
- Verify conflict resolution
- Verify version vector merging

---

## Phase 5: End-to-End Testing

### 5.1 Full stack test

**File:** `tests/integration/scheduler_sync_e2e_test.cpp`

Test scenarios:
1. Cloud creates job → vehicle receives → synced
2. Vehicle creates job → cloud receives → synced
3. Both modify offline → reconnect → conflict resolved
4. Delete vs modify conflict
5. Execution during offline period
6. Long offline period (many changes)

### 5.2 Chaos testing

- Random network disconnections
- Simultaneous modifications
- Clock skew simulation (wall time only, not versions)

---

## Phase 6: Migration & Cleanup

### 6.1 Remove old sync protocol

- Remove `scheduler-sync-envelope.proto` references
- Remove `scheduler-command-envelope.proto` references
- Update all imports

### 6.2 Database migration (offboard)

For production offboard services (not in this repo):
```sql
ALTER TABLE jobs ADD COLUMN cloud_seq BIGINT DEFAULT 0;
ALTER TABLE jobs ADD COLUMN vehicle_seq BIGINT DEFAULT 0;
ALTER TABLE jobs ADD COLUMN authority VARCHAR(20) DEFAULT 'AUTHORITY_CLOUD';
ALTER TABLE jobs ADD COLUMN sync_state VARCHAR(20) DEFAULT 'SYNC_PENDING';
```

---

## Implementation Order

```
Week 1: Phase 1 + Phase 2
  - Proto definitions
  - Core sync library with unit tests

Week 2: Phase 3
  - Vehicle scheduler updates
  - Sync bridge updates
  - Vehicle-side integration tests

Week 3: Phase 4
  - Cloud scheduler updates
  - Cloud-side integration tests

Week 4: Phase 5 + Phase 6
  - End-to-end tests
  - Cleanup old protocol
  - Documentation
```

---

## Files Changed Summary

### New Files
- `proto/scheduler-sync-v2.proto`
- `core/sync/include/version_vector.hpp`
- `core/sync/include/sync_engine.hpp`
- `core/sync/src/version_vector.cpp`
- `core/sync/src/sync_engine.cpp`
- `core/sync/tests/version_vector_test.cpp`
- `tests/integration/scheduler_sync_e2e_test.cpp`

### Modified Files
- `proto/cloud-scheduler-service.proto`
- `reference-services/scheduler/include/scheduler_server.hpp`
- `reference-services/scheduler/src/scheduler_server.cpp`
- `reference-services/scheduler-sync-bridge/include/scheduler_sync_bridge.hpp`
- `reference-services/scheduler-sync-bridge/src/scheduler_sync_bridge.cpp`
- `cloud/cloud-scheduler-service/include/cloud_scheduler_service.hpp`
- `cloud/cloud-scheduler-service/src/cloud_scheduler_service.cpp`
- `cloud/cloud-scheduler-service/tests/cloud_scheduler_integration_test.cpp`

### Removed Files (Phase 6)
- `proto/scheduler-sync-envelope.proto` (or deprecate)
- `proto/scheduler-command-envelope.proto` (or deprecate)

---

## Success Criteria

1. All unit tests pass for version vector operations
2. Conflict resolution is deterministic (same inputs → same outputs)
3. Both sides converge to identical state after sync
4. Executions are preserved regardless of job state
5. Protocol works with simulated 4-hour offline period
6. No dependency on wall-clock accuracy for correctness
