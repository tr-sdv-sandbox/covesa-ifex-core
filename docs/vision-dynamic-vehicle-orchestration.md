# Dynamic Vehicle Orchestration Platform

## Vision: Composable Automotive Services

**From hardcoded integrations to declarative workflows**

---

## The Problem

### Today's Reality

Modern vehicles contain 50-150 ECUs running specialized software. Each feature requiring cross-system coordination demands custom integration:

```
┌──────────────────────────────────────────────────────────────┐
│                    "Pre-departure Comfort"                    │
│                                                              │
│   Climate ←──────→ Defrost ←──────→ Seat Heating            │
│      ↑                ↑                   ↑                  │
│      └────────────────┼───────────────────┘                  │
│                       │                                      │
│            Custom Integration Code                           │
│         (hardcoded, OEM-specific, fragile)                   │
└──────────────────────────────────────────────────────────────┘
```

**Pain Points:**
- Every new feature requires months of integration work
- Changes ripple across codebases
- Testing combinatorial explosion
- OEM-specific implementations can't be reused
- AI/voice assistants need custom backends per feature

---

## The Solution

### Service-Oriented Vehicle Architecture

Decompose vehicle capabilities into **self-describing services** that can be discovered, composed, and orchestrated at runtime.

```
┌─────────────────────────────────────────────────────────────────┐
│                     Orchestration Layer                          │
│  ┌───────────┐    ┌────────────┐    ┌───────────┐               │
│  │ Scheduler │───▶│Orchestrator│───▶│ Dispatcher│               │
│  │  (when)   │    │   (what)   │    │   (how)   │               │
│  └───────────┘    └────────────┘    └─────┬─────┘               │
│                                           │                      │
│                        ┌──────────────────┼──────────────────┐  │
│                        ▼                  ▼                  ▼  │
│                   ┌─────────┐        ┌─────────┐       ┌─────────┐
│                   │ Climate │        │ Defrost │       │Beverage │
│                   │ Service │        │ Service │       │ Service │
│                   └─────────┘        └─────────┘       └─────────┘
│                                                                  │
│                    Services know nothing about each other        │
└─────────────────────────────────────────────────────────────────┘
```

**Services are:**
- Self-describing (IFEX schema)
- Discoverable at runtime
- Composable without code changes
- Testable in isolation

---

## Core Architecture

### Four Infrastructure Services

| Service | Question It Answers | Responsibility |
|---------|--------------------|-----------------------|
| **Discovery** | "Where is it?" | Service registry, health monitoring, capability catalog |
| **Dispatcher** | "How do I call it?" | Dynamic method routing, parameter validation, JSON↔Protobuf |
| **Orchestrator** | "What's the workflow?" | Multi-step coordination, conditional logic, error handling |
| **Scheduler** | "When should it run?" | Time-based triggers, event triggers, recurring patterns |

### Information Flow

```
                                    ┌─────────────────┐
                                    │    Discovery    │
                                    │    Registry     │
                                    └────────┬────────┘
                                             │
              ┌──────────────────────────────┼──────────────────────────────┐
              │                              │                              │
              ▼                              ▼                              ▼
    ┌──────────────────┐          ┌──────────────────┐          ┌──────────────────┐
    │  Climate Service │          │  Defrost Service │          │ Beverage Service │
    │                  │          │                  │          │                  │
    │ • Registers at   │          │ • Registers at   │          │ • Registers at   │
    │   startup        │          │   startup        │          │   startup        │
    │ • Sends heartbeat│          │ • Sends heartbeat│          │ • Sends heartbeat│
    │ • Exposes schema │          │ • Exposes schema │          │ • Exposes schema │
    └──────────────────┘          └──────────────────┘          └──────────────────┘
```

**At runtime:**

```
   User Request                     "Prepare for my 7am departure"
        │
        ▼
   ┌─────────────┐
   │  Scheduler  │                  Creates job for 6:30am
   └──────┬──────┘
          │ triggers
          ▼
   ┌─────────────┐
   │Orchestrator │                  Executes workflow definition
   └──────┬──────┘
          │ step 1: check weather
          │ step 2: defrost if needed
          │ step 3: start climate
          │ step 4: prepare beverage
          ▼
   ┌─────────────┐
   │ Dispatcher  │                  Routes each call to correct service
   └──────┬──────┘
          │
    ┌─────┴─────┬─────────────┐
    ▼           ▼             ▼
 Climate    Defrost      Beverage
```

---

## The Power: Declarative Workflows

### Workflows as Data, Not Code

Instead of hardcoding orchestration logic, define workflows declaratively:

```yaml
name: morning_departure_prep
description: Prepare vehicle for comfortable morning departure

trigger:
  type: scheduled
  time: "{{departure_time - 30min}}"

steps:
  - id: check_conditions
    service: weather-service
    method: get_current_conditions

  - id: defrost_if_needed
    service: defrost-service
    method: start_defrost
    when: "{{steps.check_conditions.result.temperature < 3}}"

  - id: climate_comfort
    service: climate-service
    method: set_cabin_temperature
    parameters:
      target: "{{user_preferences.comfort_temperature}}"

  - id: morning_beverage
    service: beverage-service
    method: prepare_beverage
    parameters:
      type: "{{user_preferences.morning_drink}}"
    delay: "{{departure_time - 5min}}"
```

**Benefits:**
- No code changes to add/modify workflows
- User-customizable through UI
- AI can generate workflows from natural language
- Version controlled, auditable
- Testable with mock services

---

## Key Capabilities

### 1. Runtime Service Discovery

Services announce themselves with full capability descriptions:

```yaml
service_info:
  name: climate-comfort-service
  version: 1.2.0
  endpoint: 192.168.1.50:50100

  methods:
    - name: set_cabin_temperature
      description: Set target cabin temperature
      parameters:
        - name: target_celsius
          type: float
          min: 16.0
          max: 28.0
        - name: zone
          type: enum
          values: [all, front, rear, driver]
      returns:
        - name: accepted
          type: boolean
        - name: estimated_time_seconds
          type: uint32
```

**Clients don't need compile-time knowledge** - they discover capabilities at runtime.

### 2. Dynamic Method Dispatch

Call any service method with JSON, no protobuf required:

```
POST /dispatcher/call
{
  "service": "climate-comfort-service",
  "method": "set_cabin_temperature",
  "parameters": {
    "target_celsius": 22.0,
    "zone": "all"
  }
}
```

**Response:**
```json
{
  "status": "SUCCESS",
  "response": {
    "accepted": true,
    "estimated_time_seconds": 180
  },
  "duration_ms": 45
}
```

### 3. Workflow Orchestration

Execute multi-step workflows with:
- **Sequential execution** - Step by step
- **Parallel execution** - Independent steps run concurrently
- **Conditional branching** - Execute steps based on conditions
- **Error handling** - Compensating actions on failure
- **Result passing** - Output of step N available to step N+1

### 4. Time & Event Scheduling

Trigger workflows from:
- **Calendar schedules** - "Every weekday at 6:30am"
- **Cron expressions** - "0 30 6 * * MON-FRI"
- **Vehicle signals** - "When door opens"
- **API calls** - On-demand execution
- **Other workflows** - Workflow chaining

---

## Example: Intelligent Departure

### User Story

> "I want my car ready when I leave for work at 7am. Warm cabin,
> defrosted windows if it's cold, and my coffee ready."

### Traditional Approach

Custom firmware with hardcoded logic:
- Climate team implements departure feature
- Integrates with defrost module
- Coffee machine vendor provides SDK
- 6+ months development
- Works only for this specific configuration

### Dynamic Orchestration Approach

1. **Define workflow** (30 minutes):

```yaml
name: smart_departure
trigger:
  schedule: "0 30 6 * * MON-FRI"

steps:
  - id: weather
    service: weather
    method: get_forecast

  - id: defrost
    service: defrost
    method: auto_defrost
    when: "{{steps.weather.result.temp_celsius < 5}}"
    on_failure:
      notify: "Defrost unavailable - check washer fluid"

  - id: climate
    service: climate-comfort
    method: set_cabin_temperature
    parallel_with: defrost
    parameters:
      target: 22

  - id: coffee
    service: beverage
    method: prepare
    delay_until: "06:55"
    parameters:
      type: espresso
      strength: medium
```

2. **Deploy** - Upload YAML to orchestrator
3. **Done** - No code changes, no recompilation

### Adding a New Capability

User gets an electric vehicle. Add battery preconditioning:

```yaml
  - id: battery_prep
    service: battery-management
    method: precondition
    when: "{{vehicle.powertrain == 'EV'}}"
    parallel_with: climate
```

**One line added to workflow.** No integration work.

---

## Architectural Principles

### 1. Services Are Independent

Each service:
- Has single responsibility
- Knows nothing about other services
- Exposes capability via IFEX schema
- Handles its own error recovery

### 2. Orchestration Is External

- Services don't orchestrate
- Workflows are data, not code
- Orchestrator executes workflow definitions
- Changes don't require service updates

### 3. Discovery Enables Flexibility

- No hardcoded addresses
- Services can move, scale, restart
- New services immediately available
- Health monitoring built-in

### 4. Schema Is the Contract

- IFEX defines service interfaces
- Runtime validation prevents errors
- Documentation auto-generated
- Backwards compatibility verifiable

---

## Safety Architecture

### Non-Safety-Critical Layer

The orchestration platform runs in a sandboxed environment:

```
┌─────────────────────────────────────────────────────────────┐
│                    Application Layer                         │
│              (Orchestration, AI, User Apps)                  │
│                                                              │
│                    Can fail safely                           │
└──────────────────────────┬──────────────────────────────────┘
                           │
                      VSS Signals
                           │
┌──────────────────────────┴──────────────────────────────────┐
│                    Vehicle Layer                             │
│              (ASIL-rated controllers)                        │
│                                                              │
│            Has final authority over all actions              │
└─────────────────────────────────────────────────────────────┘
```

**Key Safety Properties:**
- Orchestration layer cannot bypass vehicle safety
- All commands are *requests*, controllers decide
- Fail-safe: platform crash = vehicle still works
- No direct hardware access

---

## Integration Points

### AI/LLM Integration

```
User: "I'm leaving in 20 minutes, make the car comfortable"

LLM → Dispatcher:
  call("climate-comfort", "set_cabin_temperature", {"target": 22})
  call("defrost", "quick_defrost", {})
```

The LLM discovers available services and calls them directly through Dispatcher. No custom backend required.

### Voice Assistants

```
"Hey vehicle, schedule my morning routine"

Voice → Scheduler:
  create_workflow("morning_departure", trigger="06:30")
```

### Mobile Apps

Apps interact via REST/gRPC with the same services. No separate backend.

### Cloud Services

Fleet management can query Discovery across vehicles, trigger workflows remotely.

---

## Deployment Model

### Vehicle-Local

```
┌─────────────────────────────────────┐
│           Vehicle ECU               │
│  ┌─────────┐  ┌─────────┐          │
│  │Discovery│  │Scheduler│          │
│  └─────────┘  └─────────┘          │
│  ┌─────────┐  ┌──────────┐         │
│  │Dispatcher│  │Orchestrator│       │
│  └─────────┘  └──────────┘         │
│                                     │
│  ┌─────────────────────────────┐   │
│  │     Domain Services         │   │
│  │  Climate, Defrost, etc.     │   │
│  └─────────────────────────────┘   │
└─────────────────────────────────────┘
```

All services run locally. Works offline.

### Hybrid (Edge + Cloud)

```
┌──────────────────────┐        ┌──────────────────────┐
│       Vehicle        │        │        Cloud         │
│  ┌──────────────┐   │        │  ┌──────────────┐   │
│  │ Local        │   │◄──────►│  │ Fleet        │   │
│  │ Discovery    │   │        │  │ Management   │   │
│  └──────────────┘   │        │  └──────────────┘   │
│  ┌──────────────┐   │        │  ┌──────────────┐   │
│  │ Domain       │   │        │  │ Analytics    │   │
│  │ Services     │   │        │  │ ML Models    │   │
│  └──────────────┘   │        │  └──────────────┘   │
└──────────────────────┘        └──────────────────────┘
```

Cloud services can be discovered and called through the same mechanism.

---

## Benefits Summary

| Stakeholder | Benefit |
|------------|---------|
| **OEMs** | Faster feature development, reusable components |
| **Tier 1 Suppliers** | Standard interface, broader market |
| **Developers** | Clear contracts, independent testing |
| **End Users** | Customizable features, better integration |
| **AI/Voice** | Direct service access, no custom backends |

### Quantified Impact

| Metric | Traditional | With Platform |
|--------|-------------|---------------|
| New feature integration | 3-6 months | 1-2 weeks |
| Cross-system test cases | O(n²) | O(n) |
| OEM customization effort | Full fork | Config change |
| AI integration | Custom per feature | Zero additional |

---

## Technology Foundation

### Standards-Based

- **COVESA IFEX** - Interface specification
- **VSS** - Vehicle signal abstraction
- **gRPC/Protobuf** - Efficient transport
- **JSON** - Universal interchange

### Proven Components

- C++17 core services
- Standard automotive toolchains

---

## Roadmap

### Phase 1: Foundation (Current)
- Discovery, Dispatcher, Scheduler
- Basic service ecosystem
- JSON/gRPC interchange

### Phase 2: Orchestration
- Workflow engine
- Conditional execution
- Error handling & compensation

### Phase 3: Intelligence
- Event-driven triggers
- VSS signal subscriptions
- Predictive scheduling

### Phase 4: Ecosystem
- Workflow marketplace
- Cross-OEM service compatibility
- Cloud integration patterns

---

## Summary

**Dynamic Vehicle Orchestration** transforms automotive software from monolithic, hardcoded systems into a composable service ecosystem.

**Core insight:** Separate the *capabilities* (services) from the *coordination* (workflows). Services do one thing well. Workflows combine them declaratively.

**Result:** Faster development, easier customization, AI-ready architecture, and a foundation for the software-defined vehicle.

---

## Next Steps

1. **Technical Deep Dive** - IFEX schema design, service implementation patterns
2. **Proof of Concept** - Departure orchestration with 3-4 services
3. **Integration Planning** - VSS binding, ECU deployment model
4. **Pilot Program** - Limited deployment with feedback loop

---

## Related Documentation

- [VSS and IFEX Relationship](vss-ifex-relationship.md) - Understanding when to use VSS vs IFEX
- [Core Services Specification](core-services-spec.md) - Technical details of infrastructure services
- [Orchestrator Service Design](orchestrator-service-design.md) - Workflow engine specification
- [IFEX Service Architecture](ifex-service-architecture.md) - How to design and implement services
- [Intelligent Vehicle Orchestration](intelligent-vehicle-orchestration-spec.md) - AI/LLM integration
