# Departure Orchestration Example

## Scenario: Morning Departure at 7:30 AM

User request via GUI:
- Departure time: 7:30 AM
- Comfort temperature: 22°C  
- Coffee: Yes (medium strength cappuccino)
- Current conditions: 2°C outside, frost on windshield

## Timeline Calculation (working backwards from 7:30)

### 7:30 - Departure Time
- All systems ready
- Coffee in cup holder
- Cabin at comfortable 22°C
- Windows clear

### 7:28 - Coffee Ready (T-2 min)
- `beverage_service.prepare_beverage()` completes
- Coffee at optimal temperature (85°C)

### 7:25 - Start Coffee Brewing (T-5 min)
- `beverage_service.prepare_beverage({type: CAPPUCCINO, strength: 2})`
- 3 minute brew time

### 7:22 - Cabin Comfort Achieved (T-8 min)
- `climate_comfort_service` reaches target
- All zones at 22°C

### 7:18 - Windows Clear (T-12 min)
- `defrost_service` completes
- Visibility safe for driving

### 7:15 - Start Climate Comfort (T-15 min)
- `climate_comfort_service.set_comfort({mode: COMFORT, target: 22})`
- Coordinates HVAC across all zones

### 7:10 - Start Defrost (T-20 min)
- `defrost_service.start_defrost({mode: ALL_WINDOWS})`
- Priority on windshield first

### 7:08 - Battery Preconditioning (T-22 min)
- For EVs: warm battery for optimal range
- Coordinate with climate to share heating

## Service Interactions

```yaml
# Departure orchestration service coordinates:
departure_orchestration_service.schedule_departure({
  departure_time: 1704700200,  # 7:30 AM
  comfort_prefs: {
    cabin_temperature_celsius: 22.0,
    seat_temperature_level: 2,
    steering_wheel_heating: true
  },
  beverage_request: {
    prepare_beverage: true,
    beverage_type: "CAPPUCCINO",
    strength: 2
  },
  auto_defrost: true,
  precondition_battery: true
})

# Returns timeline:
[
  {step: BATTERY_PRECONDITIONING, start: "7:08", duration: 900},
  {step: DEFROST_START, start: "7:10", duration: 480},  
  {step: CLIMATE_START, start: "7:15", duration: 420},
  {step: BEVERAGE_START, start: "7:25", duration: 180}
]
```

## GUI Display

The GUI would show:
- Timeline visualization with progress bars
- Current status of each system
- Ability to modify/cancel
- Push notification when ready
- Energy usage estimate

## Energy Optimization

The orchestration service optimizes by:
- Sharing heat between battery and cabin heating
- Starting high-power operations while plugged in
- Balancing comfort vs efficiency based on mode
- Stopping defrost early if sensors show clear

## Error Handling

If coffee maker reports error:
- Continue with other preparations
- Notify user via app
- Update timeline removing coffee step
- Still achieve cabin comfort for departure