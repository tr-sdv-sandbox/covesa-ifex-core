#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <glog/logging.h>
#include <grpcpp/grpcpp.h>

#include "defrost-service.grpc.pb.h"
#include "defrost-service.pb.h"

namespace swdv {
namespace defrost_service {

// VSS paths for defrost control
namespace VSSPaths {
    constexpr const char* WINDSHIELD_DEFROST = "Vehicle.Body.Windshield.Front.Defrost.Status";
    constexpr const char* REAR_WINDOW_DEFROST = "Vehicle.Body.Windshield.Rear.Defrost.Status";
    constexpr const char* MIRROR_DEFROST_LEFT = "Vehicle.Body.Mirrors.Left.Defrost.Status";
    constexpr const char* MIRROR_DEFROST_RIGHT = "Vehicle.Body.Mirrors.Right.Defrost.Status";
    constexpr const char* WINDSHIELD_HEATER_LEVEL = "Vehicle.Body.Windshield.Front.Defrost.Level";
    constexpr const char* REAR_HEATER_LEVEL = "Vehicle.Body.Windshield.Rear.Defrost.Level";
}

class DefrostServiceImpl final :
    public start_defrost_service::Service,
    public stop_defrost_service::Service,
    public get_visibility_status_service::Service,
    public quick_clear_windshield_service::Service {
    
public:
    DefrostServiceImpl();
    ~DefrostServiceImpl() = default;
    
    void Start();
    void Stop();
    
    // Service methods
    grpc::Status start_defrost(
        grpc::ServerContext* context,
        const start_defrost_request* request,
        start_defrost_response* response) override;
        
    grpc::Status stop_defrost(
        grpc::ServerContext* context,
        const stop_defrost_request* request,
        stop_defrost_response* response) override;
        
    grpc::Status get_visibility_status(
        grpc::ServerContext* context,
        const get_visibility_status_request* request,
        get_visibility_status_response* response) override;
        
    grpc::Status quick_clear_windshield(
        grpc::ServerContext* context,
        const quick_clear_windshield_request* request,
        quick_clear_windshield_response* response) override;

    // Service discovery registration
    bool RegisterWithDiscovery(const std::string& discovery_endpoint, 
                             int port,
                             const std::string& ifex_schema);
    
private:
    struct DefrostOperation {
        std::string id;
        defrost_mode_t mode;
        std::chrono::steady_clock::time_point start_time;
        std::chrono::seconds max_duration;
        bool auto_stop_when_clear;
        bool active;
    };
    
    struct SurfaceState {
        visibility_status_t visibility;
        bool heater_active;
        float temperature_celsius;
        uint8_t heater_level;  // 0-100%
    };
    
    // State management
    std::mutex state_mutex_;
    std::unordered_map<surface_t, SurfaceState> surface_states_;
    std::vector<DefrostOperation> operations_;
    std::atomic<uint32_t> operation_counter_{0};
    
    // Background thread for simulation
    std::atomic<bool> running_{false};
    std::thread update_thread_;
    
    // Helper methods
    void InitializeSurfaces();
    void UpdateSurfaceStates();
    void ApplyDefrostMode(defrost_mode_t mode);
    void DeactivateAllHeaters();
    bool CheckVisibilityClear();
    uint32_t EstimateTimeToVisibility(visibility_status_t current);
    std::string GenerateOperationId();
    uint32_t SimulateVSSWrites(defrost_mode_t mode);
    
    // Service discovery
    std::string registration_id_;
    std::unique_ptr<std::thread> heartbeat_thread_;
    std::atomic<bool> heartbeat_running_{false};
    void HeartbeatLoop(const std::string& discovery_endpoint);
};

} // namespace defrost_service
} // namespace swdv