# Cloud Reference Services
#
# These are simple reference implementations for testing and development.
# NOT for production use. NOT built for cross-compilation targets.
#
# Built only when tests are enabled: ./build.sh --debug --test

message(STATUS "Building cloud reference services (test-only)")

# Generate proto from cloud IFEX specs
# Primary location: specs/cloud/ (source of truth)
set(CLOUD_IFEX_DIR ${CMAKE_SOURCE_DIR}/specs/cloud)
set(CLOUD_PROTO_DIR ${CMAKE_CURRENT_BINARY_DIR}/proto)

file(MAKE_DIRECTORY ${CLOUD_PROTO_DIR})

# Copy cloud IFEX specs to build directory
file(GLOB CLOUD_IFEX_FILES "${CLOUD_IFEX_DIR}/*.ifex.yml")
foreach(IFEX_FILE ${CLOUD_IFEX_FILES})
    get_filename_component(IFEX_NAME ${IFEX_FILE} NAME)
    configure_file(${IFEX_FILE} ${CMAKE_BINARY_DIR}/ifex/${IFEX_NAME} COPYONLY)
endforeach()

# TODO: Add proto generation for cloud IFEX specs
# Similar to reference-services/ifex generation

# Cloud Backend Transport Reference Service
add_subdirectory(cloud-backend-transport)

# Cloud Scheduler Reference Service (in-memory, for testing)
add_subdirectory(cloud-scheduler-service)
