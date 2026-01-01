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
