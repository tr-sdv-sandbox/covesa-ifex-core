#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include <glog/logging.h>
#include <grpcpp/grpcpp.h>

#include "climate-comfort-service.grpc.pb.h"
#include "climate-comfort-service.pb.h"

namespace swdv {
namespace climate_comfort_service {

// VSS paths for climate control
namespace VSSPaths {
    constexpr const char* HVAC_MODE = "Vehicle.Cabin.HVAC.Mode";
    constexpr const char* DRIVER_TEMP_TARGET = "Vehicle.Cabin.HVAC.Station.Row1.Left.Temperature";
    constexpr const char* PASSENGER_TEMP_TARGET = "Vehicle.Cabin.HVAC.Station.Row1.Right.Temperature";
    constexpr const char* REAR_LEFT_TEMP_TARGET = "Vehicle.Cabin.HVAC.Station.Row2.Left.Temperature";
    constexpr const char* REAR_RIGHT_TEMP_TARGET = "Vehicle.Cabin.HVAC.Station.Row2.Right.Temperature";
    constexpr const char* RECIRCULATION = "Vehicle.Cabin.HVAC.IsRecirculationActive";
    constexpr const char* FAN_SPEED = "Vehicle.Cabin.HVAC.Station.Row1.Left.FanSpeed";
}

class ClimateComfortServiceImpl final :
    public set_comfort_service::Service,
    public quick_comfort_adjustment_service::Service,
    public optimize_for_efficiency_service::Service,
    public get_comfort_status_service::Service,
    public prepare_for_occupancy_service::Service {
public:
    ClimateComfortServiceImpl();
    ~ClimateComfortServiceImpl() = default;
    
    void Start();
    void Stop();
    
    // Service methods
    grpc::Status set_comfort(
        grpc::ServerContext* context,
        const set_comfort_request* request,
        set_comfort_response* response) override;
        
    grpc::Status quick_comfort_adjustment(
        grpc::ServerContext* context,
        const quick_comfort_adjustment_request* request,
        quick_comfort_adjustment_response* response) override;
        
    grpc::Status optimize_for_efficiency(
        grpc::ServerContext* context,
        const optimize_for_efficiency_request* request,
        optimize_for_efficiency_response* response) override;
        
    grpc::Status get_comfort_status(
        grpc::ServerContext* context,
        const get_comfort_status_request* request,
        get_comfort_status_response* response) override;
        
    grpc::Status prepare_for_occupancy(
        grpc::ServerContext* context,
        const prepare_for_occupancy_request* request,
        prepare_for_occupancy_response* response) override;

    // Service discovery registration
    bool RegisterWithDiscovery(const std::string& discovery_endpoint, 
                             int port,
                             const std::string& ifex_schema);

private:
    struct ZoneState {
        float actual_temperature = 22.0f;
        float target_temperature = 22.0f;
        uint8_t fan_speed = 5;  // 0-10
        std::chrono::steady_clock::time_point last_update;
    };
    
    // State management
    std::mutex state_mutex_;
    std::unordered_map<std::string, ZoneState> zones_;
    comfort_mode_t current_mode_;
    air_quality_mode_t air_quality_mode_;
    std::atomic<bool> achieving_target_{false};
    std::atomic<uint8_t> efficiency_score_{70};
    
    // Background thread for simulation
    std::atomic<bool> running_{false};
    std::thread update_thread_;
    
    // Helper methods
    void ApplyComfortMode(comfort_mode_t mode);
    void ApplyZonePreset(zone_preset_t preset, float target_temp);
    uint32_t SimulateVSSWrites(const comfort_request_t& request);
    void UpdateZoneTemperatures();
    float CalculateAverageCabinTemp() const;
    uint32_t EstimateTimeToComfort(float target_temp) const;
    
    // Service discovery
    std::string registration_id_;
    std::unique_ptr<std::thread> heartbeat_thread_;
    std::atomic<bool> heartbeat_running_{false};
    void HeartbeatLoop(const std::string& discovery_endpoint);
};

} // namespace climate_comfort_service
} // namespace swdv