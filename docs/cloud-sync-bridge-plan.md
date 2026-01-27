# Cloud Scheduler Sync Bridge Architecture Plan

## Problem Statement

Currently, the sync protocol logic is duplicated:
1. **ifex-core** `cloud/cloud-scheduler-service/` - In-memory, for testing
2. **offboard-services** `services/ingestion/scheduler_mirror/` - PostgreSQL, for production

Both implement Scheduler Sync Protocol v2 but separately, leading to:
- Code duplication
- Potential protocol divergence
- Testing one implementation doesn't validate the other

## Goal

Create a **generic cloud-scheduler-sync-bridge** that:
1. Uses IFEX-generated gRPC interfaces (no custom abstractions)
2. Works with ANY implementation of cloud-scheduler-service
3. Works with ANY implementation of cloud-backend-transport
4. Lives in ifex-core - same binary for test and production

## Key Design Principle: Separation of Concerns

**Scheduler Service = Storage + Checksum**
- Stores jobs with version vector fields (cloud_seq, vehicle_seq, authority, etc.)
- Stores sync state (checksums) per vehicle
- **Recomputes cloud_checksum on every job create/update/delete** (uses libs/scheduler/checksum.hpp)
- Exposes generic CRUD operations on jobs
- Does NOT know about sync protocol wire format (V2C/C2V messages)
- Does NOT implement version vector comparison logic

**Sync Bridge = Protocol**
- Owns the sync protocol wire format (V2C_SyncMessage, C2V_SyncMessage)
- Parses incoming V2C messages, extracts job data
- Compares version vectors, decides what to update
- Detects quiescence by comparing checksums (cloud vs last_seen_v2c)
- Builds outgoing C2V messages
- Calls scheduler's generic job API

**Why this separation?**
- Sync protocol can evolve without changing scheduler API
- Scheduler implementations (in-memory, PostgreSQL) stay simple
- Single place to test/debug sync protocol logic
- Clear boundary: scheduler = data, bridge = protocol

## Architecture

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              CLOUD                                          │
│                                                                             │
│  Dashboard/API                                                              │
│       │                                                                     │
│       │ gRPC                                                                │
│       ▼                                                                     │
│  ┌─────────────────────────────────────┐                                   │
│  │     cloud-scheduler-service          │  ◄── IFEX gRPC interface          │
│  │                                      │                                   │
│  │  Dashboard API:                      │                                   │
│  │  • create_job, update_job, delete_job│                                   │
│  │  • list_jobs, get_job, trigger_job   │                                   │
│  │                                      │                                   │
│  │  Internal API (for sync bridge):     │                                   │
│  │  • get_jobs_for_vehicle              │                                   │
│  │  • upsert_job (with version vector)  │                                   │
│  │  • record_execution                  │                                   │
│  │  • get_vehicle_sync_state            │                                   │
│  │  • update_vehicle_sync_state         │                                   │
│  │                                      │                                   │
│  │  ┌─────────────────────────────────┐ │                                   │
│  │  │ IMPLEMENTATIONS:                │ │                                   │
│  │  │                                 │ │                                   │
│  │  │ ifex-core (testing):            │ │                                   │
│  │  │   std::map in-memory            │ │                                   │
│  │  │                                 │ │                                   │
│  │  │ offboard-services (production): │ │                                   │
│  │  │   PostgreSQL                    │ │                                   │
│  │  └─────────────────────────────────┘ │                                   │
│  └──────────────────┬──────────────────┘                                   │
│                     │                                                       │
│                     │ gRPC (generic job methods)                            │
│                     ▼                                                       │
│  ┌─────────────────────────────────────┐                                   │
│  │   cloud-scheduler-sync-bridge        │  ◄── ONE impl, lives in ifex-core │
│  │                                      │                                   │
│  │  PROTOCOL LOGIC (owns these types):  │                                   │
│  │  • V2C_SyncMessage, C2V_SyncMessage  │                                   │
│  │  • Version vector comparison         │                                   │
│  │  • Checksum computation              │                                   │
│  │  • Quiescence detection              │                                   │
│  │                                      │                                   │
│  │  OPERATIONS:                         │                                   │
│  │  • Subscribes to on_vehicle_message  │                                   │
│  │  • Subscribes to on_vehicle_status   │                                   │
│  │  • Parses V2C, calls scheduler CRUD  │                                   │
│  │  • Builds C2V, sends via transport   │                                   │
│  │                                      │                                   │
│  │  ONLY knows gRPC interfaces          │                                   │
│  └──────────────────┬──────────────────┘                                   │
│                     │                                                       │
│                     │ gRPC (transport methods)                              │
│                     ▼                                                       │
│  ┌─────────────────────────────────────┐                                   │
│  │   cloud-backend-transport            │  ◄── IFEX gRPC interface          │
│  │                                      │                                   │
│  │  • send_to_vehicle                   │                                   │
│  │  • on_vehicle_message (stream)       │                                   │
│  │  • on_vehicle_status (stream)        │                                   │
│  │                                      │                                   │
│  │  ┌─────────────────────────────────┐ │                                   │
│  │  │ IMPLEMENTATIONS:                │ │                                   │
│  │  │                                 │ │                                   │
│  │  │ ifex-core (testing):            │ │                                   │
│  │  │   MQTT direct                   │ │                                   │
│  │  │                                 │ │                                   │
│  │  │ offboard-services (production): │ │                                   │
│  │  │   Kafka + mqtt_kafka_bridge     │ │                                   │
│  │  └─────────────────────────────────┘ │                                   │
│  └──────────────────┬──────────────────┘                                   │
│                     │                                                       │
└─────────────────────┼───────────────────────────────────────────────────────┘
                      │
                      ▼
                   Vehicle
```

## IFEX Changes

### 1. Extend cloud-scheduler-service.ifex.yml with Internal Methods

The scheduler service exposes generic job CRUD with version vector fields.
It does NOT know about sync protocol message formats (V2C/C2V).

```yaml
# Additional structs for job with version vector fields
# (extend existing job_info_t or create new type)

structs:
  - name: job_with_version_t
    description: Job record with version vector fields for sync
    members:
      - name: job_id
        datatype: string
      - name: vehicle_id
        datatype: string
      - name: title
        datatype: string
      - name: service
        datatype: string
      - name: method
        datatype: string
      - name: parameters_json
        datatype: string
        mandatory: false
      - name: scheduled_time_ms
        datatype: uint64
      - name: recurrence_rule
        datatype: string
        mandatory: false
      - name: paused
        datatype: boolean
        mandatory: false
      - name: status
        datatype: scheduler_types.job_status_t
      - name: wake_policy
        datatype: scheduler_types.wake_policy_t
        mandatory: false
      - name: sleep_policy
        datatype: scheduler_types.sleep_policy_t
        mandatory: false
      - name: wake_lead_time_s
        datatype: uint32
        mandatory: false
      # Version vector fields (stored in DB, used by sync bridge)
      - name: authority
        datatype: scheduler_types.job_authority_t
      - name: cloud_seq
        datatype: uint64
      - name: vehicle_seq
        datatype: uint64
      - name: deleted
        datatype: boolean
        mandatory: false
      - name: deleted_at_ms
        datatype: uint64
        mandatory: false

  - name: execution_record_t
    description: Job execution record (append-only)
    members:
      - name: execution_id
        datatype: string
      - name: job_id
        datatype: string
      - name: vehicle_id
        datatype: string
      - name: executed_at_ms
        datatype: uint64
      - name: duration_ms
        datatype: uint32
      - name: status
        datatype: scheduler_types.job_status_t
      - name: result_json
        datatype: string
        mandatory: false
      - name: error_message
        datatype: string
        mandatory: false

  - name: vehicle_sync_state_t
    description: Per-vehicle sync state (checksums for quiescence detection)
    members:
      - name: vehicle_id
        datatype: string
      - name: cloud_checksum
        datatype: uint64
        description: >
          Checksum of cloud's job state for this vehicle.
          MAINTAINED BY SCHEDULER: recomputed on every job create/update/delete.
      - name: last_seen_v2c_checksum
        datatype: uint64
        description: Last V2C checksum received from vehicle (updated by sync bridge)
      - name: last_sync_timestamp_ms
        datatype: uint64

# Internal methods for sync bridge (generic CRUD, no protocol types)

methods:
  - name: get_jobs_for_vehicle
    description: >
      Get all jobs assigned to a vehicle (including tombstones).
      Used by sync bridge to build C2V messages.
    input:
      - name: vehicle_id
        datatype: string
        mandatory: true
      - name: include_deleted
        datatype: boolean
        description: Include soft-deleted jobs (tombstones)
        mandatory: false
    output:
      - name: jobs
        datatype: job_with_version_t[]

  - name: upsert_job
    description: >
      Create or update a job with version vector.
      Used by sync bridge when processing V2C messages.
      Scheduler stores the data, does NOT decide conflicts.
    input:
      - name: job
        datatype: job_with_version_t
        mandatory: true
    output:
      - name: success
        datatype: boolean
      - name: updated_job
        datatype: job_with_version_t

  - name: record_execution
    description: >
      Record a job execution (append-only, idempotent by execution_id).
      Used by sync bridge when processing V2C execution reports.
    input:
      - name: execution
        datatype: execution_record_t
        mandatory: true
    output:
      - name: success
        datatype: boolean
      - name: is_duplicate
        datatype: boolean
        description: True if execution_id already existed

  - name: get_vehicle_sync_state
    description: >
      Get sync state for a vehicle (checksums for quiescence detection).
    input:
      - name: vehicle_id
        datatype: string
        mandatory: true
    output:
      - name: state
        datatype: vehicle_sync_state_t
      - name: found
        datatype: boolean

  - name: update_vehicle_sync_state
    description: >
      Update sync state for a vehicle after processing V2C message.
    input:
      - name: state
        datatype: vehicle_sync_state_t
        mandatory: true
    output:
      - name: success
        datatype: boolean
```

**Note:** The sync bridge owns V2C_SyncMessage and C2V_SyncMessage types (defined in
`proto/internal/scheduler-sync-v2.proto`). The scheduler only stores jobs and sync state.

### 2. Create cloud-scheduler-sync-bridge.ifex.yml

```yaml
name: cloud_scheduler_sync_bridge
major_version: 1
minor_version: 0
description: >
  Generic cloud-side scheduler sync bridge.
  Handles bidirectional sync between cloud scheduler and vehicles.
  Uses gRPC interfaces only - agnostic to scheduler/transport implementations.

namespaces:
  - name: bridge
    description: Sync bridge control and monitoring

    structs:
      - name: bridge_stats_t
        description: Bridge statistics
        members:
          - name: v2c_messages_processed
            datatype: uint64
          - name: c2v_messages_sent
            datatype: uint64
          - name: vehicles_synced
            datatype: uint32
          - name: quiescent_skipped
            datatype: uint64
          - name: errors
            datatype: uint64
          - name: uptime_seconds
            datatype: uint64

    methods:
      - name: get_stats
        description: Get bridge statistics
        output:
          - name: stats
            datatype: bridge_stats_t

      - name: force_vehicle_sync
        description: Force sync for a specific vehicle
        input:
          - name: vehicle_id
            datatype: string
            mandatory: true
        output:
          - name: success
            datatype: boolean
          - name: error_message
            datatype: string
            mandatory: false

      - name: healthy
        description: Check bridge health
        output:
          - name: is_healthy
            datatype: boolean
          - name: scheduler_connected
            datatype: boolean
          - name: transport_connected
            datatype: boolean
```

## Implementation Structure

```
ifex-core/
├── reference-specs/cloud/
│   ├── cloud-scheduler-service.ifex.yml      # Add internal methods for sync bridge
│   └── cloud-scheduler-sync-bridge.ifex.yml  # Bridge monitoring/control API
│
├── proto/internal/
│   └── scheduler-sync-v2.proto               # V2C/C2V message types (bridge owns)
│
├── libs/scheduler/                           # EXISTING shared library
│   └── include/ifex/scheduler/
│       ├── sync_engine.hpp                   # Version vector comparison (bridge)
│       └── checksum.hpp                      # xxHash64 (scheduler + bridge)
│
├── cloud/
│   ├── cloud-scheduler-service/              # EXISTING - add internal methods
│   │   └── src/
│   │       └── cloud_scheduler_service.cpp   # Links ifex-scheduler for checksum
│   │                                         # Recomputes cloud_checksum on job changes
│   │
│   └── cloud-scheduler-sync-bridge/          # NEW - protocol logic lives here
│       ├── CMakeLists.txt
│       ├── include/
│       │   └── cloud_scheduler_sync_bridge.hpp
│       └── src/
│           ├── cloud_scheduler_sync_bridge.cpp  # Protocol logic
│           └── main.cpp


offboard-services/
├── services/api/scheduler/
│   └── scheduler_service_impl.cpp            # Same IFEX interface (PostgreSQL impl)
│
└── deploy/
    └── docker-compose.yml                    # Run sync bridge from ifex-core
```

## Sync Bridge Implementation

The sync bridge is a gRPC client that **owns all sync protocol logic**:

```cpp
class CloudSchedulerSyncBridge {
public:
    CloudSchedulerSyncBridge(
        const std::string& scheduler_endpoint,    // e.g., "localhost:50102"
        const std::string& transport_endpoint,    // e.g., "localhost:50200"
        uint32_t content_id = 202);

    void Start();
    void Stop();

private:
    // gRPC stubs (generated from IFEX)
    std::unique_ptr<cloud_scheduler_service::Stub> scheduler_stub_;
    std::unique_ptr<cloud_backend_transport::Stub> transport_stub_;

    // Event handlers
    void OnVehicleMessage(const vehicle_message_t& msg);
    void OnVehicleOnline(const std::string& vehicle_id);

    // === PROTOCOL LOGIC (sync bridge owns this) ===

    // Parse V2C wire format, extract jobs/executions
    V2C_SyncMessage ParseV2CMessage(const bytes& payload);

    // Compare version vectors, decide what to update
    // Uses ifex::scheduler::SyncEngine from libs/scheduler/
    std::vector<JobUpdate> ResolveConflicts(
        const std::vector<job_with_version_t>& cloud_jobs,
        const std::vector<JobRecord>& vehicle_jobs);

    // Apply updates to scheduler via generic CRUD
    void ApplyJobUpdates(const std::vector<JobUpdate>& updates);

    // Build C2V message from current scheduler state
    C2V_SyncMessage BuildC2VMessage(const std::string& vehicle_id);

    // Check if sync is needed (compare cloud_checksum from scheduler vs v2c_checksum)
    // Note: cloud_checksum is maintained BY THE SCHEDULER on job changes
    bool IsQuiescent(const vehicle_sync_state_t& state, uint64_t v2c_checksum);
};
```

### Protocol Flow

```
Vehicle → V2C message (protobuf) → Backend Transport
                                        │
                                        ▼
                              Sync Bridge receives bytes
                                        │
                                        ▼
                              ParseV2CMessage() → extract jobs, executions
                                        │
                                        ▼
                              scheduler.get_jobs_for_vehicle(vid)
                                        │
                                        ▼
                              ResolveConflicts() → compare version vectors
                                        │
                                        ▼
                              scheduler.upsert_job() for each update
                              scheduler.record_execution() for each exec
                                        │
                                        ▼
                              BuildC2VMessage() → get current cloud state
                                        │
                                        ▼
                              transport.send_to_vehicle(vid, c2v_bytes)
```

## Deployment

### Testing (ifex-core)

```bash
# Terminal 1: Scheduler (in-memory)
./cloud-scheduler-service --listen=0.0.0.0:50102

# Terminal 2: Transport (MQTT)
./cloud-backend-transport --listen=0.0.0.0:50200 --mqtt-host=localhost

# Terminal 3: Sync Bridge
./cloud-scheduler-sync-bridge \
    --scheduler=localhost:50102 \
    --transport=localhost:50200 \
    --content-id=202
```

### Production (offboard-services)

```bash
# scheduler_api (PostgreSQL) - already running on :50102
# mqtt_kafka_bridge + cloud-backend-transport adapter - on :50200

# Sync Bridge (SAME BINARY from ifex-core)
./cloud-scheduler-sync-bridge \
    --scheduler=localhost:50102 \
    --transport=localhost:50200 \
    --content-id=202
```

## Migration Plan

### Phase 1: Add Internal Methods to cloud-scheduler-service (ifex-core)
1. Add internal methods to IFEX spec (get_jobs_for_vehicle, upsert_job, etc.)
2. Run generate_proto.sh
3. Implement methods in CloudSchedulerService (in-memory)
4. Unit tests for internal methods
5. Verify existing dashboard API still works

### Phase 2: Create cloud-scheduler-sync-bridge (ifex-core)
1. Create IFEX spec for bridge monitoring API
2. Implement sync bridge with all protocol logic:
   - V2C/C2V message parsing (uses existing proto/internal/scheduler-sync-v2.proto)
   - Version vector comparison (uses libs/scheduler/sync_engine.hpp)
   - Checksum computation (uses libs/scheduler/checksum.hpp)
   - Quiescence detection
3. Integration tests with in-memory scheduler + MQTT transport
4. Verify protocol correctness matches current scheduler_mirror behavior

### Phase 3: Add Internal Methods to offboard-services
1. Implement same internal methods in scheduler_service_impl.cpp (PostgreSQL)
   - get_jobs_for_vehicle → SQL query
   - upsert_job → UPSERT with version fields
   - record_execution → INSERT (idempotent)
   - get/update_vehicle_sync_state → sync_state table
2. Deploy cloud-scheduler-sync-bridge (same binary from ifex-core)
3. Run in parallel with scheduler_mirror for validation
4. Compare sync behavior between old and new implementations
5. E2E tests with production-like data

### Phase 4: Deprecate scheduler_mirror
1. Route all sync traffic through new sync bridge
2. Remove scheduler_mirror from deployment
3. Remove scheduler_mirror code
4. Update documentation

## Benefits

1. **Single Protocol Implementation**: Sync logic in one place
2. **Test What You Ship**: Same sync bridge binary everywhere
3. **Clean Interfaces**: IFEX gRPC is the contract
4. **Flexible Deployment**: Mix any scheduler impl with any transport impl
5. **Easier Debugging**: Can test sync logic with simple in-memory backend

## Design Decisions

1. **Scheduler = Storage + Checksum, Bridge = Protocol**: The scheduler stores jobs with
   version vector fields and maintains the cloud_checksum (recomputed on every job change).
   The sync bridge owns protocol logic: message parsing, version comparison, quiescence detection.
   Scheduler does NOT know about V2C/C2V message formats.

2. **No Abstract Storage Interface**: IFEX IS the interface. We implement multiple versions
   of cloud-scheduler-service (in-memory, PostgreSQL) that all expose the same gRPC interface.
   No need for C++ abstract classes - the gRPC interface is the abstraction.

3. **Reuse Existing Protocol Types**: V2C_SyncMessage and C2V_SyncMessage are already defined
   in `proto/internal/scheduler-sync-v2.proto`. The sync bridge uses these directly.

4. **Reuse Existing Sync Logic**: Both scheduler and sync bridge link against `libs/scheduler/`:
   - Scheduler uses `checksum.hpp` to maintain cloud_checksum on job changes
   - Bridge uses `sync_engine.hpp` for version vector comparison
   - Bridge uses `checksum.hpp` to verify V2C checksums if needed

## Open Questions

1. **Conflict Resolution in Bridge**: The bridge decides conflicts using version vectors and
   authority. Should this be configurable per-deployment, or hardcoded?

2. **Error Recovery**: How to handle partial failures (scheduler update OK, transport send fail)?
   Options: retry queue, transaction-like semantics, or best-effort with logging.

3. **Metrics**: Where should sync metrics be exposed?
   - Bridge metrics: messages processed, latency, errors
   - Scheduler metrics: jobs per vehicle, sync state
   - Both exposed via bridge's gRPC health API?

4. **Dashboard Notifications**: When sync bridge updates a job via upsert_job, should the
   scheduler notify the dashboard (e.g., via Kafka)? Or is polling sufficient?

5. **Tombstone Cleanup**: The scheduler stores tombstones (deleted=true). Who runs cleanup?
   - Scheduler: background job that purges tombstones older than N days
   - Or external cron job?
