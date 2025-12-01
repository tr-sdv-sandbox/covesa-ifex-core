#!/bin/bash

# Build script for IFEX Core v0

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Default values
BUILD_TYPE="Release"
BUILD_DIR="build"
CLEAN_BUILD=false
RUN_TESTS=false

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --debug)
            BUILD_TYPE="Debug"
            shift
            ;;
        --clean)
            CLEAN_BUILD=true
            shift
            ;;
        --test)
            RUN_TESTS=true
            shift
            ;;
        --help)
            echo "Usage: $0 [options]"
            echo "Options:"
            echo "  --debug    Build in debug mode"
            echo "  --clean    Clean build directory before building"
            echo "  --test     Run tests after building"
            echo "  --help     Show this help message"
            exit 0
            ;;
        *)
            echo -e "${RED}Unknown option: $1${NC}"
            exit 1
            ;;
    esac
done

echo -e "${GREEN}Building IFEX Core v0${NC}"
echo "Build type: $BUILD_TYPE"

# Clean if requested
if [ "$CLEAN_BUILD" = true ]; then
    echo -e "${YELLOW}Cleaning build directory...${NC}"
    rm -rf "$BUILD_DIR"
fi

# Create build directory
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Configure
echo -e "${GREEN}Configuring...${NC}"
cmake -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
      -DBUILD_CORE=ON \
      -DBUILD_REFERENCE_SERVICES=ON \
      -DBUILD_INTEGRATION_TESTS=ON \
      ..

# Build
echo -e "${GREEN}Building...${NC}"
make -j$(nproc)

# Run tests if requested
if [ "$RUN_TESTS" = true ]; then
    echo -e "${GREEN}Running tests...${NC}"
    ctest --output-on-failure
fi

echo -e "${GREEN}Build complete!${NC}"
echo ""
echo "To run the services:"
echo "  1. Start discovery:  $BUILD_DIR/reference-services/discovery/ifex-discovery-service"
echo "  2. Start dispatcher: $BUILD_DIR/reference-services/dispatcher/ifex-dispatcher-service"