#!/bin/bash

# Script to generate proto files and IFEX schema headers from IFEX YAML definitions
# Uses the official IFEX Docker-based tool
#
# This script performs two steps:
# 1. Generate C++ headers with flattened IFEX as raw strings (for service registration)
# 2. Generate proto files from ORIGINAL IFEX (for wire compatibility)
#
# The generated headers allow services to embed their IFEX schema:
#   #include "scheduler-service.ifex.h"
#   discovery_client.register_service(ifex::schema::scheduler_service, port);

set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PROTO_BASE_DIR="${SCRIPT_DIR}/proto/ifex-generated"

echo "=============================================="
echo "IFEX Processing: Schema Headers + Proto Generation"
echo "=============================================="
echo ""

# =============================================================================
# Step 1: Generate C++ headers with flattened IFEX schemas
# =============================================================================
echo "Step 1: Generating C++ headers with embedded IFEX schemas..."
echo "       (Services can #include these to get their schema as a string)"
echo ""
echo "Output:"
echo "  proto/ifex-generated/vehicle/*.ifex.h"
echo "  proto/ifex-generated/cloud/*.ifex.h"
echo "  proto/ifex-generated/test-services/*.ifex.h"
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

FLATTEN_SCRIPT="${SCRIPT_DIR}/tools/ifex/scripts/flatten_ifex.py"

# Function to convert IFEX filename to C++ identifier
to_cpp_identifier() {
    local name="$1"
    # Remove .ifex.yml extension, replace - with _
    echo "$name" | sed 's/\.ifex\.yml$//' | sed 's/-/_/g'
}

# Function to generate C++ header with flattened IFEX as raw string
generate_ifex_header() {
    local yaml_file="$1"
    local output_dir="$2"
    local base_name=$(basename "$yaml_file")
    local cpp_name=$(to_cpp_identifier "$base_name")
    local header_file="${output_dir}/${base_name%.yml}.h"

    mkdir -p "$output_dir"

    # Create temp file for flattened YAML
    local temp_yaml=$(mktemp)

    # Flatten the IFEX file
    python3 "$FLATTEN_SCRIPT" \
        "$yaml_file" \
        "$temp_yaml" \
        --base-dir "$SCRIPT_DIR" \
        --quiet

    # Generate C++ header
    cat > "$header_file" << HEADER_START
// AUTO-GENERATED - DO NOT EDIT
// Generated from: ${yaml_file#$SCRIPT_DIR/}
// Regenerate with: ./generate_proto.sh

#pragma once

namespace ifex::schema {

inline constexpr const char* ${cpp_name} = R"IFEX(
HEADER_START

    # Append the flattened YAML content
    cat "$temp_yaml" >> "$header_file"

    # Close the raw string and namespace
    cat >> "$header_file" << HEADER_END
)IFEX";

}  // namespace ifex::schema
HEADER_END

    rm -f "$temp_yaml"
    echo "  ${base_name} -> ${header_file#$SCRIPT_DIR/}"
}

# Generate headers for vehicle specs (from each service's vehicle/ subdirectory)
echo "Generating vehicle schema headers..."
for service_dir in "${SCRIPT_DIR}/reference-specs"/*; do
    if [ -d "$service_dir/vehicle" ]; then
        for yaml_file in "$service_dir/vehicle"/*.ifex.yml; do
            if [ -f "$yaml_file" ]; then
                generate_ifex_header "$yaml_file" "${PROTO_BASE_DIR}/vehicle"
            fi
        done
    fi
done
echo ""

# Generate headers for cloud specs (from each service's cloud/ subdirectory)
echo "Generating cloud schema headers..."
for service_dir in "${SCRIPT_DIR}/reference-specs"/*; do
    if [ -d "$service_dir/cloud" ]; then
        for yaml_file in "$service_dir/cloud"/*.ifex.yml; do
            if [ -f "$yaml_file" ]; then
                generate_ifex_header "$yaml_file" "${PROTO_BASE_DIR}/cloud"
            fi
        done
    fi
done
echo ""

# Generate headers for common specs (from each service's common/ subdirectory)
echo "Generating common schema headers..."
for service_dir in "${SCRIPT_DIR}/reference-specs"/*; do
    if [ -d "$service_dir/common" ]; then
        for yaml_file in "$service_dir/common"/*.ifex.yml; do
            if [ -f "$yaml_file" ]; then
                generate_ifex_header "$yaml_file" "${PROTO_BASE_DIR}/common"
            fi
        done
    fi
done
echo ""

# Generate headers for test services
echo "Generating test-services schema headers..."
mkdir -p "${PROTO_BASE_DIR}/test-services"
for service_dir in "${SCRIPT_DIR}/test-services"/*; do
    if [ -d "$service_dir" ]; then
        for yaml_file in "$service_dir"/*.ifex.yml; do
            if [ -f "$yaml_file" ]; then
                generate_ifex_header "$yaml_file" "${PROTO_BASE_DIR}/test-services"
            fi
        done
    fi
done

# Also process test-types
for yaml_file in "${SCRIPT_DIR}/tests/test-types"/*.ifex.yml; do
    if [ -f "$yaml_file" ]; then
        generate_ifex_header "$yaml_file" "${PROTO_BASE_DIR}/test-services"
    fi
done
echo ""

echo "Schema header generation complete!"
echo ""

# =============================================================================
# Step 2: Generate proto files from ORIGINAL IFEX (for wire compatibility)
# =============================================================================
echo "Step 2: Generating proto files from ORIGINAL IFEX..."
echo "       (Uses imports for shared types - wire compatible)"
echo ""
echo "Output structure:"
echo "  proto/ifex-generated/common/      <- reference-specs/*/common/"
echo "  proto/ifex-generated/vehicle/     <- reference-specs/*/vehicle/"
echo "  proto/ifex-generated/cloud/       <- reference-specs/*/cloud/"
echo "  proto/ifex-generated/test-services/ <- test-services/"
echo ""

# Check if ifex Docker image is available
if ! docker images | grep -q "ifex-tools"; then
    echo "Error: ifex-tools Docker image not found."
    echo "Please run ./install_deps.sh first to set up dependencies."
    exit 1
fi

# Template directory (local override of ifex-tools built-in templates)
TEMPLATE_DIR="${SCRIPT_DIR}/tools/ifex/templates"

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
# Service-centric structure: reference-specs/{service}/{vehicle,cloud,common}/

# Process each output category by scanning all service directories
echo "Processing reference-specs/*/{vehicle,cloud,common}/ -> proto/ifex-generated/{vehicle,cloud,common}/"

# Process vehicle specs from all services
echo "Processing vehicle specs..."
output_dir="${PROTO_BASE_DIR}/vehicle"
mkdir -p "$output_dir"
for service_dir in "${SCRIPT_DIR}/reference-specs"/*; do
    if [ -d "$service_dir/vehicle" ]; then
        for yaml_file in "$service_dir/vehicle"/*.ifex.yml; do
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
echo ""

# Process cloud specs from all services
echo "Processing cloud specs..."
output_dir="${PROTO_BASE_DIR}/cloud"
mkdir -p "$output_dir"
for service_dir in "${SCRIPT_DIR}/reference-specs"/*; do
    if [ -d "$service_dir/cloud" ]; then
        for yaml_file in "$service_dir/cloud"/*.ifex.yml; do
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
echo ""

# Process common specs from all services
echo "Processing common specs..."
output_dir="${PROTO_BASE_DIR}/common"
mkdir -p "$output_dir"
for service_dir in "${SCRIPT_DIR}/reference-specs"/*; do
    if [ -d "$service_dir/common" ]; then
        for yaml_file in "$service_dir/common"/*.ifex.yml; do
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
echo ""

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

echo "Generated files:"
echo ""
echo "Schema headers (for service registration):"
find "${PROTO_BASE_DIR}" -name "*.ifex.h" | sed "s|${SCRIPT_DIR}/||" | sort
echo ""
echo "Proto files (for wire format):"
find "${PROTO_BASE_DIR}" -name "*.proto" | sed "s|${SCRIPT_DIR}/||" | sort

echo ""
echo "Next steps:"
echo "1. Run ./build.sh to build the project"
echo "2. Services can now #include their schema header:"
echo "   #include \"scheduler-service.ifex.h\""
echo "   discovery_client.register_service(ifex::schema::scheduler_service, port);"
