# Core Services Specification

## Overview

The IFEX platform consists of four core infrastructure services:

| Service | Question | Responsibility |
|---------|----------|----------------|
| **Discovery** | Where is it? | Service registry, health monitoring, capability catalog |
| **Dispatcher** | How do I call it? | Dynamic method routing, parameter validation, JSON↔Protobuf |
| **Orchestrator** | What's the workflow? | Multi-step coordination, conditional logic, error handling |
| **Scheduler** | When should it run? | Time-based triggers, event triggers, recurring patterns |

These services enable loose coupling between components. Services register themselves at runtime, can be discovered by name, and called without compile-time dependencies.

---

## Service Interaction

```
┌─────────────────────────────────────────────────────────────────┐
│                         Clients                                  │
│              (Apps, AI/LLM, Voice, Mobile)                       │
└──────────────────────────┬──────────────────────────────────────┘
                           │
         ┌─────────────────┼─────────────────┐
         │                 │                 │
         ▼                 ▼                 ▼
┌─────────────┐    ┌─────────────┐    ┌─────────────┐
│  Scheduler  │───▶│Orchestrator │───▶│  Dispatcher │
│   (when)    │    │   (what)    │    │    (how)    │
└─────────────┘    └─────────────┘    └──────┬──────┘
                                             │
                          ┌──────────────────┼──────────────────┐
                          │                  │                  │
                          ▼                  ▼                  ▼
                    ┌──────────┐       ┌──────────┐       ┌──────────┐
                    │ Service  │       │ Service  │       │ Service  │
                    │    A     │       │    B     │       │    C     │
                    └──────────┘       └──────────┘       └──────────┘
                          │                  │                  │
                          └──────────────────┼──────────────────┘
                                             │
                                             ▼
                                      ┌─────────────┐
                                      │  Discovery  │
                                      │  (where)    │
                                      └─────────────┘
                                             ▲
                                             │ register at startup
                                             │
```

**Flow:**
1. All services register with Discovery at startup
2. Clients can call Dispatcher directly for single operations
3. Scheduler triggers Orchestrator for scheduled workflows
4. Orchestrator executes multi-step workflows via Dispatcher
5. Dispatcher resolves services through Discovery and routes calls

---

## Discovery Service

### Purpose

Central registry where services announce their existence and capabilities. Clients query it to find service endpoints and schemas.

### Rationale

Without a registry, clients need hardcoded addresses and compile-time knowledge of services. Discovery eliminates this:
- Services come and go dynamically
- Multiple instances can coexist
- Clients get current endpoint + full API schema at runtime
- Health monitoring via heartbeat detects failures

### API

| Method | Description |
|--------|-------------|
| `register_service` | Service announces itself with endpoint and IFEX schema |
| `unregister_service` | Service removes itself from registry |
| `get_service` | Lookup single service by name |
| `query_services` | Find services matching filter (name pattern, method, transport) |
| `heartbeat` | Service reports it's still alive |
| `validate_method_call` | Check if a method call would be valid |
| `get_method_schema` | Get parameter schema for a method |

### Data Structures

```yaml
service_info_t:
  name: string                    # Service name
  version: string                 # Major.minor version
  description: string
  status: AVAILABLE | STARTING | STOPPING | ERROR | UNAVAILABLE
  endpoint:
    transport: GRPC | DBUS | SOMEIP | HTTP_REST | MQTT
    address: string               # host:port or equivalent
  namespaces:                     # From IFEX schema
    - name: string
      methods:
        - name: string
          input_parameters: [...]
          output_parameters: [...]
  ifex_schema: string             # Full IFEX YAML (optional)
  last_heartbeat: uint64          # Unix timestamp

service_filter_t:
  name_pattern: string            # Glob pattern
  has_method: string              # Must have this method
  transport_type: transport_type_t
  available_only: boolean
```

### Behavior

- **In-memory storage** - No persistence (POC limitation)
- **Multi-instance** - Same service name can have multiple registrations
- **Status tracking** - Per registration, not per service name
- **Heartbeat timeout** - Marks service UNAVAILABLE after missed heartbeats

### Default Port

`50051`

---

## Dispatcher Service

### Purpose

Routes method calls to any IFEX service. Caller specifies service name + method name + JSON parameters. Dispatcher handles lookup, validation, and execution.

### Rationale

Direct gRPC calls require knowing the service's protobuf interface at compile time. Dispatcher provides:
- Single entry point for all service calls
- Runtime service lookup via Discovery
- Parameter validation using IFEX schema
- Uniform error handling and timeouts
- JSON in/out (no protobuf knowledge needed by caller)

### API

| Method | Description |
|--------|-------------|
| `call_method` | Execute any method on any registered service |

### Data Structures

```yaml
method_call_t:
  service_name: string
  method_name: string
  parameters: string              # JSON-encoded
  timeout_ms: uint32              # Default: 5000

call_result_t:
  status: SUCCESS | FAILED | TIMEOUT | SERVICE_UNAVAILABLE | METHOD_NOT_FOUND | INVALID_PARAMETERS
  response: string                # JSON-encoded result
  error_message: string
  duration_ms: uint32
  service_endpoint: string        # Where the call was routed
```

### Call Flow

1. Receive call request (service, method, JSON params)
2. Query Discovery for service endpoint and schema
3. Validate parameters against IFEX schema
4. Build protobuf request from JSON
5. Execute gRPC call with timeout
6. Convert protobuf response to JSON
7. Return result or error

### Default Port

`50052`

---

## Orchestrator Service

### Purpose

Executes multi-step workflows that coordinate multiple services. Workflows are declarative definitions (YAML/JSON), not code.

### Rationale

Complex operations require coordinating multiple services with conditional logic, error handling, and result passing. Without orchestration:
- Each integration is custom code
- Error handling is duplicated
- Changes require recompilation
- AI can't generate complex operations

Orchestrator provides:
- Declarative workflow definitions
- Step dependencies and parallel execution
- Conditional branching
- Error handling and compensation (saga pattern)
- Result passing between steps

### API

| Method | Description |
|--------|-------------|
| `register_workflow` | Register a workflow definition |
| `execute_workflow` | Start workflow execution with inputs |
| `get_execution_status` | Get status of running/completed workflow |
| `cancel_execution` | Cancel a running workflow |
| `list_workflows` | List registered workflow definitions |
| `get_workflow` | Get a specific workflow definition |

### Data Structures

```yaml
workflow_definition_t:
  name: string
  version: string
  description: string
  input_schema: string            # JSON Schema for inputs
  steps: step_definition_t[]

step_definition_t:
  id: string
  service: string
  method: string
  parameters: string              # JSON template with {{}} expressions
  depends_on: string[]            # Step IDs this depends on
  when: string                    # Condition expression
  timeout_ms: uint32
  retry_count: uint8
  compensate_method: string       # Called on rollback

workflow_execution_t:
  execution_id: string
  workflow_name: string
  status: PENDING | RUNNING | COMPLETED | FAILED | CANCELLED | PAUSED
  inputs: string                  # JSON
  step_results: step_result_t[]
  started_at: string              # ISO8601
  completed_at: string
  error_message: string

step_result_t:
  step_id: string
  status: PENDING | RUNNING | COMPLETED | FAILED | SKIPPED | COMPENSATING
  result: string                  # JSON from service
  error_message: string
  duration_ms: uint32
```

### Workflow Example

```yaml
name: departure_prep
version: "1.0"

steps:
  - id: weather
    service: weather-service
    method: get_conditions

  - id: defrost
    service: defrost-service
    method: start_defrost
    depends_on: [weather]
    when: "{{steps.weather.result.temperature < 3}}"
    compensate_method: stop_defrost

  - id: climate
    service: climate-service
    method: set_temperature
    parameters:
      target: "{{inputs.comfort_temp}}"

  - id: notify
    service: notification-service
    method: send
    depends_on: [defrost, climate]
    parameters:
      message: "Vehicle ready"
```

### Expression Language

Parameters and conditions support template expressions:

```yaml
# Reference inputs
"{{inputs.departure_time}}"

# Reference step results
"{{steps.weather.result.temperature}}"

# Conditions
when: "{{steps.weather.result.frost == true}}"

# Built-in functions
"{{time_add(inputs.departure_time, '-30m')}}"
```

### Execution Model

- Steps with satisfied dependencies run in parallel
- `when` condition evaluated at runtime; false = SKIPPED
- Failed step triggers compensation in reverse order
- Execution state persisted for recovery

### Default Port

`50054` (planned)

See [Orchestrator Service Design](orchestrator-service-design.md) for full specification.

---

## Scheduler Service

### Purpose

Calendar-style job scheduler. Jobs specify a service method (or workflow) to execute at a scheduled time, optionally recurring.

### Rationale

Many vehicle operations are time-based:
- Pre-condition cabin before departure
- Run diagnostics overnight
- Schedule maintenance checks

Scheduler provides:
- CRUD for scheduled jobs
- Recurring schedules (daily, weekly, cron)
- Calendar views for UI
- Execution via Dispatcher or Orchestrator
- Job status tracking

### API

| Method | Description |
|--------|-------------|
| `create_job` | Schedule a new job |
| `get_job` | Get single job by ID |
| `get_jobs` | List jobs with optional filter |
| `update_job` | Modify scheduled job |
| `delete_job` | Remove job |
| `get_calendar_view` | Get jobs in date range (for calendar UI) |

### Data Structures

```yaml
job_t:
  id: string
  title: string                   # Human-readable name
  service: string                 # Target service (or "ifex-orchestrator")
  method: string                  # Target method (or "execute_workflow")
  parameters: string              # JSON-encoded
  scheduled_time: string          # ISO8601
  recurrence_rule: string         # "daily" | "weekly" | "hourly" | cron expression
  end_time: string                # For recurring jobs
  status: PENDING | RUNNING | COMPLETED | FAILED | CANCELLED
  created_at: string
  updated_at: string
  executed_at: string
  next_run_time: string           # For recurring
  error_message: string
  result: string                  # JSON result from execution

job_filter_t:
  start_date: string
  end_date: string
  service: string
  status: job_status_t
  include_completed: boolean
```

### Job Examples

**Simple job (calls service directly):**
```yaml
title: "Morning defrost"
service: defrost-service
method: start_defrost
parameters: {"intensity": "rapid"}
scheduled_time: "2025-01-15T06:30:00Z"
recurrence_rule: "0 30 6 * * MON-FRI"
```

**Workflow job (calls Orchestrator):**
```yaml
title: "Departure prep"
service: ifex-orchestrator
method: execute_workflow
parameters:
  workflow_name: "departure_prep"
  inputs:
    comfort_temp: 22
scheduled_time: "2025-01-15T06:30:00Z"
```

### Execution

Background thread polls pending jobs every second:
1. Find jobs where `scheduled_time <= now` and status = PENDING
2. Set status = RUNNING
3. Call Dispatcher with service/method/parameters
4. Set status = COMPLETED or FAILED
5. Store result or error_message
6. For recurring jobs: create next occurrence

### Default Port

`50053`

---

## Design Principles

1. **No hardcoded addresses** - All service locations from Discovery
2. **Schema-driven** - IFEX schemas enable validation and introspection
3. **JSON interchange** - Universal format, no protobuf coupling for callers
4. **Stateless calls** - Each call is independent
5. **Timeout everything** - Prevent hangs with configurable timeouts
6. **Status tracking** - Services report health, jobs track execution state
7. **Fail-safe** - Infrastructure failure doesn't break vehicle

---

## Startup Sequence

```bash
# 1. Start Discovery (fixed port, other services need to find it)
./ifex-discovery-service --port 50051

# 2. Start Dispatcher (registers with Discovery)
./ifex-dispatcher-service --discovery localhost:50051

# 3. Start Orchestrator (registers with Discovery, uses Dispatcher)
./ifex-orchestrator-service --discovery localhost:50051

# 4. Start Scheduler (registers with Discovery, uses Dispatcher/Orchestrator)
./ifex-scheduler-service --discovery localhost:50051

# 5. Start application services (register with Discovery)
./climate-comfort-service --discovery localhost:50051
./beverage-service --discovery localhost:50051
```

Or use the helper script:
```bash
./start-all-bg.sh
```

---

## Current Limitations (POC)

| Limitation | Impact |
|------------|--------|
| In-memory storage | State lost on restart |
| Single instance | No clustering or replication |
| Basic recurrence | Only daily/weekly/hourly, partial cron |
| No authentication | Open access |
| No rate limiting | No protection against flooding |
| Orchestrator not implemented | Workflows are future work |

---

## Reference Services

Beyond the core infrastructure, the platform includes reference implementations:

| Service | Purpose | Documentation |
|---------|---------|---------------|
| **Backend Transport** | Vehicle-to-cloud communication (MQTT) | [README](../reference-services/backend-transport/README.md) |

These services demonstrate best practices and can be used as-is or adapted for specific deployments.

---

## Related Documentation

- [Orchestrator Service Design](orchestrator-service-design.md) - Full workflow engine specification
- [IFEX Service Architecture](ifex-service-architecture.md) - How to design services
- [VSS and IFEX Relationship](vss-ifex-relationship.md) - When to use each
- [Backend Transport Service](../reference-services/backend-transport/README.md) - Cloud connectivity
