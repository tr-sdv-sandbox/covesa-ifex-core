#!/bin/bash

# Script to install dependencies for IFEX Core
# Usage: ./install_deps.sh [--apt-only]
#   --apt-only: Only install apt packages, skip Docker image and cpp-mcp setup

set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

APT_ONLY=false
if [ "$1" == "--apt-only" ]; then
    APT_ONLY=true
fi

echo -e "${GREEN}============================================${NC}"
echo -e "${GREEN}  COVESA IFEX Core - Dependency Installer  ${NC}"
echo -e "${GREEN}============================================${NC}"
echo ""

# Install apt packages
echo -e "${YELLOW}Installing system packages via apt...${NC}"
echo ""

sudo apt-get update

# Core build tools
echo -e "${YELLOW}[1/8] Installing build tools...${NC}"
sudo apt-get install -y \
    build-essential \
    cmake \
    pkg-config \
    git

# Protocol Buffers
echo ""
echo -e "${YELLOW}[2/8] Installing Protocol Buffers...${NC}"
sudo apt-get install -y \
    protobuf-compiler \
    libprotobuf-dev

# gRPC
echo ""
echo -e "${YELLOW}[3/8] Installing gRPC...${NC}"
sudo apt-get install -y \
    libgrpc++-dev \
    libgrpc-dev \
    protobuf-compiler-grpc

# Logging libraries
echo ""
echo -e "${YELLOW}[4/8] Installing logging libraries...${NC}"
sudo apt-get install -y \
    libgoogle-glog-dev \
    libgflags-dev

# JSON and YAML
echo ""
echo -e "${YELLOW}[5/8] Installing JSON and YAML libraries...${NC}"
sudo apt-get install -y \
    nlohmann-json3-dev \
    libyaml-cpp-dev

# Lua
echo ""
echo -e "${YELLOW}[6/8] Installing Lua...${NC}"
sudo apt-get install -y \
    liblua5.4-dev \
    lua5.4

# Testing frameworks
echo ""
echo -e "${YELLOW}[7/8] Installing testing frameworks...${NC}"
sudo apt-get install -y \
    libgtest-dev \
    libgmock-dev

# Python dependencies (for REST proxy and proto generation)
echo ""
echo -e "${YELLOW}[8/8] Installing Python dependencies...${NC}"
sudo apt-get install -y \
    python3 \
    python3-pip \
    python3-grpcio \
    python3-protobuf \
    python3-flask \
    python3-flask-cors \
    python3-yaml

# Optional: grpcurl for testing
echo ""
echo -e "${YELLOW}Installing grpcurl (optional, for testing)...${NC}"
sudo snap install --edge grpcurl 2>/dev/null || echo "grpcurl snap install skipped"

echo ""
echo -e "${GREEN}✓ All apt packages installed successfully${NC}"

if [ "$APT_ONLY" = true ]; then
    echo ""
    echo -e "${GREEN}============================================${NC}"
    echo -e "${GREEN}  APT packages installed (--apt-only mode) ${NC}"
    echo -e "${GREEN}============================================${NC}"
    exit 0
fi

# Check for Docker
echo ""
echo -e "${YELLOW}Checking Docker installation...${NC}"
if ! command -v docker &> /dev/null; then
    echo -e "${YELLOW}Docker not found. Installing docker.io...${NC}"
    sudo apt-get install -y docker.io
    sudo systemctl enable docker
    sudo systemctl start docker
    echo -e "${GREEN}✓ Docker installed${NC}"
    echo -e "${YELLOW}Note: You may need to add your user to the docker group:${NC}"
    echo "  sudo usermod -aG docker \$USER"
    echo "  (then log out and back in)"
else
    echo -e "${GREEN}✓ Docker already installed${NC}"
fi

# Build IFEX Docker tool
echo ""
echo -e "${YELLOW}Setting up IFEX Docker tool...${NC}"

ORIGINAL_PROJECT="${SCRIPT_DIR}/../covesa_ifex_playground"
if [ -d "${ORIGINAL_PROJECT}/tools/ifex" ]; then
    if docker images | grep -q "ifex-tools"; then
        echo -e "${GREEN}✓ IFEX Docker image already exists${NC}"
        echo "  To rebuild: docker rmi ifex-tools:latest && ./install_deps.sh"
    else
        echo "Building IFEX Docker image..."
        cd "${ORIGINAL_PROJECT}/tools/ifex"
        ./build_ifex.sh
        cd "${SCRIPT_DIR}"
        echo -e "${GREEN}✓ IFEX Docker image built${NC}"
    fi
else
    echo -e "${YELLOW}⚠ IFEX tools not found at ${ORIGINAL_PROJECT}/tools/ifex${NC}"
    echo "  Proto generation will not work without ifex-tools Docker image."
    echo "  Please ensure covesa_ifex_playground is in the parent directory."
fi

# Setup cpp-mcp for MCP server
echo ""
echo -e "${YELLOW}Setting up cpp-mcp library...${NC}"

MCP_DIR="${SCRIPT_DIR}/third_party/cpp-mcp"
if [ ! -d "${MCP_DIR}" ]; then
    echo "Cloning cpp-mcp repository..."
    mkdir -p "${SCRIPT_DIR}/third_party"
    cd "${SCRIPT_DIR}/third_party"
    git clone git@github.com:skarlsson/cpp-mcp.git || {
        echo -e "${YELLOW}SSH clone failed, trying HTTPS...${NC}"
        git clone https://github.com/skarlsson/cpp-mcp.git
    }
    cd "${SCRIPT_DIR}"
    echo -e "${GREEN}✓ cpp-mcp cloned successfully${NC}"
else
    echo -e "${GREEN}✓ cpp-mcp already exists${NC}"
fi

# Create build directory
echo ""
echo -e "${YELLOW}Creating build directories...${NC}"
mkdir -p build
mkdir -p proto
echo -e "${GREEN}✓ Build directories created${NC}"

# Summary
echo ""
echo -e "${GREEN}============================================${NC}"
echo -e "${GREEN}  Installation Complete!                   ${NC}"
echo -e "${GREEN}============================================${NC}"
echo ""
echo "Installed packages:"
echo "  - build-essential, cmake, pkg-config, git"
echo "  - protobuf-compiler, libprotobuf-dev"
echo "  - libgrpc++-dev, libgrpc-dev, protobuf-compiler-grpc"
echo "  - libgoogle-glog-dev, libgflags-dev"
echo "  - nlohmann-json3-dev, libyaml-cpp-dev"
echo "  - liblua5.4-dev, lua5.4"
echo "  - libgtest-dev, libgmock-dev"
echo "  - python3, python3-grpcio, python3-flask, etc."
echo ""
echo "Next steps:"
echo ""
echo "  1. Generate proto files from IFEX schemas:"
echo "     ./generate_proto.sh"
echo ""
echo "  2. Build the project:"
echo "     mkdir -p build && cd build"
echo "     cmake -DGRPC_CPP_PLUGIN_EXECUTABLE=/usr/bin/grpc_cpp_plugin .."
echo "     make -j\$(nproc)"
echo ""
echo "  3. Run services:"
echo "     ./build/reference-services/discovery/ifex-discovery-service"
echo "     ./build/reference-services/dispatcher/ifex-dispatcher-service --discovery localhost:50051"
echo "     ./build/reference-services/scheduler/ifex-scheduler-service --discovery localhost:50051"
echo ""
