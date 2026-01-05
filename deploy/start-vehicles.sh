#!/bin/bash
# Start Simulated IFEX Vehicles
# Usage: ./start-vehicles.sh [num_vehicles] [--mqtt-host HOST]
#
# Examples:
#   ./start-vehicles.sh 5                           # Start 5 vehicles, MQTT on localhost
#   ./start-vehicles.sh 10 --mqtt-host 192.168.1.10 # Start 10 vehicles, external MQTT
#   ./start-vehicles.sh 3 --vin-prefix TRUCK        # VINs: TRUCK00000000001, etc.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR/.."

# Defaults
NUM_VEHICLES="${1:-3}"
MQTT_HOST="host.docker.internal"
MQTT_PORT="1883"
VIN_PREFIX="VIN"
NETWORK_NAME="ifex-network"
IMAGE_NAME="ifex-vehicle:latest"

# Parse arguments
shift || true
while [[ $# -gt 0 ]]; do
    case $1 in
        --mqtt-host)
            MQTT_HOST="$2"
            shift 2
            ;;
        --mqtt-port)
            MQTT_PORT="$2"
            shift 2
            ;;
        --vin-prefix)
            VIN_PREFIX="$2"
            shift 2
            ;;
        --network)
            NETWORK_NAME="$2"
            shift 2
            ;;
        *)
            echo "Unknown option: $1"
            exit 1
            ;;
    esac
done

# Colors
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'
log_info() { echo -e "${GREEN}[INFO]${NC} $1"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }

log_info "Starting $NUM_VEHICLES simulated IFEX vehicles"
log_info "  MQTT Broker: $MQTT_HOST:$MQTT_PORT"
log_info "  VIN Prefix: $VIN_PREFIX"

# Check if image exists
if ! docker image inspect "$IMAGE_NAME" >/dev/null 2>&1; then
    log_warn "Image $IMAGE_NAME not found. Building..."
    docker build -f docker/Dockerfile.vehicle -t "$IMAGE_NAME" .
fi

# Create network if needed
if ! docker network inspect "$NETWORK_NAME" >/dev/null 2>&1; then
    log_info "Creating Docker network: $NETWORK_NAME"
    docker network create "$NETWORK_NAME" 2>/dev/null || true
fi

# Start vehicles
for i in $(seq 1 $NUM_VEHICLES); do
    # Generate VIN with zero-padding (14 chars to match test data format)
    VIN=$(printf "%s%014d" "$VIN_PREFIX" "$i")
    CONTAINER_NAME="ifex-vehicle-$i"

    # Remove existing container if present
    docker rm -f "$CONTAINER_NAME" 2>/dev/null || true

    log_info "Starting vehicle $i: $VIN"
    docker run -d \
        --name "$CONTAINER_NAME" \
        --network "$NETWORK_NAME" \
        -e VEHICLE_ID="$VIN" \
        -e MQTT_HOST="$MQTT_HOST" \
        -e MQTT_PORT="$MQTT_PORT" \
        -e START_TEST_SERVICES=true \
        "$IMAGE_NAME" >/dev/null

    # Small delay between starts to avoid port conflicts
    sleep 0.5
done

log_info "Started $NUM_VEHICLES vehicles"
echo ""
echo "Vehicles:"
for i in $(seq 1 $NUM_VEHICLES); do
    VIN=$(printf "%s%014d" "$VIN_PREFIX" "$i")
    echo "  - ifex-vehicle-$i ($VIN)"
done
echo ""
echo "Monitor: docker logs -f ifex-vehicle-1"
echo "Stop:    ./stop-vehicles.sh"
