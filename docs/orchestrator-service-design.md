# Orchestrator Service Design

## Overview

The Orchestrator is the fourth core infrastructure service, responsible for executing multi-step workflows. It sits between Scheduler (timing) and Dispatcher (execution), managing the coordination logic.

```
Scheduler → Orchestrator → Dispatcher → Services
  (when)      (what)         (how)       (do)
```

---

## Core Concepts

### Workflow

A workflow is a directed graph of steps that execute service methods. Workflows are **data** (YAML/JSON), not code.

```yaml
workflow:
  name: departure_prep
  version: 1.0

  inputs:
    departure_time: datetime
    comfort_level: string

  steps:
    - id: step_1
      ...
    - id: step_2
      depends_on: [step_1]
      ...
```

### Step

A step represents a single service method invocation with optional conditions, error handling, and result mapping.

```yaml
step:
  id: start_climate
  service: climate-comfort
  method: set_cabin_temperature

  parameters:
    target_celsius: 22
    zone: all

  when: "{{inputs.comfort_level != 'none'}}"
  timeout_ms: 30000

  on_success:
    - notify: "Climate started"
  on_failure:
    - compensate: stop_climate
```

### Execution Context

Runtime state that flows through the workflow:

```yaml
context:
  workflow_id: "wf_123456"
  started_at: "2025-01-15T06:30:00Z"

  inputs:
    departure_time: "2025-01-15T07:00:00Z"

  steps:
    check_weather:
      status: completed
      result:
        temperature: -2
        frost: true

    start_defrost:
      status: running
      started_at: "2025-01-15T06:31:00Z"
```

---

## IFEX Schema

```yaml
name: ifex_orchestrator
major_version: 1
minor_version: 0
description: Workflow orchestration for IFEX services

namespaces:
  - name: workflows
    description: Workflow management

    enumerations:
      - name: workflow_status_t
        datatype: uint8
        options:
          - name: PENDING
            value: 0
          - name: RUNNING
            value: 1
          - name: COMPLETED
            value: 2
          - name: FAILED
            value: 3
          - name: CANCELLED
            value: 4
          - name: PAUSED
            value: 5

      - name: step_status_t
        datatype: uint8
        options:
          - name: PENDING
            value: 0
          - name: RUNNING
            value: 1
          - name: COMPLETED
            value: 2
          - name: FAILED
            value: 3
          - name: SKIPPED
            value: 4
          - name: COMPENSATING
            value: 5

    structs:
      - name: step_definition_t
        members:
          - name: id
            datatype: string
          - name: service
            datatype: string
          - name: method
            datatype: string
          - name: parameters
            datatype: string
            description: JSON template with {{}} expressions
          - name: depends_on
            datatype: string[]
          - name: when
            datatype: string
            description: Condition expression
          - name: timeout_ms
            datatype: uint32
            default: 30000
          - name: retry_count
            datatype: uint8
            default: 0
          - name: compensate_method
            datatype: string
            mandatory: false

      - name: workflow_definition_t
        members:
          - name: name
            datatype: string
          - name: version
            datatype: string
          - name: description
            datatype: string
          - name: input_schema
            datatype: string
            description: JSON Schema for inputs
          - name: steps
            datatype: step_definition_t[]

      - name: step_result_t
        members:
          - name: step_id
            datatype: string
          - name: status
            datatype: step_status_t
          - name: result
            datatype: string
            description: JSON result from service
          - name: error_message
            datatype: string
            mandatory: false
          - name: started_at
            datatype: string
          - name: completed_at
            datatype: string
            mandatory: false
          - name: duration_ms
            datatype: uint32

      - name: workflow_execution_t
        members:
          - name: execution_id
            datatype: string
          - name: workflow_name
            datatype: string
          - name: status
            datatype: workflow_status_t
          - name: inputs
            datatype: string
            description: JSON inputs
          - name: step_results
            datatype: step_result_t[]
          - name: started_at
            datatype: string
          - name: completed_at
            datatype: string
            mandatory: false
          - name: error_message
            datatype: string
            mandatory: false

    methods:
      - name: register_workflow
        description: Register a workflow definition
        input:
          - name: workflow
            datatype: workflow_definition_t
        output:
          - name: success
            datatype: boolean
          - name: message
            datatype: string

      - name: execute_workflow
        description: Start a workflow execution
        input:
          - name: workflow_name
            datatype: string
          - name: inputs
            datatype: string
            description: JSON inputs
        output:
          - name: execution_id
            datatype: string
          - name: success
            datatype: boolean
          - name: message
            datatype: string

      - name: get_execution_status
        description: Get status of a workflow execution
        input:
          - name: execution_id
            datatype: string
        output:
          - name: execution
            datatype: workflow_execution_t
          - name: success
            datatype: boolean

      - name: cancel_execution
        description: Cancel a running workflow
        input:
          - name: execution_id
            datatype: string
        output:
          - name: success
            datatype: boolean
          - name: message
            datatype: string

      - name: list_workflows
        description: List registered workflows
        output:
          - name: workflows
            datatype: workflow_definition_t[]

      - name: get_workflow
        description: Get a workflow definition
        input:
          - name: workflow_name
            datatype: string
        output:
          - name: workflow
            datatype: workflow_definition_t
          - name: found
            datatype: boolean
```

---

## Expression Language

Parameters and conditions support template expressions:

### Variable References

```yaml
# Input variables
"{{inputs.departure_time}}"

# Step results
"{{steps.check_weather.result.temperature}}"

# Context variables
"{{context.workflow_id}}"
"{{context.started_at}}"

# Built-in functions
"{{now()}}"
"{{time_add(inputs.departure_time, '-30m')}}"
```

### Condition Expressions

```yaml
when: "{{steps.weather.result.temperature < 5}}"
when: "{{inputs.enable_defrost == true}}"
when: "{{steps.previous.status == 'completed'}}"
```

### Supported Operators

| Category | Operators |
|----------|-----------|
| Comparison | `==`, `!=`, `<`, `>`, `<=`, `>=` |
| Logical | `and`, `or`, `not` |
| Arithmetic | `+`, `-`, `*`, `/` |
| String | `contains`, `starts_with`, `ends_with` |

### Built-in Functions

| Function | Description |
|----------|-------------|
| `now()` | Current timestamp |
| `time_add(time, duration)` | Add duration to time |
| `time_diff(t1, t2)` | Difference between times |
| `json_path(obj, path)` | Extract value from JSON |
| `default(value, fallback)` | Use fallback if null |

---

## Execution Model

### Step Ordering

Steps execute based on `depends_on` declarations:

```yaml
steps:
  - id: A                    # Runs first (no dependencies)

  - id: B
    depends_on: [A]          # Runs after A

  - id: C
    depends_on: [A]          # Runs after A (parallel with B)

  - id: D
    depends_on: [B, C]       # Runs after both B and C complete
```

Execution graph:
```
    A
   / \
  B   C
   \ /
    D
```

### Parallel Execution

Steps with satisfied dependencies run in parallel:

```yaml
steps:
  - id: climate
    service: climate-comfort
    method: start

  - id: defrost
    service: defrost
    method: start
    # No depends_on - runs parallel with climate

  - id: beverage
    service: beverage
    method: prepare
    depends_on: [climate]    # Waits for climate only
```

### Conditional Execution

```yaml
steps:
  - id: check_temp
    service: weather
    method: get_temperature

  - id: defrost
    service: defrost
    method: start
    depends_on: [check_temp]
    when: "{{steps.check_temp.result.celsius < 3}}"
    # Skipped if condition is false
```

When a step is skipped:
- Status becomes `SKIPPED`
- Dependent steps still execute (they see `null` result)
- Workflow continues

### Error Handling

#### Retry

```yaml
steps:
  - id: call_external
    service: weather-api
    method: fetch
    retry_count: 3
    retry_delay_ms: 1000
```

#### Compensation (Saga Pattern)

```yaml
steps:
  - id: reserve_climate
    service: climate
    method: reserve_capacity
    compensate_method: release_capacity

  - id: reserve_power
    service: power
    method: reserve_watts
    compensate_method: release_watts

  - id: start_climate
    service: climate
    method: start
    # If this fails, compensate_method is called for
    # reserve_power, then reserve_climate (reverse order)
```

#### On-Failure Actions

```yaml
steps:
  - id: critical_step
    service: important
    method: do_thing
    on_failure:
      - service: notifications
        method: send_alert
        parameters:
          message: "Critical step failed: {{error}}"
```

---

## Scheduler Integration

Scheduler triggers Orchestrator via `execute_workflow`:

```yaml
# Scheduler job
job:
  title: "Morning departure prep"
  service: ifex-orchestrator
  method: execute_workflow
  parameters:
    workflow_name: "departure_prep"
    inputs:
      departure_time: "2025-01-15T07:00:00Z"
      comfort_level: "cozy"
  scheduled_time: "2025-01-15T06:30:00Z"
  recurrence_rule: "0 30 6 * * MON-FRI"
```

The Scheduler doesn't know about workflow internals - it just triggers execution.

---

## Example Workflows

### Simple: Sequential

```yaml
name: quick_comfort
steps:
  - id: climate
    service: climate-comfort
    method: set_temperature
    parameters:
      target: "{{inputs.temperature}}"

  - id: seats
    service: seat-control
    method: set_heating
    depends_on: [climate]
    parameters:
      level: "{{inputs.seat_heat}}"
```

### Medium: Conditional Parallel

```yaml
name: departure_prep
steps:
  - id: weather
    service: weather
    method: get_conditions

  - id: defrost
    service: defrost
    method: auto_defrost
    depends_on: [weather]
    when: "{{steps.weather.result.frost_risk > 0.5}}"

  - id: climate
    service: climate-comfort
    method: set_temperature
    depends_on: [weather]
    parameters:
      target: "{{default(inputs.target_temp, 22)}}"

  - id: beverage
    service: beverage
    method: prepare
    parameters:
      type: "{{inputs.drink_type}}"
    # No depends_on - parallel with defrost and climate

  - id: notify
    service: notification
    method: send
    depends_on: [defrost, climate, beverage]
    parameters:
      message: "Vehicle ready for departure"
```

### Complex: Saga with Compensation

```yaml
name: ev_departure_prep
description: Prepare EV for departure with resource reservation

steps:
  - id: check_battery
    service: battery
    method: get_status

  - id: reserve_charging
    service: charging
    method: reserve_slot
    depends_on: [check_battery]
    when: "{{steps.check_battery.result.soc < 80}}"
    compensate_method: cancel_reservation

  - id: precondition_battery
    service: battery
    method: start_preconditioning
    depends_on: [reserve_charging]
    compensate_method: stop_preconditioning

  - id: climate
    service: climate
    method: start
    depends_on: [check_battery]
    compensate_method: stop

  - id: final_check
    service: vehicle
    method: readiness_check
    depends_on: [precondition_battery, climate]
    # If this fails, compensate climate, then battery, then charging
```

---

## Implementation Considerations

### State Persistence

Workflow executions should survive restarts:

```
┌─────────────────────────────────────────┐
│           Orchestrator                   │
│                                         │
│  ┌─────────────┐    ┌───────────────┐  │
│  │ Executor    │───▶│ State Store   │  │
│  │ Engine      │    │ (SQLite/etc)  │  │
│  └─────────────┘    └───────────────┘  │
│                                         │
└─────────────────────────────────────────┘
```

On restart:
1. Load incomplete executions
2. Resume from last completed step
3. Re-run current step (idempotency required)

### Idempotency

Services should handle duplicate calls gracefully:

```yaml
# Good - includes idempotency key
parameters:
  request_id: "{{context.execution_id}}_{{step.id}}"
  target_temp: 22
```

### Timeouts

```yaml
step:
  timeout_ms: 30000  # Step timeout

workflow:
  timeout_ms: 300000  # Overall workflow timeout
```

### Observability

Emit events for monitoring:

```
workflow.started    {workflow_id, workflow_name, inputs}
step.started        {execution_id, step_id, service, method}
step.completed      {execution_id, step_id, status, duration_ms}
step.failed         {execution_id, step_id, error}
workflow.completed  {execution_id, status, duration_ms}
```

---

## API Usage Examples

### Register a Workflow

```json
POST /orchestrator/register_workflow
{
  "workflow": {
    "name": "morning_prep",
    "version": "1.0.0",
    "description": "Prepare vehicle for morning departure",
    "input_schema": {
      "type": "object",
      "properties": {
        "departure_time": {"type": "string", "format": "date-time"},
        "comfort_temp": {"type": "number", "default": 22}
      },
      "required": ["departure_time"]
    },
    "steps": [...]
  }
}
```

### Execute a Workflow

```json
POST /orchestrator/execute_workflow
{
  "workflow_name": "morning_prep",
  "inputs": {
    "departure_time": "2025-01-15T07:00:00Z",
    "comfort_temp": 23
  }
}

Response:
{
  "execution_id": "exec_abc123",
  "success": true
}
```

### Check Status

```json
GET /orchestrator/get_execution_status?execution_id=exec_abc123

Response:
{
  "execution": {
    "execution_id": "exec_abc123",
    "workflow_name": "morning_prep",
    "status": "RUNNING",
    "started_at": "2025-01-15T06:30:00Z",
    "step_results": [
      {
        "step_id": "weather",
        "status": "COMPLETED",
        "result": "{\"temp\": -2, \"frost\": true}",
        "duration_ms": 150
      },
      {
        "step_id": "defrost",
        "status": "RUNNING",
        "started_at": "2025-01-15T06:30:01Z"
      }
    ]
  },
  "success": true
}
```

---

## Future Extensions

### Sub-Workflows

```yaml
steps:
  - id: prep_cabin
    workflow: cabin_preparation  # Reference another workflow
    inputs:
      temperature: "{{inputs.target_temp}}"
```

### Loops

```yaml
steps:
  - id: notify_passengers
    service: notification
    method: send
    for_each: "{{inputs.passengers}}"
    parameters:
      recipient: "{{item.phone}}"
      message: "Vehicle departing in 10 minutes"
```

### Event Triggers (Mid-Workflow)

```yaml
steps:
  - id: wait_for_door
    wait_for:
      event: "vss.Vehicle.Cabin.Door.Row1.Left.IsOpen"
      condition: "{{value == false}}"
      timeout_ms: 60000
```

---

## Summary

The Orchestrator service provides:

1. **Workflow definitions** - Declarative, versioned, data-driven
2. **Step coordination** - Dependencies, parallelism, conditions
3. **Error handling** - Retry, compensation, failure actions
4. **Result passing** - Template expressions between steps
5. **Observability** - Status tracking, event emission

This enables complex multi-service operations without hardcoded integration logic.
