# VSS and IFEX: Understanding the Relationship

## Overview

**VSS** (Vehicle Signal Specification) and **IFEX** (Interface Exchange) are complementary standards that work together in the vehicle software stack. Understanding when to use each is essential for building effective vehicle services.

| Standard | Describes | Level |
|----------|-----------|-------|
| **VSS** | Vehicle **state** | Raw signals (sensors, actuators) |
| **IFEX** | Vehicle **capabilities** | Semantic operations (services, methods) |

**Simple rule:** VSS tells you what the vehicle *is*. IFEX tells you what it can *do*.

---

## The Layered Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                      Intent Layer                            │
│         "Prepare for departure" / "Make it cozy"            │
│                                                              │
│                         IFEX                                 │
│              (semantic operations, orchestration)            │
└──────────────────────────┬──────────────────────────────────┘
                           │
                    method calls
                           │
┌──────────────────────────┴──────────────────────────────────┐
│                      State Layer                             │
│     Vehicle.Cabin.HVAC.Temperature = 22                      │
│     Vehicle.Cabin.Door.Row1.Left.IsOpen = false             │
│                                                              │
│                          VSS                                 │
│              (signals, sensors, actuators)                   │
└──────────────────────────┬──────────────────────────────────┘
                           │
                    signal read/write
                           │
┌──────────────────────────┴──────────────────────────────────┐
│                    Hardware Layer                            │
│              ECUs, sensors, actuators                        │
└─────────────────────────────────────────────────────────────┘
```

**IFEX services are the primary consumers of VSS.** They encapsulate VSS complexity and expose meaningful operations.

---

## VSS: The State Layer

VSS provides a standardized taxonomy for vehicle signals. Using KUKSA.val as the databroker:

### Reading Sensors
```python
# Current cabin temperature
temp = kuksa.get("Vehicle.Cabin.HVAC.Row1.Left.Temperature")

# Door state
is_open = kuksa.get("Vehicle.Cabin.Door.Row1.Left.IsOpen")

# Battery level
soc = kuksa.get("Vehicle.Powertrain.TractionBattery.StateOfCharge.Current")
```

### Writing Actuators
```python
# Request temperature change (note: request, not command)
kuksa.set("Vehicle.Cabin.HVAC.Row1.Left.Temperature.Target", 22)

# Request seat heating
kuksa.set("Vehicle.Cabin.Seat.Row1.Left.Heating", 3)
```

### Subscribing to Changes
```python
kuksa.subscribe("Vehicle.Cabin.Door.Row1.Left.IsOpen", on_door_change)
```

**Key insight:** VSS operations are *requests*. The underlying ASIL-rated controllers decide whether to honor them.

---

## IFEX: The Capability Layer

IFEX defines service interfaces with semantic meaning:

### Service Definition
```yaml
name: climate_comfort_service
version: 1.0.0

methods:
  - name: set_comfort_level
    description: Set cabin comfort to a semantic level
    input:
      - name: level
        type: enum
        values: [cozy, normal, fresh]
      - name: zones
        type: enum
        values: [all, front, rear, driver]
    output:
      - name: accepted
        type: boolean
      - name: estimated_time_seconds
        type: uint32
```

### Service Call
```python
result = dispatcher.call(
    service="climate-comfort",
    method="set_comfort_level",
    parameters={"level": "cozy", "zones": "all"}
)
```

---

## When to Use Which

### Decision Framework

Ask these questions:

| Question | If Yes → | If No → |
|----------|----------|---------|
| Is this a single signal value? | VSS | Consider IFEX |
| Does it require coordination of multiple signals? | IFEX | VSS might work |
| Is domain knowledge needed? | IFEX | VSS |
| Does caller need to understand vehicle internals? | VSS | IFEX |

### Use VSS Directly For:

1. **Dashboards/Monitoring** - Display raw vehicle state
   ```
   Show: Speed, Battery SOC, Cabin Temperature
   ```

2. **Simple actuator control** - Single signal, obvious mapping
   ```
   Turn on seat heater: Vehicle.Cabin.Seat.Row1.Left.Heating = 3
   ```

3. **Event triggers** - React to signal changes
   ```
   When door opens → notify
   ```

4. **Diagnostics** - Raw signal inspection
   ```
   Debug: read all HVAC signals
   ```

### Use IFEX Services For:

1. **Multi-signal operations** - Anything coordinating 3+ signals
   ```
   Defrost: mirrors + windshield + fan + monitoring + timeout
   ```

2. **Timed sequences** - "Do X, wait, do Y, monitor Z"
   ```
   Pre-condition cabin: close sunroof → set temp → adjust fan → wait for stable
   ```

3. **Conditional logic** - "If cold outside, then..."
   ```
   Smart defrost: check exterior temp → decide intensity → monitor completion
   ```

4. **Domain semantics** - Human-meaningful concepts
   ```
   "cozy" vs "22 degrees" - service knows context
   ```

5. **Completion tracking** - "Tell me when it's done"
   ```
   Service monitors actual vs target, returns when stable
   ```

6. **Error recovery** - "If this fails, do that"
   ```
   Service handles retries, fallbacks, compensation
   ```

---

## Concrete Example: Climate Control

### Pure VSS Approach

```python
# Caller must know all the details
kuksa.set("Vehicle.Cabin.HVAC.Row1.Left.Temperature.Target", 22)
kuksa.set("Vehicle.Cabin.HVAC.Row1.Right.Temperature.Target", 22)
kuksa.set("Vehicle.Cabin.HVAC.Row2.Left.Temperature.Target", 21)
kuksa.set("Vehicle.Cabin.HVAC.Row2.Right.Temperature.Target", 21)
kuksa.set("Vehicle.Cabin.HVAC.IsAirConditioningActive", True)
kuksa.set("Vehicle.Cabin.HVAC.Fan.Speed", 5)

# Caller must poll for completion
while True:
    actual = kuksa.get("Vehicle.Cabin.HVAC.Row1.Left.Temperature.Actual")
    if abs(actual - 22) < 1:
        break
    sleep(5)
```

**Problems:**
- Caller needs VSS structure knowledge
- No handling of edge cases
- Polling logic duplicated
- No semantic meaning

### IFEX Service Approach

```python
# Caller expresses intent
result = dispatcher.call(
    service="climate-comfort",
    method="set_comfort_level",
    parameters={"level": "cozy", "zones": "all"}
)
# Returns: {accepted: true, estimated_time_seconds: 180}
```

**Inside the IFEX service** (uses VSS):

```python
def set_comfort_level(level, zones):
    # Domain knowledge: map semantic to concrete
    target = {"cozy": 23, "normal": 21, "fresh": 19}[level]

    # Check preconditions via VSS
    if kuksa.get("Vehicle.Cabin.Sunroof.Position") > 0:
        kuksa.set("Vehicle.Cabin.Sunroof.Position", 0)
        wait_for("Vehicle.Cabin.Sunroof.Position", 0)

    # Set targets via VSS
    for zone in expand_zones(zones):
        kuksa.set(f"Vehicle.Cabin.HVAC.{zone}.Temperature.Target", target)

    # Adaptive fan speed based on delta
    actual = kuksa.get("Vehicle.Cabin.HVAC.Row1.Left.Temperature.Actual")
    fan_speed = 5 if abs(actual - target) > 5 else 3
    kuksa.set("Vehicle.Cabin.HVAC.Fan.Speed", fan_speed)

    return {"accepted": True, "estimated_time_seconds": estimate_time(actual, target)}
```

---

## The Relationship in Practice

```
┌─────────────────────────────────────────────────────────────┐
│                    IFEX Service                              │
│                                                              │
│  ┌────────────────────────────────────────────────────────┐ │
│  │              Business Logic                             │ │
│  │  - Semantic operations                                  │ │
│  │  - Coordination & sequencing                            │ │
│  │  - Error handling & recovery                            │ │
│  │  - Domain knowledge                                     │ │
│  └───────────────────────┬────────────────────────────────┘ │
│                          │                                   │
│                   uses VSS for                               │
│                          │                                   │
│  ┌───────────────────────┴────────────────────────────────┐ │
│  │              VSS Operations                             │ │
│  │  - sensor.get() - read current state                    │ │
│  │  - actuator.set() - request state change                │ │
│  │  - subscribe() - react to changes                       │ │
│  └────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────┘
```

**Pattern:** IFEX services consume VSS internally but expose semantic interfaces externally.

---

## Integration with KUKSA

In this platform, VSS access is via KUKSA.val databroker:

```
┌─────────────────┐     ┌─────────────────┐     ┌─────────────────┐
│  IFEX Service   │────▶│  KUKSA.val      │────▶│  Vehicle ECUs   │
│     (C++)       │     │  Databroker     │     │  (ASIL-rated)   │
└─────────────────┘     └─────────────────┘     └─────────────────┘
        │                       │
        │ IFEX methods          │ VSS signals
        │ (semantic)            │ (raw)
        ▼                       ▼
   "set_comfort(cozy)"    "HVAC.Temp.Target=23"
```

---

## Summary Table

| Aspect | VSS | IFEX |
|--------|-----|------|
| **Describes** | State | Capabilities |
| **Granularity** | Individual signals | Operations |
| **Knowledge needed** | Signal paths, types | Method names, parameters |
| **Coordination** | None (single signal) | Multi-signal orchestration |
| **Semantics** | Technical | Domain/human |
| **Primary consumer** | IFEX services, diagnostics | Apps, AI, orchestration |
| **Example** | `HVAC.Temperature.Target = 22` | `set_comfort_level(cozy)` |

---

## The Pitch

> "VSS is the vehicle's nervous system - raw sensory data flowing in, motor commands flowing out.
>
> IFEX is the higher brain function - taking intent and translating it into coordinated action.
>
> You don't ask your brain to 'fire motor neurons 47-52 at 30Hz'. You think 'pick up the cup'. That's the relationship between IFEX and VSS.
>
> VSS gives every OEM, every system, a common vocabulary for vehicle state. IFEX builds on that foundation to create meaningful capabilities that users, apps, and AI can understand and invoke."

---

## Related Documentation

- [Core Services Specification](core-services-spec.md) - Discovery, Dispatcher, Scheduler, Orchestrator
- [IFEX Service Architecture](ifex-service-architecture.md) - How to design IFEX services
