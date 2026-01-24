#!/bin/bash

# Script to generate proto files from IFEX YAML definitions
# Uses the official IFEX Docker-based tool
#
# This script performs two steps:
# 1. Flatten IFEX files (resolve includes) -> specs/generated/
# 2. Generate proto files from flattened IFEX -> proto/ifex-generated/

set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PROTO_BASE_DIR="${SCRIPT_DIR}/proto/ifex-generated"
FLATTENED_BASE_DIR="${SCRIPT_DIR}/specs/generated"

echo "=============================================="
echo "IFEX Processing: Flatten + Proto Generation"
echo "=============================================="
echo ""

# =============================================================================
# Step 1: Flatten IFEX files (resolve includes)
# =============================================================================
echo "Step 1: Flattening IFEX files (resolving includes)..."
echo ""
echo "Output structure:"
echo "  specs/generated/vehicle/      <- specs/vehicle/ (flattened)"
echo "  specs/generated/cloud/        <- specs/cloud/ (flattened)"
echo "  specs/generated/test-services/ <- test-services/ (flattened)"
echo ""

# Check if Python3 and PyYAML are available
if ! command -v python3 &> /dev/null; then
    echo "Error: python3 not found."
    exit 1
fi

if ! python3 -c "import yaml" 2>/dev/null; then
    echo "Error: PyYAML not found. Install with: pip3 install pyyaml"
    exit 1
fi

FLATTEN_SCRIPT="${SCRIPT_DIR}/scripts/flatten_ifex.py"

# Create output directories
mkdir -p "${FLATTENED_BASE_DIR}/vehicle"
mkdir -p "${FLATTENED_BASE_DIR}/cloud"
mkdir -p "${FLATTENED_BASE_DIR}/test-services"
mkdir -p "${FLATTENED_BASE_DIR}/common"

# Function to flatten IFEX files in a directory
flatten_directory() {
    local source_dir="$1"
    local output_subdir="$2"
    local full_source="${SCRIPT_DIR}/${source_dir}"
    local full_output="${FLATTENED_BASE_DIR}/${output_subdir}"

    if [ -d "$full_source" ]; then
        echo "Flattening ${source_dir}/ -> specs/generated/${output_subdir}/"
        find "$full_source" -maxdepth 1 -name "*.ifex.yml" | sort | while read yaml_file; do
            if [ -f "$yaml_file" ]; then
                base_name=$(basename "$yaml_file")
                python3 "$FLATTEN_SCRIPT" \
                    "$yaml_file" \
                    "${full_output}/${base_name}" \
                    --base-dir "$SCRIPT_DIR" \
                    --quiet
                echo "  ${base_name}"
            fi
        done
        echo ""
    fi
}

# Flatten all IFEX directories
flatten_directory "specs/common" "common"
flatten_directory "specs/vehicle" "vehicle"
flatten_directory "specs/cloud" "cloud"

# Flatten test services (they're in subdirectories)
echo "Flattening test-services/ -> specs/generated/test-services/"
for service_dir in "${SCRIPT_DIR}/test-services"/*; do
    if [ -d "$service_dir" ]; then
        for yaml_file in "$service_dir"/*.ifex.yml; do
            if [ -f "$yaml_file" ]; then
                base_name=$(basename "$yaml_file")
                python3 "$FLATTEN_SCRIPT" \
                    "$yaml_file" \
                    "${FLATTENED_BASE_DIR}/test-services/${base_name}" \
                    --base-dir "$SCRIPT_DIR" \
                    --quiet
                echo "  ${base_name}"
            fi
        done
    fi
done

# Also flatten test-types
for yaml_file in "${SCRIPT_DIR}/tests/test-types"/*.ifex.yml; do
    if [ -f "$yaml_file" ]; then
        base_name=$(basename "$yaml_file")
        python3 "$FLATTEN_SCRIPT" \
            "$yaml_file" \
            "${FLATTENED_BASE_DIR}/test-services/${base_name}" \
            --base-dir "$SCRIPT_DIR" \
            --quiet
        echo "  ${base_name}"
    fi
done
echo ""

echo "Flattening complete!"
echo ""

# =============================================================================
# Step 2: Generate proto files from flattened IFEX
# =============================================================================
echo "Step 2: Generating proto files from flattened IFEX..."
echo ""
echo "Output structure:"
echo "  proto/ifex-generated/common/      <- specs/generated/common/"
echo "  proto/ifex-generated/vehicle/     <- specs/generated/vehicle/"
echo "  proto/ifex-generated/cloud/       <- specs/generated/cloud/"
echo "  proto/ifex-generated/test-services/ <- specs/generated/test-services/"
echo ""

# Check if ifex Docker image is available
if ! docker images | grep -q "ifex-tools"; then
    echo "Error: ifex-tools Docker image not found."
    echo "Please run ./install_deps.sh first to set up dependencies."
    exit 1
fi

# Template directory (local override of ifex-tools built-in templates)
TEMPLATE_DIR="${SCRIPT_DIR}/templates"

# Function to process IFEX file
process_ifex_file() {
    local yaml_file="$1"
    local relative_path="$2"
    local output_dir="$3"
    local output_name="$4"

    mkdir -p "${output_dir}"
    proto_file="${output_dir}/${output_name}.proto"

    echo "  ${relative_path} -> ${proto_file#$SCRIPT_DIR/}"

    # Use the IFEX tool via Docker with local template override
    docker run --rm \
        -v "${SCRIPT_DIR}:/workspace" \
        -v "${TEMPLATE_DIR}:/templates:ro" \
        -w /workspace \
        ifex-tools:latest \
        ifexgen -d /templates/protobuf "${relative_path}" > "${proto_file}"
}

# Define source directories, patterns, and output subdirectories
# Use flattened files as source for proto generation
# Format: "source_dir:pattern:output_subdir"
IFEX_SOURCES=(
    "specs/generated/common:*.ifex.yml:common"
    "specs/generated/vehicle:*.ifex.yml:vehicle"
    "specs/generated/cloud:*.ifex.yml:cloud"
    "specs/generated/test-services:*.ifex.yml:test-services"
)

# Process IFEX files from all source directories
for source_entry in "${IFEX_SOURCES[@]}"; do
    IFS=':' read -r source_dir pattern output_subdir <<< "$source_entry"
    full_dir="${SCRIPT_DIR}/${source_dir}"
    output_dir="${PROTO_BASE_DIR}/${output_subdir}"

    if [ -d "$full_dir" ]; then
        echo "Processing ${source_dir}/ -> proto/ifex-generated/${output_subdir}/"

        # Find all matching files
        find "$full_dir" -name "$pattern" | sort | while read yaml_file; do
            if [ -f "$yaml_file" ]; then
                # Get relative path from script dir
                relative_path="${yaml_file#$SCRIPT_DIR/}"

                # Generate output name (remove extensions)
                base_name=$(basename "$yaml_file")
                base_name="${base_name%.ifex.yml}"
                base_name="${base_name%.yml}"

                process_ifex_file "$yaml_file" "$relative_path" "$output_dir" "$base_name"
            fi
        done
        echo ""
    fi
done

echo "Proto generation complete!"
echo ""

# Generate Python protobuf files
echo "Generating Python protobuf files..."

# Check if protoc is available
if ! command -v protoc &> /dev/null; then
    echo "Warning: protoc not found. Skipping Python protobuf generation."
    echo "Install protobuf-compiler to generate Python files: sudo apt-get install protobuf-compiler"
else
    # Generate Python files for each subdirectory
    for subdir in common vehicle cloud test-services; do
        proto_dir="${PROTO_BASE_DIR}/${subdir}"
        python_dir="${proto_dir}/python"

        if [ -d "$proto_dir" ] && ls "$proto_dir"/*.proto 1>/dev/null 2>&1; then
            mkdir -p "$python_dir"
            echo "  ${subdir}/python/"

            for proto_file in "$proto_dir"/*.proto; do
                if [ -f "$proto_file" ]; then
                    protoc --python_out="${python_dir}" \
                           --grpc_python_out="${python_dir}" \
                           --plugin=protoc-gen-grpc_python=$(which grpc_python_plugin) \
                           --proto_path="${proto_dir}" \
                           "$(basename "$proto_file")" 2>/dev/null || \
                    protoc --python_out="${python_dir}" \
                           --proto_path="${proto_dir}" \
                           "$(basename "$proto_file")"
                fi
            done
        fi
    done
    echo ""
fi

echo "Generated proto structure:"
find "${PROTO_BASE_DIR}" -name "*.proto" | sed "s|${SCRIPT_DIR}/||" | sort

echo ""
echo "Next steps:"
echo "1. Run ./build.sh to build the project"
echo "2. CMake will automatically generate C++ code from these proto files"
