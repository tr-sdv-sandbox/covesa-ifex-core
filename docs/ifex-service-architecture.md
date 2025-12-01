# IFEX Service Architecture

## Overview

IFEX services provide semantic abstractions over vehicle systems. They orchestrate operations while respecting automotive safety requirements.

This document covers how to design and implement IFEX services that integrate with the platform.

---

## Key Principles

1. **IFEX services are non-safety-critical** - Run in isolated environments, cannot directly control hardware
2. **VSS is the interface to hardware** - Vehicle Signal Specification defines the contract with ASIL-rated systems
3. **Semantic over raw** - Expose meaningful operations, not signal wrappers
4. **Request-based** - Operations are requests to state machines, not direct commands
5. **Self-describing** - Services register with full IFEX schema for discovery and validation

---

## System Layers

```
┌─────────────────────────────────────────────────────────────┐
│                    Application Layer                         │
│         User Apps / AI / Voice / Orchestration               │
│                                                              │
│                    Uses IFEX services                        │
└──────────────────────────┬──────────────────────────────────┘
                           │
                      IFEX methods
                           │
┌──────────────────────────┴──────────────────────────────────┐
│                    Service Layer                             │
│              IFEX Services (this document)                   │
│                                                              │
│  - Semantic operations                                       │
│  - Coordination logic                                        │
│  - Error handling                                            │
│  - Discovery registration                                    │
└──────────────────────────┬──────────────────────────────────┘
                           │
                      VSS signals
                           │
┌──────────────────────────┴──────────────────────────────────┐
│                    Vehicle Layer                             │
│               KUKSA.val / VSS Databroker                     │
│                                                              │
│            Mediates access to ASIL controllers               │
└──────────────────────────┬──────────────────────────────────┘
                           │
┌──────────────────────────┴──────────────────────────────────┐
│                    Hardware Layer                            │
│              ASIL-rated ECUs and controllers                 │
│                                                              │
│              Has final authority over actions                │
└─────────────────────────────────────────────────────────────┘
```

See [VSS and IFEX Relationship](vss-ifex-relationship.md) for detailed guidance on when to use each layer.

---

## Service Design Guidelines

### Do

- **Semantic operations**: `set_comfort_temperature(cozy/normal/fresh)` not `set_vss_signal(path, value)`
- **Orchestrate sequences**: Coordinate multiple systems with proper timing
- **Handle state machines**: Monitor signals, wait for transitions, handle timeouts
- **Abstract complexity**: Hide OEM-specific details behind stable interfaces
- **Return meaningful results**: Estimated time, acceptance status, not just success/failure
- **Register with Discovery**: Expose full schema for introspection

### Don't

- **Wrap VSS directly**: Pointless abstraction, just use VSS
- **Assume immediate execution**: Hardware state machines take time
- **Bypass safety**: Respect controller rejections
- **Block indefinitely**: Always use timeouts
- **Couple to other services**: Services are independent; use Orchestrator for coordination

---

## VSS Integration Pattern

IFEX services are the primary consumers of VSS. They use VSS internally but expose semantic interfaces externally.

```
┌─────────────────────────────────────────────────────────────┐
│                    IFEX Service                              │
│                                                              │
│  ┌────────────────────────────────────────────────────────┐ │
│  │              Business Logic                             │ │
│  │  - Map semantic input to concrete values                │ │
│  │  - Check preconditions                                  │ │
│  │  - Coordinate multiple signals                          │ │
│  │  - Monitor for completion                               │ │
│  │  - Handle errors and edge cases                         │ │
│  └───────────────────────┬────────────────────────────────┘ │
│                          │                                   │
│  ┌───────────────────────┴────────────────────────────────┐ │
│  │              VSS Operations                             │ │
│  │  - sensor.get("Vehicle.Cabin.Temperature")              │ │
│  │  - actuator.set("Vehicle.Cabin.HVAC.Target", 22)        │ │
│  │  - subscribe("Vehicle.Cabin.Door.IsOpen", callback)     │ │
│  └────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────┘
```

### Example: Climate Comfort Service

**IFEX Schema:**
```yaml
name: climate_comfort_service
version: 1.0.0

namespaces:
  - name: climate
    methods:
      - name: set_comfort_level
        description: Set cabin comfort to a semantic level
        input:
          - name: level
            datatype: string
            description: cozy, normal, or fresh
          - name: zones
            datatype: string
            description: all, front, rear, or driver
        output:
          - name: accepted
            datatype: boolean
          - name: estimated_time_seconds
            datatype: uint32
```

**Implementation (pseudocode):**
```python
def set_comfort_level(level, zones):
    # 1. Map semantic to concrete (domain knowledge)
    targets = {
        "cozy": {"temp": 23, "fan": "low", "seat_heat": 2},
        "normal": {"temp": 21, "fan": "auto", "seat_heat": 0},
        "fresh": {"temp": 19, "fan": "high", "seat_heat": 0}
    }
    config = targets[level]

    # 2. Check preconditions via VSS
    if vss.get("Vehicle.Cabin.Sunroof.Position") > 0:
        vss.set("Vehicle.Cabin.Sunroof.Position", 0)
        wait_for("Vehicle.Cabin.Sunroof.Position", 0, timeout=30)

    # 3. Apply settings via VSS
    for zone in expand_zones(zones):
        vss.set(f"Vehicle.Cabin.HVAC.{zone}.Temperature.Target", config["temp"])
        vss.set(f"Vehicle.Cabin.Seat.{zone}.Heating", config["seat_heat"])

    vss.set("Vehicle.Cabin.HVAC.Fan.Mode", config["fan"])

    # 4. Calculate estimate
    current = vss.get("Vehicle.Cabin.HVAC.Row1.Left.Temperature.Actual")
    estimate = calculate_time(current, config["temp"])

    # 5. Return meaningful result
    return {
        "accepted": True,
        "estimated_time_seconds": estimate
    }
```

---

## Service Structure

### IFEX Schema Definition

Every service needs an IFEX YAML file:

```yaml
name: my_service
major_version: 1
minor_version: 0
description: What this service does

namespaces:
  - name: main
    description: Primary functionality

    enumerations:
      - name: status_t
        datatype: uint8
        options:
          - name: OK
            value: 0
          - name: ERROR
            value: 1

    structs:
      - name: result_t
        members:
          - name: success
            datatype: boolean
          - name: message
            datatype: string

    methods:
      - name: do_something
        description: Performs an operation
        input:
          - name: param1
            datatype: string
            mandatory: true
          - name: param2
            datatype: uint32
            default: 100
        output:
          - name: result
            datatype: result_t
```

### Registration with Discovery

At startup, services must register:

```cpp
// Create discovery client
auto discovery = ifex::DiscoveryClient::create("localhost:50051");

// Create endpoint info
ifex::ServiceEndpoint endpoint;
endpoint.address = "192.168.1.50:50100";
endpoint.transport = ifex::ServiceEndpoint::Transport::GRPC;

// Read IFEX schema
std::string schema = read_file("my-service.ifex.yml");

// Register
std::string registration_id = discovery->register_service(endpoint, schema);

// Start heartbeat thread
while (running) {
    discovery->send_heartbeat(registration_id, ifex::ServiceStatus::AVAILABLE);
    sleep(30);
}
```

### gRPC Service Implementation

Proto is generated from IFEX schema. Implement the service:

```cpp
class MyServiceImpl : public swdv::my_service::do_something_service::Service {
    grpc::Status do_something(
        grpc::ServerContext* context,
        const swdv::my_service::do_something_request* request,
        swdv::my_service::do_something_response* response) override
    {
        // Implementation
        auto* result = response->mutable_result();
        result->set_success(true);
        result->set_message("Done");
        return grpc::Status::OK;
    }
};
```

---

## Error Handling

### Return Errors, Don't Throw

Services should return error information in the response:

```yaml
output:
  - name: success
    datatype: boolean
  - name: error_code
    datatype: uint32
    mandatory: false
  - name: error_message
    datatype: string
    mandatory: false
```

### Common Error Patterns

| Situation | Response |
|-----------|----------|
| Invalid parameters | `success: false`, validation error message |
| Precondition failed | `success: false`, explain what's wrong |
| Hardware rejected | `success: false`, controller response |
| Timeout | `success: false`, timeout message |
| Partial success | `success: true`, warnings in message |

### Graceful Degradation

If a service can't complete fully, it should:
1. Complete what it can
2. Report partial success
3. Provide actionable information

```python
def set_comfort_level(level, zones):
    results = []
    for zone in zones:
        try:
            set_zone_temperature(zone, target)
            results.append({"zone": zone, "success": True})
        except Exception as e:
            results.append({"zone": zone, "success": False, "error": str(e)})

    all_success = all(r["success"] for r in results)
    return {
        "accepted": True,  # Request was processed
        "fully_applied": all_success,
        "zone_results": results
    }
```

---

## Safety Considerations

### Non-Safety-Critical

IFEX services run in a non-ASIL environment:
- They can crash without vehicle impact
- They can be updated/restarted freely
- They cannot bypass hardware safety

### VSS as Safety Boundary

```
┌─────────────────────────────────────────────────────────────┐
│         IFEX Service (can fail)                              │
│                                                              │
│    "Set temperature to 22°C"                                 │
└──────────────────────────┬──────────────────────────────────┘
                           │
            VSS Actuator Write (request)
                           │
                           ▼
┌─────────────────────────────────────────────────────────────┐
│         ASIL Controller (cannot fail)                        │
│                                                              │
│    Validates request                                         │
│    Checks safety constraints                                 │
│    Applies or rejects                                        │
└─────────────────────────────────────────────────────────────┘
```

### Request, Don't Command

All VSS actuator writes are *requests*. The controller decides:

```python
# This is a REQUEST, not a command
vss.set("Vehicle.Cabin.HVAC.Temperature.Target", 22)

# The controller may:
# - Accept and apply
# - Accept but delay
# - Reject (safety constraint)
# - Modify (limit to safe range)

# Always verify via sensors
actual = vss.get("Vehicle.Cabin.HVAC.Temperature.Actual")
```

---

## Testing

### Unit Testing

Test service logic without VSS:

```python
def test_comfort_mapping():
    config = map_comfort_level("cozy")
    assert config["temp"] == 23
    assert config["seat_heat"] == 2

def test_zone_expansion():
    zones = expand_zones("all")
    assert "Row1.Left" in zones
    assert "Row1.Right" in zones
```

### Integration Testing

Test with mock VSS:

```python
def test_set_comfort_level():
    mock_vss = MockVSS()
    mock_vss.set("Vehicle.Cabin.HVAC.Row1.Left.Temperature.Actual", 18)

    service = ClimateService(mock_vss)
    result = service.set_comfort_level("cozy", "all")

    assert result["accepted"] == True
    assert mock_vss.get("Vehicle.Cabin.HVAC.Row1.Left.Temperature.Target") == 23
```

### Service Testing

Test via Dispatcher:

```bash
# Using grpcurl
grpcurl -d '{
  "call": {
    "service_name": "climate-comfort",
    "method_name": "set_comfort_level",
    "parameters": "{\"level\": \"cozy\", \"zones\": \"all\"}"
  }
}' localhost:50052 swdv.ifex_dispatcher.call_method_service/call_method
```

---

## Example Services

The platform includes several example services:

| Service | Purpose | Location |
|---------|---------|----------|
| Climate Comfort | Cabin temperature/comfort | `test-services/climate-comfort/` |
| Defrost | Window/mirror defrosting | `test-services/defrost/` |
| Beverage | Coffee/tea preparation | `test-services/beverage/` |
| Settings | User preference management | `test-services/settings/` |
| Departure Orchestration | Pre-departure coordination | `test-services/departure-orchestration/` |

---

## Related Documentation

- [VSS and IFEX Relationship](vss-ifex-relationship.md) - When to use VSS vs IFEX
- [Core Services Specification](core-services-spec.md) - Infrastructure services
- [Orchestrator Service Design](orchestrator-service-design.md) - Multi-service workflows
