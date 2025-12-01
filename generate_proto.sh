#!/bin/bash

# Script to generate proto files from IFEX YAML definitions
# Uses the official IFEX Docker-based tool

set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PROTO_DIR="${SCRIPT_DIR}/proto"

# Create output directory
mkdir -p "${PROTO_DIR}"

echo "Generating proto files from IFEX YAML definitions..."

# Check if ifex Docker image is available
if ! docker images | grep -q "ifex-tools"; then
    echo "Error: ifex-tools Docker image not found."
    echo "Please run ./install_deps.sh first to set up dependencies."
    exit 1
fi

# Function to process IFEX file
process_ifex_file() {
    local yaml_file="$1"
    local relative_path="$2"
    local output_name="$3"
    
    proto_file="${PROTO_DIR}/${output_name}.proto"
    
    echo "Processing ${yaml_file}..."
    
    # Use the IFEX tool via Docker
    docker run --rm \
        -v "${SCRIPT_DIR}:/workspace" \
        -w /workspace \
        ifex-tools:latest \
        ifexgen -d protobuf "${relative_path}" > "${proto_file}"
    
    echo "Generated ${proto_file}"
}

# Define directories containing IFEX files and their patterns
declare -A IFEX_SOURCES=(
    ["reference-services/ifex"]="*.yml"
    ["test-services"]="*.ifex.yml"
    ["lua-orchestration"]="*.ifex.yml"
    ["lua-orchestration2/test"]="*.ifex.yml"
)

# Process IFEX files from all source directories
for source_dir in "${!IFEX_SOURCES[@]}"; do
    pattern="${IFEX_SOURCES[$source_dir]}"
    full_dir="${SCRIPT_DIR}/${source_dir}"
    
    if [ -d "$full_dir" ]; then
        echo ""
        echo "Processing IFEX files in ${source_dir}..."
        
        # Find all matching files
        find "$full_dir" -name "$pattern" | while read yaml_file; do
            if [ -f "$yaml_file" ]; then
                # Get relative path from script dir
                relative_path="${yaml_file#$SCRIPT_DIR/}"
                
                # Generate output name (remove extensions)
                base_name=$(basename "$yaml_file")
                # Remove .ifex.yml or .yml extension
                base_name="${base_name%.ifex.yml}"
                base_name="${base_name%.yml}"
                
                process_ifex_file "$yaml_file" "$relative_path" "$base_name"
            fi
        done
    fi
done

echo ""
echo "Proto generation complete!"
echo "Generated proto files in: ${PROTO_DIR}"

# Generate Python protobuf files
echo ""
echo "Generating Python protobuf files..."
PYTHON_PROTO_DIR="${PROTO_DIR}/python"
mkdir -p "${PYTHON_PROTO_DIR}"

# Check if protoc is available
if ! command -v protoc &> /dev/null; then
    echo "Warning: protoc not found. Skipping Python protobuf generation."
    echo "Install protobuf-compiler to generate Python files: sudo apt-get install protobuf-compiler"
else
    # Generate Python files for each proto
    for proto_file in "${PROTO_DIR}"/*.proto; do
        if [ -f "$proto_file" ]; then
            echo "Generating Python for $(basename "$proto_file")..."
            protoc --python_out="${PYTHON_PROTO_DIR}" \
                   --grpc_python_out="${PYTHON_PROTO_DIR}" \
                   --plugin=protoc-gen-grpc_python=$(which grpc_python_plugin) \
                   --proto_path="${PROTO_DIR}" \
                   "$(basename "$proto_file")"
        fi
    done
    echo "Python protobuf files generated in: ${PYTHON_PROTO_DIR}"
fi

echo ""
echo "Next steps:"
echo "1. Run ./build.sh to build the project"
echo "2. CMake will automatically generate C++ code from these proto files"