#pragma once

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <grpcpp/grpcpp.h>
#include <glog/logging.h>

#include "beverage-service.grpc.pb.h"
#include "service-discovery-service.grpc.pb.h"

namespace swdv {
namespace beverage_service {

struct PreparationInfo {
    std::string preparation_id;
    beverage_config_t config;
    preparation_state_t state;
    std::chrono::steady_clock::time_point start_time;
    std::chrono::steady_clock::time_point ready_time;
    uint32_t progress_percent;
};

class BeverageServiceImpl final : 
    public prepare_beverage_service::Service,
    public get_preparation_status_service::Service,
    public cancel_preparation_service::Service,
    public get_capabilities_service::Service {
public:
    BeverageServiceImpl();
    ~BeverageServiceImpl() = default;

    // Service methods
    grpc::Status prepare_beverage(
        grpc::ServerContext* context,
        const prepare_beverage_request* request,
        prepare_beverage_response* response) override;

    grpc::Status get_preparation_status(
        grpc::ServerContext* context,
        const get_preparation_status_request* request,
        get_preparation_status_response* response) override;

    grpc::Status cancel_preparation(
        grpc::ServerContext* context,
        const cancel_preparation_request* request,
        cancel_preparation_response* response) override;

    grpc::Status get_capabilities(
        grpc::ServerContext* context,
        const get_capabilities_request* request,
        get_capabilities_response* response) override;

    // Lifecycle
    void Start();
    void Stop();
    
    // Registration with discovery
    bool RegisterWithDiscovery(const std::string& discovery_endpoint, 
                              int port, 
                              const std::string& ifex_schema);

private:
    // Helper methods
    uint32_t CalculateBrewTime(const beverage_config_t& config) const;
    std::string GeneratePreparationId();
    void UpdatePreparationProgress();
    
    // State
    mutable std::mutex state_mutex_;
    std::map<std::string, std::unique_ptr<PreparationInfo>> preparations_;
    PreparationInfo* current_preparation_ = nullptr;
    
    // Simulated hardware state
    std::atomic<uint8_t> water_level_percent_{80};
    std::atomic<bool> requires_maintenance_{false};
    
    // Background thread for preparation updates
    std::atomic<bool> running_{false};
    std::thread update_thread_;
    
    // ID generation
    std::atomic<uint32_t> preparation_counter_{0};
    
    // Registration ID from discovery service
    std::string registration_id_;
};

} // namespace beverage_service
} // namespace swdv