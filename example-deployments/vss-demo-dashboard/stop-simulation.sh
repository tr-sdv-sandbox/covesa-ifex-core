#!/bin/bash
# Stop IFEX V2 Simulation
#
# Stops all components started by start-simulation.sh:
# - Vehicle containers
# - Cloud services (native processes)
# - MQTT broker (Docker)

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

LOG_DIR="/tmp/ifex-v2-logs"
NETWORK_NAME="ifex-simulation-v2"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log_info() { echo -e "${GREEN}[INFO]${NC} $1"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }

# Stop a process by PID file
stop_process() {
    local pid_file="$1"
    local name="$2"

    if [ -f "$pid_file" ]; then
        local pid=$(cat "$pid_file")
        if kill -0 "$pid" 2>/dev/null; then
            log_info "Stopping $name (PID: $pid)..."
            kill "$pid" 2>/dev/null || true
            sleep 0.3
            # Force kill if still running
            if kill -0 "$pid" 2>/dev/null; then
                kill -9 "$pid" 2>/dev/null || true
            fi
        fi
        rm -f "$pid_file"
    fi
}

# Stop all vehicle containers
stop_vehicles() {
    log_info "Stopping vehicle containers..."

    # Find all v2 vehicle containers
    local containers=$(docker ps -aq --filter "name=ifex-v2-vehicle-" 2>/dev/null)

    if [ -n "$containers" ]; then
        local count=$(echo "$containers" | wc -l)
        log_info "  Stopping $count vehicle containers in parallel..."
        # Stop all at once with 1 second timeout (default is 10s)
        echo "$containers" | xargs -r docker stop -t 1 >/dev/null 2>&1 || true
        # Remove all at once
        echo "$containers" | xargs -r docker rm >/dev/null 2>&1 || true
        log_info "  Done"
    else
        log_info "  No vehicle containers found"
    fi
}

# Stop dashboard container
stop_dashboard() {
    log_info "Stopping dashboard container..."
    docker stop -t 1 ifex-v2-dashboard >/dev/null 2>&1 || true
    docker rm ifex-v2-dashboard >/dev/null 2>&1 || true
}

# Stop cloud services
stop_cloud_services() {
    log_info "Stopping cloud services..."

    stop_process "$LOG_DIR/cloud-dispatcher.pid" "cloud-dispatcher"
    stop_process "$LOG_DIR/cloud-sync-bridge.pid" "cloud-sync-bridge"
    stop_process "$LOG_DIR/cloud-scheduler.pid" "cloud-scheduler"
    stop_process "$LOG_DIR/cloud-discovery.pid" "cloud-discovery"
    stop_process "$LOG_DIR/cloud-transport.pid" "cloud-transport"
}

# Stop MQTT container
stop_mqtt() {
    log_info "Stopping MQTT broker..."

    if docker compose version &> /dev/null 2>&1; then
        docker compose down 2>/dev/null || true
    else
        docker-compose down 2>/dev/null || true
    fi
}

# Cleanup Docker network
cleanup_network() {
    if docker network inspect "$NETWORK_NAME" >/dev/null 2>&1; then
        log_info "Removing Docker network: $NETWORK_NAME"
        docker network rm "$NETWORK_NAME" 2>/dev/null || true
    fi
}

# Kill any remaining processes
cleanup_orphans() {
    log_info "Cleaning up orphan processes..."

    # Kill any remaining ifex cloud processes
    pkill -f "ifex-cloud-backend-transport-service" 2>/dev/null || true
    pkill -f "ifex-cloud-discovery-service" 2>/dev/null || true
    pkill -f "ifex-cloud-scheduler-service" 2>/dev/null || true
    pkill -f "ifex-cloud-scheduler-sync-bridge" 2>/dev/null || true
    pkill -f "ifex-cloud-dispatcher-service" 2>/dev/null || true
}

# Main
main() {
    echo ""
    echo "Stopping IFEX V2 Simulation..."
    echo ""

    stop_vehicles
    stop_dashboard
    stop_cloud_services
    stop_mqtt
    cleanup_orphans
    cleanup_network

    echo ""
    log_info "Simulation stopped"
    echo ""
}

main "$@"
