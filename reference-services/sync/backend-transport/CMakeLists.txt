cmake_minimum_required(VERSION 3.16)

if(TARGET ifex-sync-backend-transport-bridge)
    return()
endif()

add_library(ifex-sync-backend-transport-bridge STATIC
    src/cloud_vehicle_sync_bridge.cpp
)

target_include_directories(ifex-sync-backend-transport-bridge
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(ifex-sync-backend-transport-bridge
    PUBLIC
        ifex-sync-core
        ifex-backend-transport-client
        ifex-cloud-backend-transport-client
        ifex-proto-generated
        gRPC::grpc++
)

target_compile_features(ifex-sync-backend-transport-bridge PUBLIC cxx_std_17)

add_library(ifex::sync-backend-transport-bridge ALIAS ifex-sync-backend-transport-bridge)
