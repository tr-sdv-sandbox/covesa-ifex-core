#!/bin/bash

# Script to generate proto files from IFEX YAML definitions
# Uses the official IFEX Docker-based tool
#
# This script performs two separate steps:
# 1. Flatten IFEX files (resolve includes) -> reference-specs/generated/ (for runtime)
# 2. Generate proto files from ORIGINAL IFEX -> proto/ifex-generated/ (for wire compatibility)
#
# Why separate?
# - Flattened IFEX: Self-contained for runtime (dynamic gRPC, service registration)
# - Original IFEX for proto: Uses imports for shared types (wire compatible across services)
#
# Type references like "scheduler_types.job_record_t" map to:
# - Flattened IFEX: inlined types in scheduler_types namespace
# - Proto: import "common/scheduler-types.proto" with swdv.scheduler_types.job_record_t

set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PROTO_BASE_DIR="${SCRIPT_DIR}/proto/ifex-generated"
FLATTENED_BASE_DIR="${SCRIPT_DIR}/reference-specs/generated"

echo "=============================================="
echo "IFEX Processing: Flatten + Proto Generation"
echo "=============================================="
echo ""

# =============================================================================
# Step 1: Flatten IFEX files (resolve includes) - for RUNTIME use
# =============================================================================
echo "Step 1: Flattening IFEX files (resolving includes)..."
echo "       (Self-contained IFEX for runtime - keeps type prefixes)"
echo ""
echo "Output structure:"
echo "  reference-specs/generated/vehicle/      <- reference-specs/vehicle/ (flattened)"
echo "  reference-specs/generated/cloud/        <- reference-specs/cloud/ (flattened)"
echo "  reference-specs/generated/test-services/ <- test-services/ (flattened)"
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
        echo "Flattening ${source_dir}/ -> reference-specs/generated/${output_subdir}/"
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
flatten_directory "reference-specs/common" "common"
flatten_directory "reference-specs/vehicle" "vehicle"
flatten_directory "reference-specs/cloud" "cloud"

# Flatten test services (they're in subdirectories)
echo "Flattening test-services/ -> reference-specs/generated/test-services/"
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
# Step 2: Generate proto files from ORIGINAL IFEX (for wire compatibility)
# =============================================================================
echo "Step 2: Generating proto files from ORIGINAL IFEX..."
echo "       (Uses imports for shared types - wire compatible)"
echo ""
echo "Output structure:"
echo "  proto/ifex-generated/common/      <- reference-specs/common/"
echo "  proto/ifex-generated/vehicle/     <- reference-specs/vehicle/"
echo "  proto/ifex-generated/cloud/       <- reference-specs/cloud/"
echo "  proto/ifex-generated/test-services/ <- test-services/"
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

# Process ORIGINAL IFEX files (not flattened) for proto generation
# This preserves includes -> proto imports for shared types
# Format: "source_dir:pattern:output_subdir"
IFEX_SOURCES=(
    "reference-specs/common:*.ifex.yml:common"
    "reference-specs/vehicle:*.ifex.yml:vehicle"
    "reference-specs/cloud:*.ifex.yml:cloud"
)

# Process IFEX files from source directories
for source_entry in "${IFEX_SOURCES[@]}"; do
    IFS=':' read -r source_dir pattern output_subdir <<< "$source_entry"
    full_dir="${SCRIPT_DIR}/${source_dir}"
    output_dir="${PROTO_BASE_DIR}/${output_subdir}"

    if [ -d "$full_dir" ]; then
        echo "Processing ${source_dir}/ -> proto/ifex-generated/${output_subdir}/"

        # Find all matching files
        find "$full_dir" -maxdepth 1 -name "$pattern" | sort | while read yaml_file; do
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

# Process test services (they're in subdirectories)
echo "Processing test-services/ -> proto/ifex-generated/test-services/"
output_dir="${PROTO_BASE_DIR}/test-services"
mkdir -p "$output_dir"

for service_dir in "${SCRIPT_DIR}/test-services"/*; do
    if [ -d "$service_dir" ]; then
        for yaml_file in "$service_dir"/*.ifex.yml; do
            if [ -f "$yaml_file" ]; then
                relative_path="${yaml_file#$SCRIPT_DIR/}"
                base_name=$(basename "$yaml_file")
                base_name="${base_name%.ifex.yml}"
                base_name="${base_name%.yml}"
                process_ifex_file "$yaml_file" "$relative_path" "$output_dir" "$base_name"
            fi
        done
    fi
done

# Also process test-types
for yaml_file in "${SCRIPT_DIR}/tests/test-types"/*.ifex.yml; do
    if [ -f "$yaml_file" ]; then
        relative_path="${yaml_file#$SCRIPT_DIR/}"
        base_name=$(basename "$yaml_file")
        base_name="${base_name%.ifex.yml}"
        base_name="${base_name%.yml}"
        process_ifex_file "$yaml_file" "$relative_path" "$output_dir" "$base_name"
    fi
done
echo ""

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
                           --proto_path="${PROTO_BASE_DIR}" \
                           "$(basename "$proto_file")" 2>/dev/null || \
                    protoc --python_out="${python_dir}" \
                           --proto_path="${proto_dir}" \
                           --proto_path="${PROTO_BASE_DIR}" \
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
