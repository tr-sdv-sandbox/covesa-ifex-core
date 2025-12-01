#include "climate_comfort_server.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <fstream>

#include "service-discovery-service.grpc.pb.h"
#include "ifex/network.hpp"

namespace swdv {
namespace climate_comfort_service {

using namespace std::chrono_literals;

ClimateComfortServiceImpl::ClimateComfortServiceImpl() 
    : current_mode_(comfort_mode_t::COMFORT),
      air_quality_mode_(air_quality_mode_t::AUTO) {
    
    // Initialize zones
    zones_["driver"] = ZoneState{};
    zones_["passenger"] = ZoneState{};
    zones_["rear_left"] = ZoneState{};
    zones_["rear_right"] = ZoneState{};
}

void ClimateComfortServiceImpl::Start() {
    running_ = true;
    update_thread_ = std::thread([this]() {
        while (running_) {
            UpdateZoneTemperatures();
            std::this_thread::sleep_for(500ms);
        }
    });
    LOG(INFO) << "Climate comfort service started";
}

void ClimateComfortServiceImpl::Stop() {
    running_ = false;
    if (update_thread_.joinable()) {
        update_thread_.join();
    }
    LOG(INFO) << "Climate comfort service stopped";
}

grpc::Status ClimateComfortServiceImpl::set_comfort(
    grpc::ServerContext* context,
    const set_comfort_request* request,
    set_comfort_response* response) {
    
    std::lock_guard<std::mutex> lock(state_mutex_);
    
    const auto& comfort_request = request->request();
    
    // Apply comfort mode
    current_mode_ = comfort_request.mode();
    ApplyComfortMode(current_mode_);
    
    // Apply zone preset
    ApplyZonePreset(comfort_request.zone_preset(), comfort_request.target_temperature());
    
    // Set air quality mode
    air_quality_mode_ = comfort_request.air_quality_mode();
    
    // Simulate VSS writes
    uint32_t vss_signals = SimulateVSSWrites(comfort_request);
    
    // Calculate time to comfort
    uint32_t time_to_comfort = EstimateTimeToComfort(comfort_request.target_temperature());
    
    response->set_accepted(true);
    response->set_estimated_time_seconds(time_to_comfort);
    response->set_vss_signals_modified(vss_signals);
    
    achieving_target_ = true;
    
    LOG(INFO) << "Set comfort mode to " << static_cast<int>(current_mode_)
              << " with target temperature " << comfort_request.target_temperature();
    
    return grpc::Status::OK;
}

grpc::Status ClimateComfortServiceImpl::quick_comfort_adjustment(
    grpc::ServerContext* context,
    const quick_comfort_adjustment_request* request,
    quick_comfort_adjustment_response* response) {
    
    std::lock_guard<std::mutex> lock(state_mutex_);
    
    float adjustment_delta = request->intensity() * 0.5f; // 0.5°C per intensity level
    std::stringstream new_settings;
    
    if (request->adjustment() == "warmer") {
        for (auto& [name, zone] : zones_) {
            zone.target_temperature = std::min(zone.target_temperature + adjustment_delta, 28.0f);
        }
        new_settings << "Increased temperature by " << adjustment_delta << "°C";
        
    } else if (request->adjustment() == "cooler") {
        for (auto& [name, zone] : zones_) {
            zone.target_temperature = std::max(zone.target_temperature - adjustment_delta, 16.0f);
        }
        new_settings << "Decreased temperature by " << adjustment_delta << "°C";
        
    } else if (request->adjustment() == "quieter") {
        for (auto& [name, zone] : zones_) {
            zone.fan_speed = std::max(1u, zone.fan_speed - request->intensity());
        }
        current_mode_ = comfort_mode_t::QUIET;
        new_settings << "Reduced fan speed, switched to quiet mode";
        
    } else if (request->adjustment() == "fresher") {
        air_quality_mode_ = air_quality_mode_t::FRESH_AIR;
        new_settings << "Switched to fresh air intake mode";
        
    } else {
        response->set_success(false);
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, 
                          "Unknown adjustment type: " + request->adjustment());
    }
    
    response->set_success(true);
    response->set_new_settings(new_settings.str());
    
    LOG(INFO) << "Applied quick adjustment: " << request->adjustment() 
              << " with intensity " << request->intensity();
    
    return grpc::Status::OK;
}

grpc::Status ClimateComfortServiceImpl::optimize_for_efficiency(
    grpc::ServerContext* context,
    const optimize_for_efficiency_request* request,
    optimize_for_efficiency_response* response) {
    
    std::lock_guard<std::mutex> lock(state_mutex_);
    
    std::vector<std::string> optimizations;
    uint8_t improvement = 0;
    
    // Switch to ECO mode
    if (current_mode_ != comfort_mode_t::ECO) {
        current_mode_ = comfort_mode_t::ECO;
        ApplyComfortMode(current_mode_);
        optimizations.push_back("Switched to ECO mode");
        improvement += 15;
    }
    
    // Reduce temperature differences between zones
    float avg_temp = CalculateAverageCabinTemp();
    for (auto& [name, zone] : zones_) {
        if (std::abs(zone.target_temperature - avg_temp) > 2.0f) {
            zone.target_temperature = avg_temp;
            optimizations.push_back("Normalized " + name + " zone temperature");
            improvement += 5;
        }
    }
    
    // Enable recirculation
    if (air_quality_mode_ != air_quality_mode_t::RECIRCULATE) {
        air_quality_mode_ = air_quality_mode_t::RECIRCULATE;
        optimizations.push_back("Enabled air recirculation");
        improvement += 10;
    }
    
    // Reduce fan speeds
    for (auto& [name, zone] : zones_) {
        if (zone.fan_speed > 5) {
            zone.fan_speed = 5;
            optimizations.push_back("Reduced " + name + " fan speed");
            improvement += 3;
        }
    }
    
    if (request->maintain_minimum_comfort()) {
        // Ensure temperatures stay within comfort range
        for (auto& [name, zone] : zones_) {
            zone.target_temperature = std::clamp(zone.target_temperature, 20.0f, 24.0f);
        }
    }
    
    for (const auto& opt : optimizations) {
        response->add_optimizations_applied(opt);
    }
    response->set_efficiency_improvement_percent(std::min(improvement, uint8_t(40)));
    
    efficiency_score_ = std::min(100, efficiency_score_.load() + improvement);
    
    LOG(INFO) << "Applied " << optimizations.size() << " efficiency optimizations";
    
    return grpc::Status::OK;
}

grpc::Status ClimateComfortServiceImpl::get_comfort_status(
    grpc::ServerContext* context,
    const get_comfort_status_request* request,
    get_comfort_status_response* response) {
    
    std::lock_guard<std::mutex> lock(state_mutex_);
    
    auto* status = response->mutable_status();
    status->set_current_mode(current_mode_);
    status->set_average_cabin_temp(CalculateAverageCabinTemp());
    status->set_achieving_target(achieving_target_.load());
    status->set_efficiency_score(efficiency_score_.load());
    
    // Check if we're close to target
    bool all_zones_comfortable = true;
    for (const auto& [name, zone] : zones_) {
        if (std::abs(zone.actual_temperature - zone.target_temperature) > 1.0f) {
            all_zones_comfortable = false;
            break;
        }
    }
    
    if (all_zones_comfortable) {
        status->set_estimated_time_to_comfort(0);
        achieving_target_ = false;
    } else {
        status->set_estimated_time_to_comfort(EstimateTimeToComfort(zones_["driver"].target_temperature));
    }
    
    // Add zone temperatures
    for (const auto& [name, zone] : zones_) {
        response->add_zone_temperatures(zone.actual_temperature);
    }
    
    return grpc::Status::OK;
}

grpc::Status ClimateComfortServiceImpl::prepare_for_occupancy(
    grpc::ServerContext* context,
    const prepare_for_occupancy_request* request,
    prepare_for_occupancy_response* response) {
    
    std::lock_guard<std::mutex> lock(state_mutex_);
    
    // Apply the target comfort settings
    const auto& target = request->target_comfort();
    current_mode_ = target.mode();
    ApplyComfortMode(current_mode_);
    ApplyZonePreset(target.zone_preset(), target.target_temperature());
    air_quality_mode_ = target.air_quality_mode();
    
    // Calculate if we can achieve comfort in time
    uint32_t time_needed = EstimateTimeToComfort(target.target_temperature());
    bool ready_in_time = time_needed <= request->expected_occupancy_seconds();
    
    // If we have extra time, use ECO mode initially
    if (request->expected_occupancy_seconds() > time_needed + 300) { // 5 min buffer
        current_mode_ = comfort_mode_t::ECO;
        ApplyComfortMode(current_mode_);
        LOG(INFO) << "Using ECO mode initially, will switch to target mode closer to occupancy";
    }
    
    response->set_preparation_started(true);
    response->set_ready_by_time(ready_in_time);
    
    achieving_target_ = true;
    
    LOG(INFO) << "Preparing for occupancy in " << request->expected_occupancy_seconds() 
              << " seconds, need " << time_needed << " seconds";
    
    return grpc::Status::OK;
}

void ClimateComfortServiceImpl::ApplyComfortMode(comfort_mode_t mode) {
    switch (mode) {
        case comfort_mode_t::ECO:
            // Reduce fan speeds, allow wider temperature range
            for (auto& [name, zone] : zones_) {
                zone.fan_speed = std::min(zone.fan_speed, uint8_t(4));
            }
            efficiency_score_ = 85;
            break;
            
        case comfort_mode_t::COMFORT:
            // Balanced settings
            for (auto& [name, zone] : zones_) {
                zone.fan_speed = 6;
            }
            efficiency_score_ = 70;
            break;
            
        case comfort_mode_t::PERFORMANCE:
            // Maximum comfort, ignore efficiency
            for (auto& [name, zone] : zones_) {
                zone.fan_speed = 8;
            }
            efficiency_score_ = 40;
            break;
            
        case comfort_mode_t::QUIET:
            // Minimize noise
            for (auto& [name, zone] : zones_) {
                zone.fan_speed = 3;
            }
            efficiency_score_ = 75;
            break;
    }
}

void ClimateComfortServiceImpl::ApplyZonePreset(zone_preset_t preset, float target_temp) {
    switch (preset) {
        case zone_preset_t::ALL_ZONES:
            for (auto& [name, zone] : zones_) {
                zone.target_temperature = target_temp;
            }
            break;
            
        case zone_preset_t::DRIVER_ONLY:
            zones_["driver"].target_temperature = target_temp;
            // Turn off other zones
            zones_["passenger"].target_temperature = zones_["passenger"].actual_temperature;
            zones_["rear_left"].target_temperature = zones_["rear_left"].actual_temperature;
            zones_["rear_right"].target_temperature = zones_["rear_right"].actual_temperature;
            break;
            
        case zone_preset_t::FRONT_ONLY:
            zones_["driver"].target_temperature = target_temp;
            zones_["passenger"].target_temperature = target_temp;
            zones_["rear_left"].target_temperature = zones_["rear_left"].actual_temperature;
            zones_["rear_right"].target_temperature = zones_["rear_right"].actual_temperature;
            break;
            
        case zone_preset_t::INDIVIDUAL:
            // Keep current individual settings
            break;
    }
}

uint32_t ClimateComfortServiceImpl::SimulateVSSWrites(const comfort_request_t& request) {
    uint32_t signal_count = 0;
    
    // HVAC mode
    LOG(INFO) << "VSS Write: " << VSSPaths::HVAC_MODE << " = " 
              << static_cast<int>(request.mode());
    signal_count++;
    
    // Zone temperatures
    LOG(INFO) << "VSS Write: " << VSSPaths::DRIVER_TEMP_TARGET << " = " 
              << zones_["driver"].target_temperature;
    signal_count++;
    
    LOG(INFO) << "VSS Write: " << VSSPaths::PASSENGER_TEMP_TARGET << " = " 
              << zones_["passenger"].target_temperature;
    signal_count++;
    
    // Air quality
    LOG(INFO) << "VSS Write: " << VSSPaths::RECIRCULATION << " = " 
              << (air_quality_mode_ == air_quality_mode_t::RECIRCULATE ? "true" : "false");
    signal_count++;
    
    // Fan speeds
    LOG(INFO) << "VSS Write: " << VSSPaths::FAN_SPEED << " = " 
              << static_cast<int>(zones_["driver"].fan_speed);
    signal_count++;
    
    return signal_count;
}

void ClimateComfortServiceImpl::UpdateZoneTemperatures() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    
    // Simulate temperature changes
    for (auto& [name, zone] : zones_) {
        float diff = zone.target_temperature - zone.actual_temperature;
        float change_rate = 0.1f * zone.fan_speed / 5.0f; // Higher fan = faster change
        
        if (std::abs(diff) > 0.1f) {
            zone.actual_temperature += std::copysign(change_rate, diff);
            zone.actual_temperature = std::clamp(zone.actual_temperature, 16.0f, 28.0f);
        }
        
        zone.last_update = std::chrono::steady_clock::now();
    }
}

float ClimateComfortServiceImpl::CalculateAverageCabinTemp() const {
    float sum = 0.0f;
    for (const auto& [name, zone] : zones_) {
        sum += zone.actual_temperature;
    }
    return sum / zones_.size();
}

uint32_t ClimateComfortServiceImpl::EstimateTimeToComfort(float target_temp) const {
    float max_diff = 0.0f;
    
    for (const auto& [name, zone] : zones_) {
        float diff = std::abs(target_temp - zone.actual_temperature);
        max_diff = std::max(max_diff, diff);
    }
    
    // Estimate based on temperature difference and mode
    float base_time = max_diff * 60.0f; // 60 seconds per degree
    
    switch (current_mode_) {
        case comfort_mode_t::ECO:
            base_time *= 1.5f; // Slower in ECO mode
            break;
        case comfort_mode_t::PERFORMANCE:
            base_time *= 0.7f; // Faster in performance mode
            break;
        default:
            break;
    }
    
    return static_cast<uint32_t>(base_time);
}

bool ClimateComfortServiceImpl::RegisterWithDiscovery(
    const std::string& discovery_endpoint,
    int port,
    const std::string& ifex_schema) {
    
    try {
        // Get primary IP address
        std::string primary_ip = ifex::network::get_primary_ip_address();
        LOG(INFO) << "Primary IP address: " << primary_ip;
        
        // Create discovery service channel and stub
        auto channel = grpc::CreateChannel(discovery_endpoint, 
                                         grpc::InsecureChannelCredentials());
        auto stub = swdv::service_discovery::register_service_service::NewStub(channel);
        
        // Create service info
        auto* service_info = new swdv::service_discovery::service_info_t();
        service_info->set_name("climate_comfort_service");
        service_info->set_version("1.0.0");
        service_info->set_description("High-level climate comfort orchestration service");
        service_info->set_status(swdv::service_discovery::service_status_t::AVAILABLE);
        
        // Set endpoint
        auto* endpoint = service_info->mutable_endpoint();
        endpoint->set_transport(swdv::service_discovery::transport_type_t::GRPC);
        std::string endpoint_address = primary_ip + ":" + std::to_string(port);
        endpoint->set_address(endpoint_address);
        
        LOG(INFO) << "Registering with endpoint: " << endpoint_address;
        
        // Load and set IFEX schema if provided
        if (!ifex_schema.empty()) {
            std::ifstream schema_file(ifex_schema);
            if (schema_file.is_open()) {
                std::string schema_content((std::istreambuf_iterator<char>(schema_file)),
                                         std::istreambuf_iterator<char>());
                service_info->set_ifex_schema(schema_content);
                LOG(INFO) << "Loaded IFEX schema from: " << ifex_schema;
            } else {
                LOG(WARNING) << "Could not open IFEX schema file: " << ifex_schema;
            }
        }
        
        // Create request
        swdv::service_discovery::register_service_request request;
        request.set_allocated_service_info(service_info);
        
        // Make the call
        swdv::service_discovery::register_service_response response;
        grpc::ClientContext context;
        grpc::Status status = stub->register_service(&context, request, &response);
        
        if (status.ok()) {
            registration_id_ = response.registration_id();
            LOG(INFO) << "Successfully registered with service discovery, ID: " << registration_id_;
            
            // Start heartbeat thread
            heartbeat_running_ = true;
            heartbeat_thread_ = std::make_unique<std::thread>(
                &ClimateComfortServiceImpl::HeartbeatLoop, this, discovery_endpoint);
            
            return true;
        } else {
            LOG(ERROR) << "Failed to register with service discovery: " 
                      << status.error_code() << ": " << status.error_message();
            return false;
        }
    } catch (const std::exception& e) {
        LOG(ERROR) << "Exception during service registration: " << e.what();
        return false;
    }
}

void ClimateComfortServiceImpl::HeartbeatLoop(const std::string& discovery_endpoint) {
    auto channel = grpc::CreateChannel(discovery_endpoint, 
                                     grpc::InsecureChannelCredentials());
    auto stub = swdv::service_discovery::heartbeat_service::NewStub(channel);
    
    while (heartbeat_running_) {
        swdv::service_discovery::heartbeat_request request;
        request.set_registration_id(registration_id_);
        request.set_status(swdv::service_discovery::service_status_t::AVAILABLE);
        
        swdv::service_discovery::heartbeat_response response;
        grpc::ClientContext context;
        
        auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(5);
        context.set_deadline(deadline);
        
        grpc::Status status = stub->heartbeat(&context, request, &response);
        
        if (!status.ok()) {
            LOG(WARNING) << "Heartbeat failed: " << status.error_code() 
                        << ": " << status.error_message();
        }
        
        // Sleep for 10 seconds before next heartbeat
        for (int i = 0; i < 10 && heartbeat_running_; ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
}

} // namespace climate_comfort_service
} // namespace swdv