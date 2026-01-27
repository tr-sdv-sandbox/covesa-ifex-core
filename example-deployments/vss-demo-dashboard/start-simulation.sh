#!/bin/bash
# Start IFEX V2 Simulation (In-Memory Cloud Services)
#
# This script starts a simulation environment using the in-memory
# cloud services from covesa-ifex-core. It does NOT require:
# - Kafka
# - PostgreSQL
#
# Usage: ./start-simulation.sh [num_trucks] [options]
#
# Examples:
#   ./start-simulation.sh              # 10 trucks (default)
#   ./start-simulation.sh 5            # 5 trucks
#   ./start-simulation.sh 20 --clean   # 20 trucks, clean restart
#
# Components started:
# - Mosquitto MQTT broker (Docker, port 1884)
# - Cloud Backend Transport Service (gRPC, port 50100)
# - Cloud Scheduler Service (in-memory, port 50102)
# - Cloud Scheduler Sync Bridge (port 50103)
# - N simulated vehicle containers (ifex-vehicle:latest)

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Defaults
NUM_TRUCKS="${1:-10}"
VEHICLE_IMAGE="ifex-vehicle:latest"
VIN_PREFIX="TRUCK"
NETWORK_NAME="ifex-simulation-v2"
LOG_DIR="/tmp/ifex-v2-logs"

# Port assignments (different from deploy/ to avoid conflicts)
MQTT_PORT=1884
CLOUD_TRANSPORT_PORT=50100
CLOUD_DISCOVERY_PORT=50101
CLOUD_SCHEDULER_PORT=50102
CLOUD_SYNC_BRIDGE_PORT=50103
CLOUD_DISPATCHER_PORT=50104
DASHBOARD_PORT=8080

# Locate ifex-core build directory (we're inside covesa-ifex-core)
IFEX_CORE_DIR="$SCRIPT_DIR/../.."
IFEX_BUILD_DIR="$IFEX_CORE_DIR/build"

# Parse options
shift || true
CLEAN=false
while [[ $# -gt 0 ]]; do
    case $1 in
        --clean)
            CLEAN=true
            shift
            ;;
        --vin-prefix)
            VIN_PREFIX="$2"
            shift 2
            ;;
        *)
            echo "Unknown option: $1"
            exit 1
            ;;
    esac
done

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log_info() { echo -e "${GREEN}[INFO]${NC} $1"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
log_error() { echo -e "${RED}[ERROR]${NC} $1"; }
log_step() { echo -e "${BLUE}[STEP]${NC} $1"; }

# Check prerequisites
check_prerequisites() {
    log_step "Checking prerequisites..."

    if ! command -v docker &> /dev/null; then
        log_error "Docker is not installed"
        exit 1
    fi

    if [ ! -d "$IFEX_BUILD_DIR" ]; then
        log_error "IFEX core build directory not found: $IFEX_BUILD_DIR"
        log_error "Please build covesa-ifex-core first:"
        log_error "  cd $IFEX_CORE_DIR && ./build.sh --debug --test"
        exit 1
    fi

    # Check required cloud service binaries
    local required_bins=(
        "reference-services/backend-transport/cloud/service/ifex-cloud-backend-transport-service"
        "reference-services/discovery/cloud/service/ifex-cloud-discovery-service"
        "reference-services/scheduler/cloud/service/ifex-cloud-scheduler-service"
        "reference-services/scheduler/cloud/sync-bridge/ifex-cloud-scheduler-sync-bridge"
        "reference-services/dispatcher/cloud/service/ifex-cloud-dispatcher-service"
    )

    for bin in "${required_bins[@]}"; do
        if [ ! -x "$IFEX_BUILD_DIR/$bin" ]; then
            log_error "Binary not found: $IFEX_BUILD_DIR/$bin"
            log_error "Please rebuild covesa-ifex-core with tests enabled:"
            log_error "  cd $IFEX_CORE_DIR && ./build.sh --debug --test"
            exit 1
        fi
    done

    log_info "All prerequisites satisfied"
}

# Setup logs and network
setup_environment() {
    mkdir -p "$LOG_DIR"
    log_info "Logs will be written to $LOG_DIR"

    # Create Docker network if needed
    if ! docker network inspect "$NETWORK_NAME" >/dev/null 2>&1; then
        log_info "Creating Docker network: $NETWORK_NAME"
        docker network create "$NETWORK_NAME"
    fi
}

# Stop any existing simulation
stop_existing() {
    log_step "Stopping any existing simulation..."
    ./stop-simulation.sh 2>/dev/null || true
    sleep 1
}

# Start MQTT broker
start_mqtt() {
    log_step "Starting MQTT broker (port $MQTT_PORT)..."

    # Use docker compose based on availability
    if docker compose version &> /dev/null 2>&1; then
        docker compose up -d
    else
        docker-compose up -d
    fi

    # Connect MQTT container to simulation network
    docker network connect "$NETWORK_NAME" ifex-mosquitto-v2 2>/dev/null || true

    # Wait for MQTT to be ready
    for i in {1..30}; do
        if nc -z localhost $MQTT_PORT 2>/dev/null; then
            log_info "MQTT broker is ready"
            return 0
        fi
        sleep 0.5
    done

    log_error "MQTT broker failed to start"
    return 1
}

# Start cloud backend transport
start_cloud_transport() {
    log_step "Starting cloud backend transport (port $CLOUD_TRANSPORT_PORT)..."

    "$IFEX_BUILD_DIR/reference-services/backend-transport/cloud/service/ifex-cloud-backend-transport-service" \
        --listen="0.0.0.0:$CLOUD_TRANSPORT_PORT" \
        --mqtt_host=localhost \
        --mqtt_port=$MQTT_PORT \
        > "$LOG_DIR/cloud-transport.log" 2>&1 &

    echo $! > "$LOG_DIR/cloud-transport.pid"

    # Wait for service to be ready
    for i in {1..30}; do
        if nc -z localhost $CLOUD_TRANSPORT_PORT 2>/dev/null; then
            log_info "Cloud transport service started (PID: $(cat "$LOG_DIR/cloud-transport.pid"))"
            return 0
        fi
        sleep 0.5
    done

    log_error "Cloud transport service failed to start"
    cat "$LOG_DIR/cloud-transport.log"
    return 1
}

# Start cloud discovery service
start_cloud_discovery() {
    log_step "Starting cloud discovery service (port $CLOUD_DISCOVERY_PORT)..."

    "$IFEX_BUILD_DIR/reference-services/discovery/cloud/service/ifex-cloud-discovery-service" \
        --listen="0.0.0.0:$CLOUD_DISCOVERY_PORT" \
        --transport="localhost:$CLOUD_TRANSPORT_PORT" \
        --content_id=201 \
        > "$LOG_DIR/cloud-discovery.log" 2>&1 &

    echo $! > "$LOG_DIR/cloud-discovery.pid"

    # Wait for service to be ready
    for i in {1..30}; do
        if nc -z localhost $CLOUD_DISCOVERY_PORT 2>/dev/null; then
            log_info "Cloud discovery service started (PID: $(cat "$LOG_DIR/cloud-discovery.pid"))"
            return 0
        fi
        sleep 0.5
    done

    log_error "Cloud discovery service failed to start"
    cat "$LOG_DIR/cloud-discovery.log"
    return 1
}

# Start cloud scheduler service
start_cloud_scheduler() {
    log_step "Starting cloud scheduler service (port $CLOUD_SCHEDULER_PORT)..."

    "$IFEX_BUILD_DIR/reference-services/scheduler/cloud/service/ifex-cloud-scheduler-service" \
        --listen="0.0.0.0:$CLOUD_SCHEDULER_PORT" \
        > "$LOG_DIR/cloud-scheduler.log" 2>&1 &

    echo $! > "$LOG_DIR/cloud-scheduler.pid"

    # Wait for service to be ready
    for i in {1..30}; do
        if nc -z localhost $CLOUD_SCHEDULER_PORT 2>/dev/null; then
            log_info "Cloud scheduler service started (PID: $(cat "$LOG_DIR/cloud-scheduler.pid"))"
            return 0
        fi
        sleep 0.5
    done

    log_error "Cloud scheduler service failed to start"
    cat "$LOG_DIR/cloud-scheduler.log"
    return 1
}

# Start cloud scheduler sync bridge
start_cloud_sync_bridge() {
    log_step "Starting cloud scheduler sync bridge (port $CLOUD_SYNC_BRIDGE_PORT)..."

    "$IFEX_BUILD_DIR/reference-services/scheduler/cloud/sync-bridge/ifex-cloud-scheduler-sync-bridge" \
        --listen="0.0.0.0:$CLOUD_SYNC_BRIDGE_PORT" \
        --scheduler="localhost:$CLOUD_SCHEDULER_PORT" \
        --transport="localhost:$CLOUD_TRANSPORT_PORT" \
        --content_id=202 \
        > "$LOG_DIR/cloud-sync-bridge.log" 2>&1 &

    echo $! > "$LOG_DIR/cloud-sync-bridge.pid"

    # Wait for service to be ready
    for i in {1..30}; do
        if nc -z localhost $CLOUD_SYNC_BRIDGE_PORT 2>/dev/null; then
            log_info "Cloud sync bridge started (PID: $(cat "$LOG_DIR/cloud-sync-bridge.pid"))"
            return 0
        fi
        sleep 0.5
    done

    log_error "Cloud sync bridge failed to start"
    cat "$LOG_DIR/cloud-sync-bridge.log"
    return 1
}

# Start cloud dispatcher service
start_cloud_dispatcher() {
    log_step "Starting cloud dispatcher service (port $CLOUD_DISPATCHER_PORT)..."

    "$IFEX_BUILD_DIR/reference-services/dispatcher/cloud/service/ifex-cloud-dispatcher-service" \
        --listen="0.0.0.0:$CLOUD_DISPATCHER_PORT" \
        --transport="localhost:$CLOUD_TRANSPORT_PORT" \
        --content_id=200 \
        --default_timeout=30000 \
        > "$LOG_DIR/cloud-dispatcher.log" 2>&1 &

    echo $! > "$LOG_DIR/cloud-dispatcher.pid"

    # Wait for service to be ready
    for i in {1..30}; do
        if nc -z localhost $CLOUD_DISPATCHER_PORT 2>/dev/null; then
            log_info "Cloud dispatcher service started (PID: $(cat "$LOG_DIR/cloud-dispatcher.pid"))"
            return 0
        fi
        sleep 0.5
    done

    log_error "Cloud dispatcher service failed to start"
    cat "$LOG_DIR/cloud-dispatcher.log"
    return 1
}

# Start vehicle containers
start_vehicles() {
    log_step "Starting $NUM_TRUCKS vehicle containers..."

    for i in $(seq 1 $NUM_TRUCKS); do
        # Generate vehicle ID with prefix (e.g., TRUCK00001)
        VIN=$(printf "%s%05d" "$VIN_PREFIX" "$i")
        CONTAINER_NAME="ifex-v2-vehicle-$i"

        log_info "  Starting vehicle $i: $VIN"

        docker run -d \
            --name "$CONTAINER_NAME" \
            --network "$NETWORK_NAME" \
            -e VEHICLE_ID="$VIN" \
            -e MQTT_HOST="ifex-mosquitto-v2" \
            -e MQTT_PORT="1883" \
            -e START_TEST_SERVICES=true \
            "$VEHICLE_IMAGE" >/dev/null

        # Small delay to avoid overwhelming the broker
        sleep 0.3
    done

    log_info "All $NUM_TRUCKS vehicles started"
}

# Wait for vehicles to register
wait_for_vehicles() {
    log_step "Waiting for vehicles to register services..."
    sleep 5
}

# Build vehicle image (delegates to build-test-container.sh)
build_vehicle_image() {
    log_step "Building vehicle container image..."
    "$SCRIPT_DIR/build-test-container.sh"
}

# Build dashboard image if needed (delegates to dashboard/build_container.sh)
build_dashboard_image() {
    log_info "Building dashboard image..."
    "$SCRIPT_DIR/dashboard/build_container.sh"
}

# Start dashboard
start_dashboard() {
    log_step "Starting dashboard container (port $DASHBOARD_PORT)..."

    build_dashboard_image

    # Run dashboard container
    # Use host.docker.internal on Mac/Windows, or host network on Linux
    if [[ "$(uname)" == "Linux" ]]; then
        docker run -d \
            --name "ifex-v2-dashboard" \
            --network host \
            -e SCHEDULER_HOST=localhost \
            -e SCHEDULER_PORT=$CLOUD_SCHEDULER_PORT \
            -e SYNC_BRIDGE_HOST=localhost \
            -e SYNC_BRIDGE_PORT=$CLOUD_SYNC_BRIDGE_PORT \
            -e TRANSPORT_HOST=localhost \
            -e TRANSPORT_PORT=$CLOUD_TRANSPORT_PORT \
            -e DISCOVERY_HOST=localhost \
            -e DISCOVERY_PORT=$CLOUD_DISCOVERY_PORT \
            -e DASHBOARD_PORT=$DASHBOARD_PORT \
            ifex-v2-dashboard:latest >/dev/null
    else
        docker run -d \
            --name "ifex-v2-dashboard" \
            -p $DASHBOARD_PORT:$DASHBOARD_PORT \
            -e SCHEDULER_HOST=host.docker.internal \
            -e SCHEDULER_PORT=$CLOUD_SCHEDULER_PORT \
            -e SYNC_BRIDGE_HOST=host.docker.internal \
            -e SYNC_BRIDGE_PORT=$CLOUD_SYNC_BRIDGE_PORT \
            -e TRANSPORT_HOST=host.docker.internal \
            -e TRANSPORT_PORT=$CLOUD_TRANSPORT_PORT \
            -e DISCOVERY_HOST=host.docker.internal \
            -e DISCOVERY_PORT=$CLOUD_DISCOVERY_PORT \
            -e DASHBOARD_PORT=$DASHBOARD_PORT \
            ifex-v2-dashboard:latest >/dev/null
    fi

    # Wait for dashboard to be ready
    for i in {1..30}; do
        if nc -z localhost $DASHBOARD_PORT 2>/dev/null; then
            log_info "Dashboard container started"
            return 0
        fi
        sleep 0.5
    done

    log_warn "Dashboard may not be fully ready (check: docker logs ifex-v2-dashboard)"
    return 0
}

# Print summary
print_summary() {
    echo ""
    echo "=========================================="
    echo " IFEX V2 Simulation Environment Started"
    echo "=========================================="
    echo ""
    echo "Dashboard:    http://localhost:$DASHBOARD_PORT"
    echo ""
    echo "Infrastructure (In-Memory):"
    echo "  MQTT Broker:           localhost:$MQTT_PORT"
    echo "  Cloud Transport:       localhost:$CLOUD_TRANSPORT_PORT"
    echo "  Cloud Discovery:       localhost:$CLOUD_DISCOVERY_PORT"
    echo "  Cloud Scheduler:       localhost:$CLOUD_SCHEDULER_PORT"
    echo "  Cloud Sync Bridge:     localhost:$CLOUD_SYNC_BRIDGE_PORT"
    echo "  Cloud Dispatcher:      localhost:$CLOUD_DISPATCHER_PORT"
    echo ""
    echo "Vehicles: $NUM_TRUCKS"
    echo "  Container prefix:      ifex-v2-vehicle-"
    echo "  VIN Range:             ${VIN_PREFIX}00001 - ${VIN_PREFIX}$(printf '%05d' $NUM_TRUCKS)"
    echo ""
    echo "Logs: $LOG_DIR/"
    echo ""
    echo "Quick Start:"
    echo "  1. Open http://localhost:$DASHBOARD_PORT in your browser"
    echo "  2. Go to Calendar tab and select a vehicle"
    echo "  3. Click + New Job or click a time slot to schedule"
    echo ""
    echo "CLI Commands:"
    echo "  # Monitor MQTT traffic"
    echo "  mosquitto_sub -h localhost -p $MQTT_PORT -t '#' -v"
    echo ""
    echo "  # View vehicle logs"
    echo "  docker logs -f ifex-v2-vehicle-1"
    echo ""
    echo "To stop: ./stop-simulation.sh"
    echo ""
}

# Main
main() {
    echo ""
    echo "Starting IFEX V2 Simulation..."
    echo "  Trucks: $NUM_TRUCKS"
    echo "  VIN Pattern: ${VIN_PREFIX}00001 - ${VIN_PREFIX}$(printf '%05d' $NUM_TRUCKS)"
    echo ""

    check_prerequisites
    setup_environment

    if [ "$CLEAN" = true ]; then
        stop_existing
    fi

    build_vehicle_image
    start_mqtt
    start_cloud_transport
    start_cloud_discovery
    start_cloud_scheduler
    start_cloud_sync_bridge
    start_cloud_dispatcher
    start_vehicles
    wait_for_vehicles
    start_dashboard
    print_summary
}

main "$@"
