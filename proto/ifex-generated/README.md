# IFEX-Generated Proto Files

**DO NOT EDIT THESE FILES MANUALLY**

These proto files are automatically generated from IFEX YAML specifications.

## Regenerating

```bash
./generate_proto.sh
```

## Source Files

The IFEX YAML source files are located in:
- `reference-services/ifex/` - Core infrastructure services
- `test-services/*/` - Test service definitions

## If You Need to Change a Proto

1. Edit the corresponding `.ifex.yml` file
2. Run `./generate_proto.sh`
3. Rebuild: `cmake --build build`

Do NOT edit these `.proto` files directly - your changes will be overwritten.
