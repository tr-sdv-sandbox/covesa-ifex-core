#!/bin/bash

# Script to build the IFEX tools Docker container

set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

echo "Building IFEX tools Docker image..."

# Build the Docker image
docker build -t ifex-tools:latest .

if [ $? -eq 0 ]; then
    echo ""
    echo "✅ IFEX tools Docker image built successfully!"
    echo ""
    echo "You can now use the containerized IFEX tools:"
    echo "  - ifexgen: Generate code from IFEX definitions"
    echo "  - ifexgen_dbus: Generate D-Bus interfaces from IFEX"
    echo "  - ifexconv_protobuf: Convert between IFEX and Protobuf"
    echo ""
    echo "Example usage:"
    echo "  docker run --rm -v \$(pwd):/workspace -w /workspace ifex-tools:latest ifexgen -d protobuf /workspace/ifex/climate-control-service-v2.yml"
else
    echo "❌ Failed to build IFEX tools Docker image"
    exit 1
fi
