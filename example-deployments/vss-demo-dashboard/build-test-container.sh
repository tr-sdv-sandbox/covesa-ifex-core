#!/bin/bash
# Build the ifex-vehicle Docker image for E2E testing
#
# This image contains all vehicle-side IFEX services including:
#   - Discovery, Dispatcher, Scheduler, Backend Transport
#   - Sync bridges (discovery, scheduler, dispatcher)
#   - Test services (echo, beverage, climate-comfort, defrost)
#
# Usage:
#   ./build-test-container.sh           # Build binaries + image
#   ./build-test-container.sh --clean   # Clean rebuild
#
# IMPORTANT: Run this whenever you change vehicle-side code!
# The E2E tests in covesa-ifex-offboard-services depend on this image.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$PROJECT_ROOT"

echo "============================================"
echo "Building ifex-vehicle:latest Docker image"
echo "============================================"
echo ""

# Build binaries first
echo "[INFO] Building IFEX services..."
if [ "$1" = "--clean" ]; then
    ./build.sh --clean
else
    ./build.sh
fi
echo ""

# Build Docker image
echo "[INFO] Building Docker image..."
docker build -f example-deployments/vss-demo-dashboard/vehicle-docker/Dockerfile.vehicle -t ifex-vehicle:latest .

echo ""
echo "============================================"
echo "SUCCESS: ifex-vehicle:latest built"
echo "============================================"
