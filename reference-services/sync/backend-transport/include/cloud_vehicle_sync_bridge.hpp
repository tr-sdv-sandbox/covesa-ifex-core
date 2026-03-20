#pragma once

#include "../../common/include/cloud_vehicle_db_adapter.hpp"
#include "../../common/include/cloud_vehicle_sync_types.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace ifex {
namespace sync {
namespace bridge {

struct BridgeStats {
    std::uint64_t sync_messages_sent = 0;
    std::uint64_t sync_messages_received = 0;
    std::uint64_t checkpoint_messages_sent = 0;
    std::uint64_t checkpoint_messages_received = 0;
    std::uint64_t gap_requests_sent = 0;
    std::uint64_t gap_requests_received = 0;
    std::uint64_t gap_responses_sent = 0;
    std::uint64_t gap_responses_received = 0;
    std::uint64_t records_applied = 0;
    std::uint64_t records_duplicated = 0;
    std::uint64_t records_conflicted = 0;
};

struct CommonBridgeConfig {
    std::string local_node_id;
    std::string remote_node_id;
    std::string namespace_name;
    std::uint32_t content_id = 202;
    std::uint32_t poll_interval_ms = 250;
    std::uint32_t heartbeat_interval_ms = 1000;
    std::size_t max_batch_records = 64;
    std::shared_ptr<CloudVehicleDbAdapter> adapter;
};

struct CloudBridgeConfig {
    CommonBridgeConfig common;
    std::string cloud_transport_address = "localhost:50100";
    std::string vehicle_id = "vehicle-001";
};

struct TruckBridgeConfig {
    CommonBridgeConfig common;
    std::string backend_transport_address = "localhost:50060";
};

class CloudVehicleCloudBridge {
public:
    explicit CloudVehicleCloudBridge(CloudBridgeConfig config);
    ~CloudVehicleCloudBridge();

    CloudVehicleCloudBridge(const CloudVehicleCloudBridge&) = delete;
    CloudVehicleCloudBridge& operator=(const CloudVehicleCloudBridge&) = delete;

    bool Start();
    void Stop();
    bool IsRunning() const;
    void ForceSync();
    BridgeStats GetStats() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

class CloudVehicleTruckBridge {
public:
    explicit CloudVehicleTruckBridge(TruckBridgeConfig config);
    ~CloudVehicleTruckBridge();

    CloudVehicleTruckBridge(const CloudVehicleTruckBridge&) = delete;
    CloudVehicleTruckBridge& operator=(const CloudVehicleTruckBridge&) = delete;

    bool Start();
    void Stop();
    bool IsRunning() const;
    void ForceSync();
    BridgeStats GetStats() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}
}
}
