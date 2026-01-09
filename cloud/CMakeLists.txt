# Cloud Reference Services
#
# These are simple reference implementations for testing and development.
# NOT for production use. NOT built for cross-compilation targets.
#
# Built only when tests are enabled: ./build.sh --debug --test

message(STATUS "Building cloud reference services (test-only)")

# Generate proto from cloud IFEX specs
set(CLOUD_IFEX_DIR ${CMAKE_CURRENT_SOURCE_DIR}/ifex)
set(CLOUD_PROTO_DIR ${CMAKE_CURRENT_BINARY_DIR}/proto)

file(MAKE_DIRECTORY ${CLOUD_PROTO_DIR})

# TODO: Add proto generation for cloud-backend-transport-service.yml
# Similar to reference-services/ifex generation

# Cloud Backend Transport Reference Service
# add_subdirectory(cloud-backend-transport)
