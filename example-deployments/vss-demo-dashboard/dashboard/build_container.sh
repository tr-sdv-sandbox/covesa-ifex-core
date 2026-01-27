#!/bin/bash
# Build the IFEX V2 Dashboard Docker container
#
# This script:
# 1. Copies proto files from covesa-ifex-core
# 2. Builds the Docker image (Docker layer caching handles efficiency)
#
# Usage:
#   ./build_container.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

IMAGE_NAME="ifex-v2-dashboard:latest"
PROTOS_DIR="$SCRIPT_DIR/protos"

# Locate covesa-ifex-core (we're inside it: example-deployments/vss-demo-dashboard/dashboard/)
IFEX_CORE_DIR="$SCRIPT_DIR/../../.."
IFEX_PROTO_DIR="$IFEX_CORE_DIR/proto/ifex-generated"
INTERNAL_PROTO_DIR="$IFEX_CORE_DIR/proto/internal"

# Verify ifex-core exists
if [ ! -d "$IFEX_PROTO_DIR" ]; then
    echo "Error: IFEX proto directory not found: $IFEX_PROTO_DIR"
    echo "Please build covesa-ifex-core first: cd $IFEX_CORE_DIR && ./generate_proto.sh"
    exit 1
fi

# Step 1: Copy proto files
echo "Copying proto files..."
mkdir -p "$PROTOS_DIR/common"
cp "$IFEX_PROTO_DIR/cloud/cloud-backend-transport-service.proto" "$PROTOS_DIR/"
cp "$IFEX_PROTO_DIR/cloud/cloud-discovery-service.proto" "$PROTOS_DIR/"
cp "$IFEX_PROTO_DIR/cloud/cloud-scheduler-service.proto" "$PROTOS_DIR/"
cp "$IFEX_PROTO_DIR/cloud/cloud-scheduler-sync-bridge.proto" "$PROTOS_DIR/"
cp "$IFEX_PROTO_DIR/common/scheduler-types.proto" "$PROTOS_DIR/common/"
cp "$INTERNAL_PROTO_DIR/dispatcher-rpc-envelope.proto" "$PROTOS_DIR/"

# Step 2: Build Docker image (layer caching handles efficiency)
echo "Building Docker image: $IMAGE_NAME"
docker build -t "$IMAGE_NAME" .
echo "Build complete!"
