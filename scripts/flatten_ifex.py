#!/usr/bin/env python3
"""
Flatten IFEX YAML files by resolving includes.

This script takes an IFEX YAML file with `includes:` directives and produces
a self-contained flattened version with all included namespaces merged.

Usage:
    python3 flatten_ifex.py input.ifex.yml output.ifex.yml [--base-dir DIR]

The flattened output is suitable for:
- Service registration with Discovery (no include resolution needed)
- Syncing to cloud (self-contained schema)
- Runtime loading (no file dependencies)
"""

import argparse
import os
import sys
from pathlib import Path

import yaml


def load_yaml(path: Path) -> dict:
    """Load a YAML file."""
    with open(path, 'r') as f:
        return yaml.safe_load(f)


def resolve_include_path(include_file: str, current_file: Path, base_dir: Path) -> Path:
    """Resolve an include path to an absolute path.

    Tries in order:
    1. Relative to base_dir (e.g., specs/common/types.yml from project root)
    2. Relative to the current file's directory
    """
    # Try relative to base_dir first
    path = base_dir / include_file
    if path.exists():
        return path

    # Try relative to current file
    path = current_file.parent / include_file
    if path.exists():
        return path

    raise FileNotFoundError(f"Cannot resolve include: {include_file}")


def flatten_ifex(input_path: Path, base_dir: Path, visited: set = None) -> dict:
    """Recursively flatten an IFEX file by resolving includes.

    Args:
        input_path: Path to the IFEX YAML file
        base_dir: Base directory for resolving includes
        visited: Set of already-visited files (for cycle detection)

    Returns:
        Flattened IFEX dict with all includes resolved
    """
    if visited is None:
        visited = set()

    # Cycle detection
    abs_path = input_path.resolve()
    if abs_path in visited:
        raise ValueError(f"Circular include detected: {input_path}")
    visited.add(abs_path)

    # Load the file
    data = load_yaml(input_path)
    if not data:
        return {}

    # Process includes
    includes = data.pop('includes', [])
    included_namespaces = []

    for include in includes:
        include_file = include.get('file')
        if not include_file:
            continue

        # Resolve the include path
        include_path = resolve_include_path(include_file, input_path, base_dir)

        # Recursively flatten the included file
        included_data = flatten_ifex(include_path, base_dir, visited.copy())

        # Extract namespaces from included file
        if 'namespaces' in included_data:
            # The included service name becomes the namespace name for references
            # e.g., scheduler_types.ifex.yml with name "scheduler_types"
            # means references like "scheduler_types.job_status_t" should work
            service_name = included_data.get('name', '')

            # Merge all namespaces from included file into a single namespace
            # named after the service (for reference resolution)
            merged_ns = {
                'name': service_name,
                'description': f"Types from included {service_name}",
                'enumerations': [],
                'structs': [],
                'methods': [],
            }

            for ns in included_data['namespaces']:
                # Merge enumerations
                if 'enumerations' in ns:
                    merged_ns['enumerations'].extend(ns['enumerations'])
                # Merge structs
                if 'structs' in ns:
                    merged_ns['structs'].extend(ns['structs'])
                # Merge methods (if any)
                if 'methods' in ns:
                    merged_ns['methods'].extend(ns['methods'])

            # Remove empty lists
            merged_ns = {k: v for k, v in merged_ns.items() if v}
            included_namespaces.append(merged_ns)

    # Merge included namespaces with this file's namespaces
    if included_namespaces:
        existing_namespaces = data.get('namespaces', [])
        # Add included namespaces first (so main file's definitions take precedence)
        data['namespaces'] = included_namespaces + existing_namespaces

    return data


def main():
    parser = argparse.ArgumentParser(
        description='Flatten IFEX YAML files by resolving includes'
    )
    parser.add_argument('input', help='Input IFEX YAML file')
    parser.add_argument('output', help='Output flattened YAML file')
    parser.add_argument('--base-dir', '-b', default='.',
                        help='Base directory for resolving includes (default: current dir)')
    parser.add_argument('--quiet', '-q', action='store_true',
                        help='Suppress output messages')

    args = parser.parse_args()

    input_path = Path(args.input)
    output_path = Path(args.output)
    base_dir = Path(args.base_dir)

    if not input_path.exists():
        print(f"Error: Input file not found: {input_path}", file=sys.stderr)
        sys.exit(1)

    try:
        # Flatten the file
        flattened = flatten_ifex(input_path, base_dir)

        # Create output directory if needed
        output_path.parent.mkdir(parents=True, exist_ok=True)

        # Write output
        with open(output_path, 'w') as f:
            # Add header comment
            f.write("# AUTO-GENERATED - DO NOT EDIT\n")
            f.write(f"# Flattened from: {input_path}\n")
            f.write("# Regenerate with: ./generate_proto.sh\n")
            f.write("---\n")
            yaml.dump(flattened, f, default_flow_style=False, sort_keys=False, allow_unicode=True)

        if not args.quiet:
            print(f"Flattened: {input_path} -> {output_path}")

    except Exception as e:
        print(f"Error processing {input_path}: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == '__main__':
    main()
