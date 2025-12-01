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
mkdir -p build && cd build
cmake ..
make -j$(nproc)

# Run tests (from build directory)
ctest --output-on-failure              # All tests
./tests/ifex-unit-tests --gtest_filter=ParserTest.BasicParsing   # Single unit test
./tests/ifex-tests-integration --gtest_filter=DiscoveryTest.*    # Single integration test
```

## Running Services

```bash
# All services (recommended)
./start-all-bg.sh
./stop_services.sh

# Manual (must be in order - Discovery first)
./build/reference-services/discovery/ifex-discovery-service --port 50051
./build/reference-services/dispatcher/ifex-dispatcher-service --discovery localhost:50051
./build/reference-services/scheduler/ifex-scheduler-service --discovery localhost:50051
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
| Echo (test) | 50053 | Simple echo for testing |
| Settings (test) | 50055 | User preferences and presets |
| Beverage (test) | 50061 | In-vehicle beverage prep |
| Climate Comfort (test) | 50062 | Cabin comfort control |
| Defrost (test) | 50063 | Windshield defrost |

### Key Concepts

- **VSS** = vehicle *state* (signals, sensors, actuators via KUKSA.val)
- **IFEX** = vehicle *capabilities* (services, methods)

IFEX services consume VSS internally but expose semantic interfaces externally.

### IFEX Schema → Proto Generation

Services are defined in YAML (`*.ifex.yml`):
- `reference-services/ifex/` - Core infrastructure service schemas
- `test-services/<service>/` - Domain service schemas

Run `./generate_proto.sh` to regenerate proto files from IFEX YAML. The proto generation happens in CMake during build; the script generates the `.proto` files from IFEX YAML.

### Core Library (`core/`)

The shared library provides:
- `parser.hpp` / `parser_impl.cpp` - IFEX YAML parsing
- `discovery.hpp` / `discovery_client_impl.cpp` - gRPC client for Discovery service
- `types.hpp` - Core type definitions (ServiceInfo, ServiceEndpoint, etc.)

Services link against `ifex-core` and `ifex-proto-generated`.

### Adding a New Service

1. Create IFEX schema in `test-services/<name>/<name>.ifex.yml`
2. Run `./generate_proto.sh` to generate proto
3. Implement gRPC server using generated stubs
4. Register with Discovery at startup using `DiscoveryClient::register_service()`
5. Add CMakeLists.txt linking `ifex-core` and `ifex-proto-generated`

## Key Files

- `generate_proto.sh` - Converts IFEX YAML → .proto files
- `start-all-bg.sh` / `stop_services.sh` - Service lifecycle management
- `tests/integration/test_fixture.cpp` - Test fixture managing service lifecycle
