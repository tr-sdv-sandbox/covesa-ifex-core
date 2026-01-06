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
- `reference-services/ifex/` - Core infrastructure service schemas
- `test-services/<service>/` - Domain service schemas

Run `./generate_proto.sh` to regenerate proto files from IFEX YAML. Requires `ifex-tools` Docker image (installed via `install_deps.sh`). The script generates `.proto` files in `proto/`; CMake then compiles these to C++ during build.

### Core Library (`core/`)

The shared library (`ifex-core`) provides:
- `types.hpp` - Core type definitions (ServiceInfo, ServiceEndpoint, MethodSignature, etc.)
- `parser.hpp` - IFEX YAML parsing
- `discovery.hpp` - gRPC client for Discovery service

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

### Scheduler Sync Protocol

The Scheduler Sync Bridge synchronizes job state to cloud and receives commands.

**Vehicle-to-Cloud (v2c):**
```
Scheduler → SchedulerSyncBridge → Backend Transport → MQTT → Cloud
              (poll 1s, hash-based change detection)
```

Event types: `FULL_SYNC`, `JOB_CREATED`, `JOB_UPDATED`, `JOB_DELETED`, `JOB_EXECUTED`, `HEARTBEAT`

**Cloud-to-Vehicle (c2v):**
```
Cloud API → Kafka → MQTT → Backend Transport → SchedulerSyncBridge → Scheduler
                                                 (command execution + ACK)
```

Command types: `CREATE_JOB`, `UPDATE_JOB`, `DELETE_JOB`, `PAUSE_JOB`, `RESUME_JOB`, `TRIGGER_JOB`

**Key differences from Discovery:**
| Aspect | Discovery | Scheduler |
|--------|-----------|-----------|
| Data | Static schemas | Dynamic jobs |
| Sharing | Fleet-wide dedup | Per-vehicle unique |
| Direction | Mostly v2c | Bidirectional |
| Sync | Hash-first, pull | Delta events |

See `docs/scheduler-sync-protocol.md` for full protocol specification.

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

## Key Files

- `generate_proto.sh` - Converts IFEX YAML → .proto files (requires ifex-tools Docker image)
- `start-all-bg.sh` / `stop_services.sh` - Service lifecycle management
- `tests/integration/test_fixture.cpp` - Test fixture managing service lifecycle (spawns/kills services automatically)
