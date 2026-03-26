#!/bin/bash

# Run integration tests inside Docker container with Nix devshell
# This avoids local toolchain/protobuf compatibility issues

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

# Default values
BUILD_DIR="build-docker-nix"
TEST_REGEX=""
KEEP_BUILD=false
CLEAN_BUILD=false
JOBS=4

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --build-dir)
            BUILD_DIR="$2"
            shift 2
            ;;
        --test-regex|-R)
            TEST_REGEX="$2"
            shift 2
            ;;
        --keep-build)
            KEEP_BUILD=true
            shift
            ;;
        --clean)
            CLEAN_BUILD=true
            shift
            ;;
        --jobs|-j)
            JOBS="$2"
            shift 2
            ;;
        --help|-h)
            echo "Usage: $0 [options]"
            echo ""
            echo "Run integration tests inside Docker container with Nix devshell."
            echo ""
            echo "Options:"
            echo "  --build-dir DIR    Build directory (default: build-docker-nix)"
            echo "  --test-regex, -R   CTest regex filter (default: all integration tests)"
            echo "  --keep-build       Keep build directory after run"
            echo "  --clean            Remove build directory before starting"
            echo "  --jobs, -j N       Parallel build jobs (default: 4)"
            echo "  --help, -h         Show this help message"
            echo ""
            echo "Examples:"
            echo "  $0                                    # Run all integration tests"
            echo "  $0 -R sync_bridge                     # Run sync bridge tests only"
            echo "  $0 -R reconnect_offline --clean      # Clean build, run specific test"
            exit 0
            ;;
        *)
            echo -e "${RED}Unknown option: $1${NC}"
            echo "Use --help for usage information"
            exit 1
            ;;
    esac
done

# Check Docker is available
if ! command -v docker &> /dev/null; then
    echo -e "${RED}Error: docker command not found${NC}"
    exit 1
fi

if ! docker info &> /dev/null; then
    echo -e "${RED}Error: Docker daemon is not running${NC}"
    exit 1
fi

echo -e "${GREEN}╔══════════════════════════════════════════════════════════════╗${NC}"
echo -e "${GREEN}║       IFEX Integration Tests (Docker + Nix)                  ║${NC}"
echo -e "${GREEN}╚══════════════════════════════════════════════════════════════╝${NC}"
echo ""
echo -e "${CYAN}Build directory:${NC} $BUILD_DIR"
echo -e "${CYAN}Parallel jobs:${NC}   $JOBS"
if [ -n "$TEST_REGEX" ]; then
    echo -e "${CYAN}Test filter:${NC}     $TEST_REGEX"
else
    echo -e "${CYAN}Test filter:${NC}     (all integration tests)"
fi
echo ""

# Clean if requested
if [ "$CLEAN_BUILD" = true ] && [ -d "$BUILD_DIR" ]; then
    echo -e "${YELLOW}Cleaning build directory...${NC}"
    rm -rf "$BUILD_DIR"
fi

# Build the test filter argument
if [ -n "$TEST_REGEX" ]; then
    TEST_FILTER="-R \"$TEST_REGEX\""
else
    TEST_FILTER="-L integration"
fi

# Required build targets for integration tests
BUILD_TARGETS=(
    "ifex-discovery-service"
    "ifex-dispatcher-service"
    "ifex-scheduler-service"
    "ifex-echo-service"
    "ifex-test-types-service"
)

TARGETS_STR="${BUILD_TARGETS[*]}"

echo -e "${GREEN}Starting Docker container...${NC}"
echo ""

docker run --rm \
    -v /var/run/docker.sock:/var/run/docker.sock \
    -v "$(pwd)":/workspace \
    -w /workspace \
    nixos/nix:2.24.11 \
    sh -lc "
        set -euo pipefail
        export DOCKER_HOST=unix:///var/run/docker.sock

        echo '${GREEN}Entering Nix devshell...${NC}'

        # Add flake.nix to git index temporarily if untracked (nix flakes require tracked files)
        if ! git ls-files --error-unmatch flake.nix >/dev/null 2>&1; then
            git add flake.nix
            export FLAKE_WAS_UNTRACKED=1
        else
            export FLAKE_WAS_UNTRACKED=0
        fi

        nix --extra-experimental-features 'nix-command flakes' develop -c bash -lc '
            set -euo pipefail

            # Install newer Docker CLI (API v1.53) to avoid macOS API mismatch
            echo \"${YELLOW}Installing compatible Docker CLI...${NC}\"
            DOCKER_OUT=\$(nix --extra-experimental-features \"nix-command flakes\" build --no-link --print-out-paths nixpkgs/nixos-unstable#docker)
            export PATH=\"\$DOCKER_OUT/bin:\$PATH\"
            
            DOCKER_API=\$(docker version --format \"{{.Client.APIVersion}}\" 2>/dev/null || echo \"unknown\")
            echo \"${CYAN}Docker CLI API version:${NC} \$DOCKER_API\"

            # Get tool paths from devshell
            PLUGIN=\$(which grpc_cpp_plugin)
            PROTOC=\$(which protoc)
            echo \"${CYAN}grpc_cpp_plugin:${NC} \$PLUGIN\"
            echo \"${CYAN}protoc:${NC} \$PROTOC\"
            echo \"\"

            # Configure
            echo \"${GREEN}Configuring CMake...${NC}\"
            cmake -S . -B $BUILD_DIR \
                -DCMAKE_BUILD_TYPE=Debug \
                -DCMAKE_FIND_PACKAGE_PREFER_CONFIG=ON \
                -DGRPC_CPP_PLUGIN_EXECUTABLE=\"\$PLUGIN\" \
                -DProtobuf_PROTOC_EXECUTABLE=\"\$PROTOC\"

            TARGET_HELP=\$(cmake --build $BUILD_DIR --target help)
            if printf '%s' \"\$TARGET_HELP\" | grep -q \"ifex-cloud-vehicle-sync-bridge-integration-test\"; then
                SYNC_BRIDGE_TARGET=\"ifex-cloud-vehicle-sync-bridge-integration-test\"
            elif printf '%s' \"\$TARGET_HELP\" | grep -q \"ifex-cloud-vehicle-sync-bridge-integration-test\"; then
                SYNC_BRIDGE_TARGET=\"ifex-cloud-vehicle-sync-bridge-integration-test\"
            else
                echo \"${RED}Error: could not find sync bridge integration test target${NC}\"
                exit 1
            fi

            # Build required targets
            echo \"\"
            echo \"${GREEN}Building targets...${NC}\"
            cmake --build $BUILD_DIR --target $TARGETS_STR \"\$SYNC_BRIDGE_TARGET\" -j$JOBS

            # Run tests
            echo \"${GREEN}Running tests...${NC}\"
            eval \"ctest --test-dir $BUILD_DIR $TEST_FILTER --output-on-failure\"

            TEST_RESULT=\$?

            # Restore flake.nix tracking state
            if [ \"\$FLAKE_WAS_UNTRACKED\" = \"1\" ]; then
                git reset HEAD flake.nix >/dev/null 2>&1 || true
            fi

            exit \$TEST_RESULT
        '
    "

TEST_EXIT=$?

echo ""
if [ $TEST_EXIT -eq 0 ]; then
    echo -e "${GREEN}╔══════════════════════════════════════════════════════════════╗${NC}"
    echo -e "${GREEN}║                    ALL TESTS PASSED                          ║${NC}"
    echo -e "${GREEN}╚══════════════════════════════════════════════════════════════╝${NC}"
else
    echo -e "${RED}╔══════════════════════════════════════════════════════════════╗${NC}"
    echo -e "${RED}║                    TESTS FAILED                              ║${NC}"
    echo -e "${RED}╚══════════════════════════════════════════════════════════════╝${NC}"
fi

# Cleanup if not keeping build
if [ "$KEEP_BUILD" = false ] && [ -d "$BUILD_DIR" ]; then
    echo ""
    echo -e "${YELLOW}Cleaning up build directory...${NC}"
    rm -rf "$BUILD_DIR"
fi

exit $TEST_EXIT
