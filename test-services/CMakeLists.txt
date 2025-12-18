# Test services for IFEX Core
# Note: This is included as a subdirectory, so packages are already found

# Option to build example services (settings, beverage, climate, etc.)
option(BUILD_EXAMPLE_SERVICES "Build example domain services" ON)

# =============================================================================
# Echo service (core test service - always built)
# =============================================================================
add_executable(ifex-echo-service
    echo/src/echo_server.cpp
    echo/src/main.cpp
)

target_include_directories(ifex-echo-service
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/echo/include
)

target_link_libraries(ifex-echo-service
    PRIVATE
        ifex-core
        ifex-proto-generated
        gRPC::grpc++
        protobuf::libprotobuf
        glog::glog
        nlohmann_json::nlohmann_json
)

# =============================================================================
# Example domain services (optional)
# =============================================================================
if(BUILD_EXAMPLE_SERVICES)
    # Settings service
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/settings/CMakeLists.txt)
        add_subdirectory(settings)
    endif()

    # Test types service
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test-types/CMakeLists.txt)
        add_subdirectory(test-types)
    endif()

    # Beverage service
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/beverage/CMakeLists.txt)
        add_subdirectory(beverage)
    endif()

    # Climate comfort service
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/climate-comfort/CMakeLists.txt)
        add_subdirectory(climate-comfort)
    endif()

    # Defrost service
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/defrost/CMakeLists.txt)
        add_subdirectory(defrost)
    endif()

    # Departure orchestration service
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/departure-orchestration/CMakeLists.txt)
        add_subdirectory(departure-orchestration)
    endif()
endif()

# Copy IFEX files to build directory as resources
file(GLOB_RECURSE TEST_IFEX_FILES "${CMAKE_CURRENT_SOURCE_DIR}/*/*.ifex.yml" "${CMAKE_CURRENT_SOURCE_DIR}/*/*.yml")
foreach(IFEX_FILE ${TEST_IFEX_FILES})
    get_filename_component(IFEX_NAME ${IFEX_FILE} NAME)
    configure_file(${IFEX_FILE} ${CMAKE_CURRENT_BINARY_DIR}/ifex/${IFEX_NAME} COPYONLY)
    # Also copy to the main binary directory for easier access
    configure_file(${IFEX_FILE} ${CMAKE_BINARY_DIR}/ifex/${IFEX_NAME} COPYONLY)
endforeach()