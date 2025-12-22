# COVESA IFEX Core

Dynamic vehicle orchestration platform using the COVESA IFEX standard.

## The Problem

Modern vehicles have 50-150 ECUs. Every feature requiring cross-system coordination (climate + defrost + seat heating for "departure prep") demands custom integration code. This means:

- Months of development per feature
- Tight coupling between teams
- OEM-specific implementations that can't be reused
- AI assistants need custom backends for each capability

## The Solution

**Separate capabilities from coordination.**

Services do one thing well and know nothing about each other. Workflows combine them declaratively. The Scheduler decouples *when* from *what*, enabling function composition without direct dependencies.

```
┌─────────────────────────────────────────────────────────────┐
│                    User / AI / Apps                          │
└──────────────────────────┬──────────────────────────────────┘
                           │
┌──────────────────────────┴──────────────────────────────────┐
│                 Orchestration Layer                          │
│   Scheduler ──▶ Orchestrator ──▶ Dispatcher ──▶ Discovery   │
│    (when)         (what)          (how)        (where)      │
└──────────────────────────┬──────────────────────────────────┘
                           │
┌──────────────────────────┴──────────────────────────────────┐
│                   IFEX Services                              │
│         Climate    Defrost    Beverage    Battery           │
└──────────────────────────┬──────────────────────────────────┘
                           │
┌──────────────────────────┴──────────────────────────────────┐
│                  VSS / KUKSA.val                             │
└──────────────────────────┬──────────────────────────────────┘
                           │
┌──────────────────────────┴──────────────────────────────────┐
│               Vehicle Hardware (ASIL-rated)                  │
└─────────────────────────────────────────────────────────────┘
```

## Core Architecture

### Four Infrastructure Services

| Service | Question | Purpose |
|---------|----------|---------|
| **Discovery** | Where is it? | Service registry. Services register at startup with endpoint + IFEX schema |
| **Dispatcher** | How do I call it? | Dynamic routing. Call any service with JSON, no protobuf knowledge needed |
| **Orchestrator** | What's the workflow? | Multi-step coordination. Workflows are YAML definitions, not code |
| **Scheduler** | When should it run? | Time/event triggers. Decouples timing from logic |

### VSS vs IFEX

**VSS** describes vehicle *state* (signals, sensors, actuators).
**IFEX** describes vehicle *capabilities* (services, methods).

```
VSS:   "The cabin temperature IS 18°C"
IFEX:  "Make the cabin comfortable for my morning commute"
```

IFEX services consume VSS internally but expose semantic interfaces externally. You don't ask users to "set Vehicle.Cabin.HVAC.Row1.Left.Temperature.Target to 22" - you offer `set_comfort_level(cozy)`.

### Why Scheduler is Key

The Scheduler breaks the dependency chain:

```
Traditional:  ClimateService calls DefrostService calls BeverageService
              (tight coupling, each must know the others)

With Scheduler:
  Job 1: 6:30am → call defrost.start()
  Job 2: 6:40am → call climate.set_comfort(cozy)
  Job 3: 6:55am → call beverage.prepare(espresso)

  (services know nothing about each other)
```

When extended with workflow chaining (`on_success`, `on_failure`), the Scheduler becomes a lightweight workflow engine without services needing direct dependencies.

## Quick Start

```bash
# Install dependencies
./install_deps.sh

# Generate protobuf from IFEX schemas
./generate_proto.sh

# Build
mkdir -p build && cd build
cmake -DGRPC_CPP_PLUGIN_EXECUTABLE=/usr/bin/grpc_cpp_plugin ..
make -j$(nproc)

# Run all services
cd ..
./start-all-bg.sh
```

### Service Ports

| Service | Port | Description |
|---------|------|-------------|
| Discovery | 50051 | Service registry |
| Dispatcher | 50052 | Dynamic routing |
| Scheduler | 50053 | Time/event triggers |
| Backend Transport | 50060 | Vehicle-to-cloud (MQTT) |

### Run Tests

```bash
cd build
ctest --output-on-failure

# Single test
./tests/ifex-unit-tests --gtest_filter=ParserTest.*
```

## Example: Departure Preparation

**Traditional approach:** 6 months of integration work, hardcoded logic.

**With this platform:**

```yaml
# Workflow definition (data, not code)
name: morning_departure
trigger:
  schedule: "0 30 6 * * MON-FRI"

steps:
  - id: weather
    service: weather
    method: get_conditions

  - id: defrost
    service: defrost
    method: start
    when: "{{steps.weather.result.temperature < 3}}"

  - id: climate
    service: climate
    method: set_comfort_level
    parameters:
      level: cozy

  - id: coffee
    service: beverage
    method: prepare
    delay_until: "06:55"
```

Upload YAML. Done. No code changes, no recompilation.

## Project Structure

```
covesa-ifex-core/
├── core/                    # Shared library (parser, discovery client)
├── reference-services/      # Infrastructure services
│   ├── discovery/           # Service registry (50051)
│   ├── dispatcher/          # Dynamic routing (50052)
│   ├── scheduler/           # Time triggers (50053)
│   ├── backend-transport/   # Cloud connectivity (50060)
│   └── ifex/                # IFEX schema definitions
├── test-services/           # Example domain services
│   ├── climate-comfort/     # Cabin comfort (50062)
│   ├── defrost/             # Window defrost (50063)
│   ├── beverage/            # Beverage prep (50061)
│   └── settings/            # User preferences (50055)
├── proto/                   # Generated protobuf from IFEX
├── tests/                   # Unit and integration tests
└── docs/                    # Detailed documentation
```

## Documentation

| Document | Description |
|----------|-------------|
| [docs/vision-dynamic-vehicle-orchestration.md](docs/vision-dynamic-vehicle-orchestration.md) | Full vision |
| [docs/vss-ifex-relationship.md](docs/vss-ifex-relationship.md) | When to use VSS vs IFEX |
| [docs/core-services-spec.md](docs/core-services-spec.md) | Infrastructure service specification |
| [docs/orchestrator-service-design.md](docs/orchestrator-service-design.md) | Workflow engine design |
| [docs/ifex-service-architecture.md](docs/ifex-service-architecture.md) | How to build services |
| [reference-services/backend-transport/README.md](reference-services/backend-transport/README.md) | Cloud connectivity service |

## Design Principles

1. **Services are independent** - No compile-time dependencies
2. **Orchestration is external** - Workflows are data, not code
3. **Schema is the contract** - IFEX enables runtime validation
4. **JSON interchange** - No protobuf coupling for callers
5. **Non-safety-critical** - Orchestration can fail safely; ASIL controllers have final authority

## Roadmap

| Phase | Status | Features |
|-------|--------|----------|
| Foundation | Current | Discovery, Dispatcher, Scheduler |
| Orchestration | In Progress | Workflow engine, conditional execution |
| Intelligence | Planned | Event triggers, VSS subscriptions |
| Ecosystem | Future | Workflow marketplace, cross-OEM compatibility |

## Standards

- [COVESA IFEX](https://github.com/COVESA/ifex) - Interface Exchange
- [COVESA VSS](https://github.com/COVESA/vehicle_signal_specification) - Vehicle Signal Specification
- [KUKSA.val](https://github.com/eclipse/kuksa.val) - VSS databroker

## License

Licensed under the Apache License, Version 2.0. See [LICENSE](LICENSE) for details.
