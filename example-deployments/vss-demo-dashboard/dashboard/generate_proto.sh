#!/bin/bash
# Generate Python protobuf stubs for the V2 dashboard (local development)
#
# This script does the same thing as the Docker build:
# 1. Copies proto files from covesa-ifex-core to ./protos/
# 2. Generates Python gRPC stubs in ./proto_gen/
#
# Prerequisites:
#   pip install grpcio-tools
#
# Usage:
#   ./generate_proto.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROTOS_DIR="$SCRIPT_DIR/protos"
OUTPUT_DIR="$SCRIPT_DIR/proto_gen"

# Locate covesa-ifex-core proto directories (we're inside it)
IFEX_CORE_DIR="$SCRIPT_DIR/../../.."
IFEX_PROTO_DIR="$IFEX_CORE_DIR/proto/ifex-generated"
INTERNAL_PROTO_DIR="$IFEX_CORE_DIR/proto/internal"

# Verify directory exists
if [ ! -d "$IFEX_PROTO_DIR" ]; then
    echo "Error: IFEX proto directory not found: $IFEX_PROTO_DIR"
    echo "Please build covesa-ifex-core first: cd $IFEX_CORE_DIR && ./generate_proto.sh"
    exit 1
fi

# Step 1: Copy proto files (same as start-simulation.sh does)
echo "Copying proto files..."
mkdir -p "$PROTOS_DIR/common"
cp "$IFEX_PROTO_DIR/cloud/cloud-backend-transport-service.proto" "$PROTOS_DIR/"
cp "$IFEX_PROTO_DIR/cloud/cloud-scheduler-service.proto" "$PROTOS_DIR/"
cp "$IFEX_PROTO_DIR/cloud/cloud-discovery-service.proto" "$PROTOS_DIR/"
cp "$IFEX_PROTO_DIR/common/scheduler-types.proto" "$PROTOS_DIR/common/"
cp "$INTERNAL_PROTO_DIR/dispatcher-rpc-envelope.proto" "$PROTOS_DIR/"
echo "  Copied to: $PROTOS_DIR"

# Step 2: Generate Python stubs (same as Dockerfile does)
echo "Generating Python proto stubs..."
mkdir -p "$OUTPUT_DIR"

python3 -m grpc_tools.protoc \
    -I"$PROTOS_DIR" \
    --python_out="$OUTPUT_DIR" \
    --grpc_python_out="$OUTPUT_DIR" \
    "$PROTOS_DIR/cloud-backend-transport-service.proto"

python3 -m grpc_tools.protoc \
    -I"$PROTOS_DIR" \
    --python_out="$OUTPUT_DIR" \
    --grpc_python_out="$OUTPUT_DIR" \
    "$PROTOS_DIR/common/scheduler-types.proto"

python3 -m grpc_tools.protoc \
    -I"$PROTOS_DIR" \
    --python_out="$OUTPUT_DIR" \
    --grpc_python_out="$OUTPUT_DIR" \
    "$PROTOS_DIR/cloud-scheduler-service.proto"

python3 -m grpc_tools.protoc \
    -I"$PROTOS_DIR" \
    --python_out="$OUTPUT_DIR" \
    --grpc_python_out="$OUTPUT_DIR" \
    "$PROTOS_DIR/cloud-discovery-service.proto"

python3 -m grpc_tools.protoc \
    -I"$PROTOS_DIR" \
    --python_out="$OUTPUT_DIR" \
    --grpc_python_out="$OUTPUT_DIR" \
    "$PROTOS_DIR/dispatcher-rpc-envelope.proto"

# Move common/*.py to proto_gen root (flatten structure)
if [ -d "$OUTPUT_DIR/common" ]; then
    mv "$OUTPUT_DIR/common"/*.py "$OUTPUT_DIR/"
    rmdir "$OUTPUT_DIR/common"
fi

# Create __init__.py
touch "$OUTPUT_DIR/__init__.py"

# Fix imports in generated files (grpc_tools generates absolute imports)
for f in "$OUTPUT_DIR"/*_pb2_grpc.py "$OUTPUT_DIR"/*_pb2.py; do
    if [ -f "$f" ]; then
        sed -i 's/^import \([a-z_]*_pb2\) as/from . import \1 as/' "$f"
        sed -i 's/^from common import/from . import/' "$f"
    fi
done

echo "Done! Generated files in $OUTPUT_DIR:"
ls -la "$OUTPUT_DIR"/*.py 2>/dev/null | grep -v __pycache__
