#!/bin/bash
# Build IFEX Vehicle Docker Image
# Usage: ./build-image.sh [--clean]

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR/.."

# Parse args
CLEAN=false
if [ "$1" = "--clean" ]; then
    CLEAN=true
fi

echo "[INFO] Building IFEX Vehicle Docker Image"

# Build the project first if needed
if [ ! -d "build" ] || [ "$CLEAN" = true ]; then
    echo "[INFO] Building IFEX services..."
    if [ "$CLEAN" = true ]; then
        ./build.sh --clean
    else
        ./build.sh
    fi
fi

# Verify required binaries exist
REQUIRED_BINS=(
    "build/reference-services/discovery/ifex-discovery-service"
    "build/reference-services/dispatcher/ifex-dispatcher-service"
    "build/reference-services/scheduler/ifex-scheduler-service"
    "build/reference-services/backend-transport/ifex-backend-transport-service"
)

for bin in "${REQUIRED_BINS[@]}"; do
    if [ ! -x "$bin" ]; then
        echo "[ERROR] Required binary not found: $bin"
        echo "[ERROR] Run ./build.sh first"
        exit 1
    fi
done

# Build Docker image
echo "[INFO] Building Docker image..."
docker build -f docker/Dockerfile.vehicle -t ifex-vehicle:latest .

echo "[INFO] Image built: ifex-vehicle:latest"
echo ""
echo "Run vehicles with:"
echo "  ./deploy/start-vehicles.sh 5 --mqtt-host <broker>"
