#pragma once

#include <cstdint>

namespace ifex {

/// Content ID assignments for IFEX infrastructure services.
///
/// Content IDs are used by Backend Transport to route messages to the correct
/// MQTT topic (v2c/{vehicle_id}/{content_id} and c2v/{vehicle_id}/{content_id}).
///
/// Layout:
///   1-199:    Reserved (future use)
///   200-999:  Infrastructure services
///   1000+:    Application services

namespace content_id {

// Infrastructure services (200-999)
constexpr uint32_t DISPATCHER_RPC = 200;    ///< RPC forwarding (cloud → vehicle service calls)
constexpr uint32_t DISCOVERY_SYNC = 201;    ///< Discovery state sync (future)
constexpr uint32_t SCHEDULER_SYNC = 202;    ///< Scheduler state sync (future)

// Application range starts at 1000
constexpr uint32_t APP_BASE = 1000;

}  // namespace content_id

}  // namespace ifex
