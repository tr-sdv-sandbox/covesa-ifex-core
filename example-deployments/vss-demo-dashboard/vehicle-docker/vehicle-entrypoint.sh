#!/bin/bash
# IFEX Vehicle Simulation Entrypoint
# Starts onboard services and connects to MQTT broker

set -e

# Colors
log_info() { echo "[INFO] $1"; }
log_error() { echo "[ERROR] $1" >&2; }

log_info "Starting IFEX Vehicle Simulation"
log_info "  VEHICLE_ID: $VEHICLE_ID"
log_info "  MQTT_HOST: $MQTT_HOST:$MQTT_PORT"

# Wait for MQTT broker
log_info "Waiting for MQTT broker..."
for i in {1..30}; do
    if nc -z "$MQTT_HOST" "$MQTT_PORT" 2>/dev/null; then
        log_info "MQTT broker available"
        break
    fi
    if [ $i -eq 30 ]; then
        log_error "MQTT broker not available after 30s"
        exit 1
    fi
    sleep 1
done

# Create log directory
mkdir -p /app/logs

# Start Discovery Service
log_info "Starting Discovery Service..."
/app/bin/ifex-discovery-service \
    --listen=0.0.0.0:$DISCOVERY_PORT \
    > /app/logs/discovery.log 2>&1 &
DISCOVERY_PID=$!
sleep 1

# Verify Discovery is running
if ! kill -0 $DISCOVERY_PID 2>/dev/null; then
    log_error "Discovery Service failed to start"
    cat /app/logs/discovery.log
    exit 1
fi

# Start Scheduler Service
log_info "Starting Scheduler Service..."
/app/bin/ifex-scheduler-service \
    --listen=0.0.0.0:$SCHEDULER_PORT \
    --discovery=localhost:$DISCOVERY_PORT \
    --persistence-dir=/app/data \
    --ifex-schema=/app/ifex/scheduler-service.ifex.yml \
    > /app/logs/scheduler.log 2>&1 &
SCHEDULER_PID=$!
sleep 1

# Start Backend Transport Service (connects to MQTT)
log_info "Starting Backend Transport Service..."
MQTT_HOST=$MQTT_HOST \
MQTT_PORT=$MQTT_PORT \
VEHICLE_ID=$VEHICLE_ID \
/app/bin/ifex-backend-transport-service \
    --listen=0.0.0.0:$BACKEND_TRANSPORT_PORT \
    --discovery=localhost:$DISCOVERY_PORT \
    > /app/logs/backend-transport.log 2>&1 &
BACKEND_PID=$!
sleep 1

# Verify Backend Transport is running
if ! kill -0 $BACKEND_PID 2>/dev/null; then
    log_error "Backend Transport Service failed to start"
    cat /app/logs/backend-transport.log
    exit 1
fi

# Start Dispatcher Service
log_info "Starting Dispatcher Service..."
/app/bin/ifex-dispatcher-service \
    --listen=0.0.0.0:$DISPATCHER_PORT \
    --discovery=localhost:$DISCOVERY_PORT \
    --ifex-schema=/app/ifex/dispatcher-service.ifex.yml \
    > /app/logs/dispatcher.log 2>&1 &
DISPATCHER_PID=$!
sleep 1

# Start Sync Bridges (these publish to MQTT via Backend Transport)
log_info "Starting Discovery Sync Bridge..."
/app/bin/ifex-discovery-sync-bridge \
    --discovery=localhost:$DISCOVERY_PORT \
    --backend-transport=localhost:$BACKEND_TRANSPORT_PORT \
    --content-id=201 \
    --vehicle-id=$VEHICLE_ID \
    > /app/logs/discovery-sync-bridge.log 2>&1 &
sleep 0.5

log_info "Starting Scheduler Sync Bridge..."
/app/bin/ifex-scheduler-sync-bridge \
    --scheduler=localhost:$SCHEDULER_PORT \
    --backend-transport=localhost:$BACKEND_TRANSPORT_PORT \
    --content-id=202 \
    --vehicle_id=$VEHICLE_ID \
    > /app/logs/scheduler-sync-bridge.log 2>&1 &
sleep 0.5

log_info "Starting Dispatcher Bridge..."
/app/bin/ifex-dispatcher-bridge \
    --dispatcher=localhost:$DISPATCHER_PORT \
    --backend-transport=localhost:$BACKEND_TRANSPORT_PORT \
    --content-id=200 \
    > /app/logs/dispatcher-bridge.log 2>&1 &
sleep 0.5

# Optionally start test services for richer simulation
if [ "${START_TEST_SERVICES:-true}" = "true" ]; then
    log_info "Starting test services..."

    if [ -x /app/bin/ifex-echo-service ]; then
        /app/bin/ifex-echo-service \
            --listen=0.0.0.0:50064 \
            --discovery=localhost:$DISCOVERY_PORT \
            --ifex-schema=/app/ifex/echo_service.ifex.yml \
            > /app/logs/echo.log 2>&1 &
        log_info "  Echo service started on port 50064"
    fi

    if [ -x /app/bin/beverage-service ]; then
        /app/bin/beverage-service \
            --listen=0.0.0.0:50061 \
            --discovery=localhost:$DISCOVERY_PORT \
            --ifex-schema=/app/ifex/beverage-service.ifex.yml \
            > /app/logs/beverage.log 2>&1 &
    fi

    if [ -x /app/bin/climate-comfort-service ]; then
        /app/bin/climate-comfort-service \
            --listen=0.0.0.0:50062 \
            --discovery=localhost:$DISCOVERY_PORT \
            --ifex-schema=/app/ifex/climate-comfort-service.ifex.yml \
            > /app/logs/climate-comfort.log 2>&1 &
    fi

    if [ -x /app/bin/defrost-service ]; then
        /app/bin/defrost-service \
            --listen=0.0.0.0:50063 \
            --ifex-schema=/app/ifex/defrost-service.ifex.yml \
            --discovery=localhost:$DISCOVERY_PORT \
            > /app/logs/defrost.log 2>&1 &
    fi
fi

log_info "IFEX Vehicle $VEHICLE_ID ready"
log_info "Services:"
log_info "  Discovery:         localhost:$DISCOVERY_PORT"
log_info "  Dispatcher:        localhost:$DISPATCHER_PORT"
log_info "  Scheduler:         localhost:$SCHEDULER_PORT"
log_info "  Backend Transport: localhost:$BACKEND_TRANSPORT_PORT"

# Handle shutdown gracefully
cleanup() {
    log_info "Shutting down..."
    kill $BACKEND_PID $DISPATCHER_PID $SCHEDULER_PID $DISCOVERY_PID 2>/dev/null || true
    wait
    log_info "Goodbye"
}
trap cleanup SIGTERM SIGINT

# Keep container running and tail logs
tail -f /app/logs/*.log &
wait
