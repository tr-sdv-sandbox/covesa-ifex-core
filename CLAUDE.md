# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Dynamic vehicle orchestration platform using COVESA IFEX. The core idea: **separate capabilities from coordination**. Services do one thing well and know nothing about each other. Workflows combine them declaratively. The Scheduler decouples *when* from *what*, enabling function composition without direct dependencies.

## Build Commands

```bash
# First time setup
./install_deps.sh
./generate_proto.sh

# Build (from project root)
./build.sh                    # Release build
./build.sh --debug            # Debug build
./build.sh --clean            # Clean rebuild
./build.sh --debug --test     # Debug build + run tests

# Manual build alternative
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Debug ..
make -j$(nproc)

# Run tests (from build directory)
ctest --output-on-failure                                        # All tests
./tests/ifex-unit-tests --gtest_filter=ParserTest.BasicParsing   # Single unit test
./tests/ifex-tests-integration --gtest_filter=DiscoveryTest.*    # Single integration test
```

## Running Services

```bash
# All services (recommended)
./start-all-bg.sh
./stop_services.sh

# Manual (must be in order - Discovery first)
./build/reference-services/discovery/ifex-discovery-service --listen=0.0.0.0:50051
./build/reference-services/dispatcher/ifex-dispatcher-service --listen=0.0.0.0:50052 --discovery=localhost:50051
./build/reference-services/scheduler/ifex-scheduler-service --listen=0.0.0.0:50053 --discovery=localhost:50051

# Scheduler with persistence (jobs survive restarts)
./build/reference-services/scheduler/ifex-scheduler-service --listen=0.0.0.0:50053 --discovery=localhost:50051 --persistence-dir=/var/lib/ifex/scheduler
```

Integration tests spawn their own service instances (via `test_fixture.cpp`) - no need to start services manually before running tests.

## Architecture

### Service Flow

```
Scheduler → Orchestrator → Dispatcher → Discovery → Target Service
 (when)       (what)         (how)       (where)
```

| Service | Port | Purpose |
|---------|------|---------|
| Discovery | 50051 | Service registry with IFEX schema |
| Dispatcher | 50052 | Dynamic routing, JSON↔Protobuf translation |
| Scheduler | 50053 | Time/event triggers, job persistence |
| Backend Transport | 50060 | Vehicle-to-cloud (MQTT) |
| Beverage (test) | 50061 | In-vehicle beverage prep |
| Climate Comfort (test) | 50062 | Cabin comfort control |
| Defrost (test) | 50063 | Windshield defrost |
| Echo (test) | 50097* | Simple echo for integration tests |
| Test Types (test) | 50095* | Type validation tests |

*Integration tests use ports 50094-50099 to avoid conflicts with manually started services.

### Key Concepts

- **VSS** = vehicle *state* (signals, sensors, actuators via KUKSA.val)
- **IFEX** = vehicle *capabilities* (services, methods)

IFEX services consume VSS internally but expose semantic interfaces externally.

### IFEX Schema → Proto Generation

Services are defined in YAML (`*.ifex.yml`):
- `reference-specs/vehicle/` - Vehicle service schemas
- `reference-specs/cloud/` - Cloud service schemas
- `reference-specs/common/` - Shared types (scheduler-types, etc.)
- `test-services/<service>/` - Test service schemas

Run `./generate_proto.sh` to regenerate from IFEX YAML. The script generates:
1. **C++ headers** (`*.ifex.h`) - Flattened IFEX as embedded strings for service registration
2. **Proto files** (`*.proto`) - For gRPC code generation

Services use embedded schemas - no runtime file loading:
```cpp
#include "scheduler-service.ifex.h"
discovery_client.register_service(ifex::schema::scheduler_service, port);
```

### Proto Directory Structure

```
proto/
├── ifex-generated/   # Generated from IFEX YAML (DO NOT EDIT)
│   ├── vehicle/      # Vehicle service protos + *.ifex.h headers
│   ├── cloud/        # Cloud service protos + *.ifex.h headers
│   ├── common/       # Shared types (scheduler-types.proto)
│   └── test-services/# Test service protos + *.ifex.h headers
├── internal/         # Hand-written internal protocols (scheduler-sync-v2.proto)
└── api/              # Hand-written external APIs
```

- **ifex-generated/**: Auto-generated - both `.proto` and `.ifex.h` files
- **internal/**: Wire protocols between internal components (not gRPC services)
- **api/**: External gRPC APIs not expressible in IFEX

### Core Library (`core/`)

The shared library (`ifex-core`) provides:
- `types.hpp` - Core type definitions (ServiceInfo, ServiceEndpoint, MethodSignature, etc.)
- `parser.hpp` - IFEX YAML parsing
- `discovery.hpp` - gRPC client for Discovery service

The `core/sync/` sublibrary (`ifex-sync`, header-only) provides:
- `version_vector.hpp` - Two-component version vectors for sync protocol v2
- `sync_engine.hpp` - Conflict resolution logic (dominance, authority-based merge)

Services link against `ifex-core` and `ifex-proto-generated`.

### Adding a New Service

1. Create IFEX schema in `test-services/<name>/<name>.ifex.yml`
2. Run `./generate_proto.sh` to generate proto
3. Implement gRPC server using generated stubs
4. Register with Discovery at startup using `DiscoveryClient::register_service()`
5. Add CMakeLists.txt linking `ifex-core` and `ifex-proto-generated`

### Backend Transport Service

The `reference-services/backend-transport/` provides vehicle-to-cloud communication:
- gRPC interface for IFEX services to publish/subscribe
- Single MQTT connection shared across all clients
- Per-content-id message queues with ordering guarantees
- Persistence levels: BEST_EFFORT, VOLATILE, DURABLE

See `reference-services/backend-transport/README.md` for full API documentation.

### Vehicle Online/Offline Status

Backend Transport publishes vehicle connection status to the cloud using MQTT LWT (Last Will and Testament).

#### Architecture (Vehicle Side)

```
┌─────────────────────────────────────────────────────────────────────────────┐
│ Backend Transport Service                                                   │
│                                                                             │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │ backend_transport_server.cpp                                         │   │
│  │                                                                      │   │
│  │  Config:                                                             │   │
│  │    mqtt_config.status_topic = "v2c/" + vehicle_id + "/is_online"    │   │
│  └──────────────────────────────┬──────────────────────────────────────┘   │
│                                 │                                           │
│  ┌──────────────────────────────▼──────────────────────────────────────┐   │
│  │ mqtt_client.cpp                                                      │   │
│  │                                                                      │   │
│  │  MqttClient::Connect():                                             │   │
│  │    ┌────────────────────────────────────────────────────────────┐   │   │
│  │    │ 1. mosquitto_will_set(status_topic, "0", QoS=1, retain)    │   │   │
│  │    │    └─▶ LWT: broker publishes "0" on unexpected disconnect  │   │   │
│  │    └────────────────────────────────────────────────────────────┘   │   │
│  │    ┌────────────────────────────────────────────────────────────┐   │   │
│  │    │ 2. mosquitto_connect(host, port)                           │   │   │
│  │    └────────────────────────────────────────────────────────────┘   │   │
│  │                                                                      │   │
│  │  MqttClient::HandleConnect():                                       │   │
│  │    ┌────────────────────────────────────────────────────────────┐   │   │
│  │    │ 3. mosquitto_publish(status_topic, "1", QoS=1, retain)     │   │   │
│  │    │    └─▶ Immediately marks vehicle as online                 │   │   │
│  │    └────────────────────────────────────────────────────────────┘   │   │
│  │                                                                      │   │
│  │  MqttClient::Disconnect():                                          │   │
│  │    ┌────────────────────────────────────────────────────────────┐   │   │
│  │    │ 4. mosquitto_publish(status_topic, "0", QoS=1, retain)     │   │   │
│  │    │    └─▶ Marks vehicle as offline before graceful disconnect │   │   │
│  │    └────────────────────────────────────────────────────────────┘   │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────────────┘
                                  │
                                  │ MQTT (QoS 1, retained)
                                  ▼
                         ┌─────────────────┐
                         │ Mosquitto Broker │ ──▶ Cloud (mqtt_kafka_bridge)
                         └─────────────────┘
```

#### Code Path

| Step | File | Line | Code |
|------|------|------|------|
| Config topic | `backend_transport_server.cpp` | ~43 | `status_topic = "v2c/" + vehicle_id + "/is_online"` |
| Set LWT | `mqtt_client.cpp` | ~82 | `mosquitto_will_set(mosq_, topic, 1, "0", 1, true)` |
| Publish online | `mqtt_client.cpp` | ~291 | `mosquitto_publish(mosq_, nullptr, topic, 1, "1", 1, true)` |
| Publish offline | `mqtt_client.cpp` | ~131 | `mosquitto_publish(mosq_, nullptr, topic, 1, "0", 1, true)` |

#### MQTT Message

| Property | Value |
|----------|-------|
| Topic | `v2c/{vehicle_id}/is_online` |
| Payload | `1` (online) or `0` (offline) |
| QoS | 1 (at least once) |
| Retain | true |

#### Testing

```bash
# Integration tests
./build/reference-services/backend-transport/ifex-backend-transport-integration-test \
    --gtest_filter="IsOnlineStatusTest.*"
```

The cloud side (`covesa-ifex-offboard-services/mqtt_kafka_bridge`) subscribes to `v2c/#`, detects the `/is_online` suffix, and updates PostgreSQL.

### Discovery Sync Protocol

Backend Transport synchronizes the vehicle's service registry to the cloud using a hash-based protocol that minimizes bandwidth.

```
VEHICLE                                         CLOUD
   │                                              │
   ├──── v2c/{vid}/201: [hash1, hash2, hash3] ───▶  (service hashes)
   │                                              │
   │                               Known hashes?──┤
   │                               hash1 ✓        │
   │                               hash2 ✓        │
   │                               hash3 ✗        │
   │                                              │
   ◀──── c2v/{vid}/201: [hash3] ──────────────────┤  (request unknown)
   │                                              │
   ├──── v2c/{vid}/201: {hash3: "yaml..."} ───────▶  (send schema)
   │                                              │
```

**Key concepts:**
- **Hash as identity**: SHA-256 of IFEX YAML uniquely identifies schema
- **Transfer on demand**: Full schema only sent when cloud doesn't have it
- **Steady state**: Vehicle reconnect sends ~100 bytes (just hashes)
- **Fleet deduplication**: 100K vehicles with same software → one schema stored

See `docs/discovery-sync-protocol.md` for full protocol specification.

### Scheduler Sync Protocol v2

The Scheduler Sync Bridge (v2) uses **version vectors** for bidirectional state synchronization.

**Vehicle-to-Cloud (v2c):**
```
Scheduler → SchedulerSyncBridge → Backend Transport → MQTT → Cloud
              (poll 1s, checksum-based change detection)
```

**Cloud-to-Vehicle (c2v):**
```
Cloud API → Kafka → MQTT → Backend Transport → SchedulerSyncBridge → Scheduler
```

**Key protocol features:**
- **Version vectors:** `{cloud_seq, vehicle_seq}` - no wall-clock dependency
- **Authority-based resolution:** Cloud or vehicle wins based on job origin
- **Tombstone deletion:** Soft deletes with 7-day retention, confirmed via echo
- **Checksum quiescence:** No traffic when both sides agree (xxHash64)
- **Append-only executions:** Execution records are immutable facts

**Job identity:** `<source>-<uuid>` (e.g., `cloud-abc123`, `veh-WDB123-def456`)

**Key differences from Discovery:**
| Aspect | Discovery | Scheduler v2 |
|--------|-----------|--------------|
| Data | Static schemas | Dynamic jobs |
| Sharing | Fleet-wide dedup | Per-vehicle unique |
| Direction | Mostly v2c | Bidirectional |
| Sync | Hash-first, pull | Version vectors |
| Conflict | N/A (immutable) | Authority wins |

See `docs/scheduler-sync-protocol-v2.md` for full protocol specification.

### RPC Protocol

The Dispatcher RPC Bridge enables cloud-initiated method calls on vehicle services.

**Request Flow:**
```
Cloud API → Kafka → MQTT → Backend Transport → DispatcherRpcBridge → Dispatcher → Service
                                                                         ↓
Cloud API ← Kafka ← MQTT ← Backend Transport ← DispatcherRpcBridge ← Response
```

**Message types:**
- `RPC_REQUEST`: Cloud → Vehicle (method invocation)
- `RPC_RESPONSE`: Vehicle → Cloud (success result)
- `RPC_ERROR`: Vehicle → Cloud (failure)

**Key features:**
- Synchronous request-response semantics over async MQTT
- Request correlation via `request_id`
- Timeout handling (default 30s)
- JSON parameters converted to protobuf by Dispatcher

See `docs/rpc-protocol.md` for full protocol specification.

### Scheduler Persistence

The Scheduler supports optional job persistence via `--persistence-dir`:
- Jobs are saved to JSON immediately on create/update/delete (not just at shutdown)
- Jobs are restored automatically when the scheduler restarts
- Without `--persistence-dir`, jobs are in-memory only

```bash
# Enable persistence
./ifex-scheduler-service --discovery localhost:50051 --persistence-dir /var/lib/ifex/scheduler

# Or via environment variable
SCHEDULER_PERSISTENCE_DIR=/var/lib/ifex/scheduler ./ifex-scheduler-service --discovery localhost:50051
```

## Code Conventions

- **C++ Standard:** C++17
- **Logging:** glog (`LOG(INFO)`, `LOG(ERROR)`, `VLOG(1)`)
- **CLI flags:** gflags for command-line parsing
- **Config/Schema files:** YAML via yaml-cpp, JSON via nlohmann_json
- **Proto generation:** IFEX YAML → .proto via Docker-based ifexgen tool
- **Timestamps:** All timestamp fields use **milliseconds** with `_ms` suffix (e.g., `timestamp_ms`, `last_heartbeat_ms`)

### Timestamp Convention

All timestamp fields across the codebase use **milliseconds since Unix epoch**:

| Field Pattern | Unit | Example |
|---------------|------|---------|
| `*_ms` | milliseconds | `timestamp_ms`, `last_sync_timestamp_ms` |
| `*_timestamp_ms` | milliseconds | `last_heartbeat_ms`, `executed_at_ms` |

**Do not use nanoseconds.** The `_ns` suffix is reserved for internal conversions only.

### Version Vector Convention (Scheduler Sync v2)

Jobs use two-component version vectors for conflict detection:

```cpp
struct VersionVector {
    uint64_t cloud_seq;    // Incremented by cloud on any change
    uint64_t vehicle_seq;  // Incremented by vehicle on any change
};
```

**Comparison rules:**
- A dominates B: `A.cloud >= B.cloud && A.vehicle >= B.vehicle && (A.cloud > B.cloud || A.vehicle > B.vehicle)`
- Neither dominates = **conflict** → resolve by job's `authority` field
- Merged version: `{max(cloud_seq), max(vehicle_seq)}`

### Schema Hash Validation

Service schemas are identified by SHA-256 hashes (64-character lowercase hex strings):

```cpp
// Valid hash: 64 hex characters
"72ec7e49f668c318c766c492969d4448648bc303cf83288f0cfb32e89b81caed"

// Invalid: too short, wrong characters, etc.
"hash1"  // Placeholder - will be rejected
```

The discovery sync bridge validates hashes before sending to cloud:
- Hashes must be exactly 64 characters
- Only hex characters (0-9, a-f, A-F) allowed
- Invalid hashes are logged as warnings and skipped

### Cloud Reference Services (`cloud/`)

Test-only cloud-side implementations (NOT for production):

| Service | Description |
|---------|-------------|
| `cloud-backend-transport/` | Cloud counterpart to vehicle Backend Transport (MQTT) |
| `cloud-scheduler-service/` | Fleet scheduler API (job CRUD, sync processing) |

Cloud services are **only built when tests are enabled** (`./build.sh --debug --test`).
They are NOT built for cross-compilation targets.

For production cloud deployments, see `covesa-ifex-offboard-services`.

## Key Files

- `generate_proto.sh` - Converts IFEX YAML → .proto files (requires ifex-tools Docker image)
- `start-all-bg.sh` / `stop_services.sh` - Service lifecycle management
- `tests/integration/test_fixture.cpp` - Test fixture managing service lifecycle (spawns/kills services automatically)

## Testing

### Running Tests

```bash
# All tests (from build directory)
ctest --output-on-failure

# Parallel execution (MQTT tests are serialized via RESOURCE_LOCK)
ctest --output-on-failure -j4

# Specific test suite
ctest -R "discovery_sync" --output-on-failure

# Single test binary
./reference-services/backend-transport/ifex-backend-transport-integration-test
```

### Test Categories

| Test | Label | Description |
|------|-------|-------------|
| `ifex-sync-tests` | unit | Version vector operations |
| `backend_transport_conformance_test` | conformance | API contract verification |
| `backend_transport_integration_test` | integration, mqtt | End-to-end with MQTT broker |
| `backend_transport_resilience_test` | resilience, mqtt | Broker disconnect/reconnect |
| `discovery_sync_bridge_integration_test` | integration, mqtt | Hash-based sync protocol |
| `dispatcher_bridge_integration_test` | integration, mqtt | RPC request/response |
| `scheduler_sync_bridge_test` | integration, mqtt | v2 sync protocol (version vectors, tombstones) |

### MQTT Test Notes

MQTT-based tests use Docker containers and share a broker resource:

- All MQTT tests have `RESOURCE_LOCK mqtt_broker` in CMakeLists.txt
- This prevents parallel execution conflicts when running `ctest -j`
- Tests automatically start/stop `eclipse-mosquitto:2` container on port 11883
- Container name: `ifex-mqtt-test-broker`

If tests fail with "Connection refused" or container conflicts:
```bash
# Clean up stale containers
docker rm -f ifex-mqtt-test-broker 2>/dev/null

# Check for stale processes
pgrep -af ifex-discovery
```
