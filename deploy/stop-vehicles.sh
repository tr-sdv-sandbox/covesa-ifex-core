#!/bin/bash
# Stop all simulated IFEX vehicles
# Usage: ./stop-vehicles.sh

set -e

echo "[INFO] Stopping all IFEX vehicle containers..."

# Find and stop all ifex-vehicle containers
CONTAINERS=$(docker ps -aq --filter "name=ifex-vehicle-" 2>/dev/null || true)

if [ -z "$CONTAINERS" ]; then
    echo "[INFO] No vehicle containers running"
    exit 0
fi

docker rm -f $CONTAINERS >/dev/null 2>&1 || true

echo "[INFO] Stopped $(echo $CONTAINERS | wc -w) vehicle containers"
