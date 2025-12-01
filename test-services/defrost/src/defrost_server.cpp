#include "defrost_server.hpp"

#include <algorithm>
#include <sstream>
#include <fstream>
#include <iomanip>

#include "service-discovery-service.grpc.pb.h"
#include "ifex/network.hpp"

namespace swdv {
namespace defrost_service {

using namespace std::chrono_literals;

DefrostServiceImpl::DefrostServiceImpl() {
    InitializeSurfaces();
}

void DefrostServiceImpl::Start() {
    running_ = true;
    update_thread_ = std::thread([this]() {
        while (running_) {
            UpdateSurfaceStates();
            std::this_thread::sleep_for(500ms);
        }
    });
    LOG(INFO) << "Defrost service started";
}

void DefrostServiceImpl::Stop() {
    running_ = false;
    if (update_thread_.joinable()) {
        update_thread_.join();
    }
    
    heartbeat_running_ = false;
    if (heartbeat_thread_ && heartbeat_thread_->joinable()) {
        heartbeat_thread_->join();
    }
    
    LOG(INFO) << "Defrost service stopped";
}

grpc::Status DefrostServiceImpl::start_defrost(
    grpc::ServerContext* context,
    const start_defrost_request* request,
    start_defrost_response* response) {
    
    std::lock_guard<std::mutex> lock(state_mutex_);
    
    const auto& defrost_request = request->request();
    
    // Generate operation ID
    std::string op_id = GenerateOperationId();
    
    // Check if we already have an active defrost operation
    bool has_active = std::any_of(operations_.begin(), operations_.end(),
        [](const DefrostOperation& op) { return op.active; });
    
    if (has_active && defrost_request.mode() != defrost_mode_t::OFF) {
        response->set_accepted(false);
        response->set_operation_id("");
        response->set_estimated_clear_time_seconds(0);
        LOG(WARNING) << "Defrost request rejected - operation already in progress";
        return grpc::Status::OK;
    }
    
    // Create new operation
    DefrostOperation operation;
    operation.id = op_id;
    operation.mode = defrost_request.mode();
    operation.start_time = std::chrono::steady_clock::now();
    operation.max_duration = std::chrono::minutes(defrost_request.max_duration_minutes());
    operation.auto_stop_when_clear = defrost_request.auto_stop_when_clear();
    operation.active = (defrost_request.mode() != defrost_mode_t::OFF);
    
    operations_.push_back(operation);
    
    // Apply the defrost mode
    ApplyDefrostMode(defrost_request.mode());
    
    // Simulate VSS writes
    uint32_t vss_signals = SimulateVSSWrites(defrost_request.mode());
    
    // Estimate time to clear
    uint32_t time_to_clear = 0;
    if (defrost_request.mode() != defrost_mode_t::OFF) {
        float max_time = 0;
        for (const auto& [surface, state] : surface_states_) {
            uint32_t surface_time = EstimateTimeToVisibility(state.visibility);
            max_time = std::max(max_time, static_cast<float>(surface_time));
        }
        
        // Adjust based on mode
        switch (defrost_request.mode()) {
            case defrost_mode_t::QUICK_CLEAR:
                max_time *= 0.7f;  // 30% faster
                break;
            case defrost_mode_t::WINDSHIELD_ONLY:
                max_time *= 0.8f;  // Focused heat
                break;
            default:
                break;
        }
        
        time_to_clear = static_cast<uint32_t>(max_time);
    }
    
    response->set_operation_id(op_id);
    response->set_accepted(operation.active);
    response->set_estimated_clear_time_seconds(time_to_clear);
    
    LOG(INFO) << "Started defrost operation " << op_id 
              << " mode: " << static_cast<int>(defrost_request.mode())
              << " estimated time: " << time_to_clear << "s";
    
    return grpc::Status::OK;
}

grpc::Status DefrostServiceImpl::stop_defrost(
    grpc::ServerContext* context,
    const stop_defrost_request* request,
    stop_defrost_response* response) {
    
    std::lock_guard<std::mutex> lock(state_mutex_);
    
    bool success = false;
    
    if (request->operation_id().empty()) {
        // Stop all active operations
        for (auto& op : operations_) {
            if (op.active) {
                op.active = false;
                success = true;
            }
        }
        DeactivateAllHeaters();
    } else {
        // Stop specific operation
        auto it = std::find_if(operations_.begin(), operations_.end(),
            [&request](const DefrostOperation& op) { 
                return op.id == request->operation_id() && op.active; 
            });
        
        if (it != operations_.end()) {
            it->active = false;
            success = true;
            
            // Check if any other operations are still active
            bool any_active = std::any_of(operations_.begin(), operations_.end(),
                [](const DefrostOperation& op) { return op.active; });
            
            if (!any_active) {
                DeactivateAllHeaters();
            }
        }
    }
    
    response->set_success(success);
    
    LOG(INFO) << "Stop defrost " << (request->operation_id().empty() ? "all" : request->operation_id())
              << " success: " << success;
    
    return grpc::Status::OK;
}

grpc::Status DefrostServiceImpl::get_visibility_status(
    grpc::ServerContext* context,
    const get_visibility_status_request* request,
    get_visibility_status_response* response) {
    
    std::lock_guard<std::mutex> lock(state_mutex_);
    
    // Add all surface statuses
    for (const auto& [surface, state] : surface_states_) {
        auto* status = response->add_surfaces();
        status->set_surface(surface);
        status->set_visibility(state.visibility);
        status->set_heater_active(state.heater_active);
        status->set_temperature_celsius(state.temperature_celsius);
    }
    
    // Determine overall visibility safety
    bool safe_to_drive = true;
    
    // Front windshield must be at least light fog or better
    auto windshield_it = surface_states_.find(surface_t::FRONT_WINDSHIELD);
    if (windshield_it != surface_states_.end()) {
        if (windshield_it->second.visibility > visibility_status_t::LIGHT_FOG) {
            safe_to_drive = false;
        }
    }
    
    // Side mirrors should be clear or light fog
    auto left_mirror_it = surface_states_.find(surface_t::SIDE_MIRRORS);
    if (left_mirror_it != surface_states_.end()) {
        if (left_mirror_it->second.visibility > visibility_status_t::LIGHT_FOG) {
            safe_to_drive = false;
        }
    }
    
    response->set_overall_visibility_safe(safe_to_drive);
    
    return grpc::Status::OK;
}

grpc::Status DefrostServiceImpl::quick_clear_windshield(
    grpc::ServerContext* context,
    const quick_clear_windshield_request* request,
    quick_clear_windshield_response* response) {
    
    std::lock_guard<std::mutex> lock(state_mutex_);
    
    // Check if quick clear is already active
    bool already_active = std::any_of(operations_.begin(), operations_.end(),
        [](const DefrostOperation& op) { 
            return op.active && op.mode == defrost_mode_t::QUICK_CLEAR; 
        });
    
    if (already_active) {
        response->set_activated(false);
        response->set_estimated_clear_seconds(0);
        return grpc::Status::OK;
    }
    
    // Start quick clear
    std::string op_id = GenerateOperationId();
    
    DefrostOperation operation;
    operation.id = op_id;
    operation.mode = defrost_mode_t::QUICK_CLEAR;
    operation.start_time = std::chrono::steady_clock::now();
    operation.max_duration = std::chrono::minutes(5);  // Quick clear is limited to 5 minutes
    operation.auto_stop_when_clear = true;
    operation.active = true;
    
    operations_.push_back(operation);
    
    // Apply quick clear mode
    ApplyDefrostMode(defrost_mode_t::QUICK_CLEAR);
    
    // Estimate time for just windshield
    auto windshield_it = surface_states_.find(surface_t::FRONT_WINDSHIELD);
    uint32_t clear_time = 60;  // Default 60 seconds
    if (windshield_it != surface_states_.end()) {
        clear_time = static_cast<uint32_t>(EstimateTimeToVisibility(windshield_it->second.visibility) * 0.7f);
    }
    
    response->set_activated(true);
    response->set_estimated_clear_seconds(clear_time);
    
    LOG(INFO) << "Quick clear windshield activated, estimated time: " << clear_time << "s";
    
    return grpc::Status::OK;
}

void DefrostServiceImpl::InitializeSurfaces() {
    // Initialize all surfaces with default states
    surface_states_[surface_t::FRONT_WINDSHIELD] = {
        visibility_status_t::HEAVY_FOG, false, 5.0f, 0
    };
    surface_states_[surface_t::REAR_WINDOW] = {
        visibility_status_t::LIGHT_FOG, false, 8.0f, 0
    };
    surface_states_[surface_t::SIDE_MIRRORS] = {
        visibility_status_t::FROST, false, 3.0f, 0
    };
    surface_states_[surface_t::SIDE_WINDOWS] = {
        visibility_status_t::LIGHT_FOG, false, 7.0f, 0
    };
}

void DefrostServiceImpl::UpdateSurfaceStates() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    
    // Update surface temperatures and visibility based on heater states
    for (auto& [surface, state] : surface_states_) {
        if (state.heater_active) {
            // Temperature increases when heater is on
            float heat_rate = 0.5f * (state.heater_level / 100.0f);
            state.temperature_celsius += heat_rate;
            state.temperature_celsius = std::min(state.temperature_celsius, 40.0f);
            
            // Improve visibility based on temperature
            if (state.temperature_celsius > 15.0f && state.visibility > visibility_status_t::CLEAR) {
                // Gradual improvement
                if (state.temperature_celsius > 30.0f) {
                    state.visibility = visibility_status_t::CLEAR;
                } else if (state.temperature_celsius > 25.0f && state.visibility > visibility_status_t::LIGHT_FOG) {
                    state.visibility = visibility_status_t::LIGHT_FOG;
                } else if (state.temperature_celsius > 20.0f && state.visibility > visibility_status_t::HEAVY_FOG) {
                    state.visibility = visibility_status_t::HEAVY_FOG;
                } else if (state.temperature_celsius > 15.0f && state.visibility == visibility_status_t::ICE) {
                    state.visibility = visibility_status_t::FROST;
                }
            }
        } else {
            // Temperature decreases when heater is off
            state.temperature_celsius -= 0.1f;
            state.temperature_celsius = std::max(state.temperature_celsius, -5.0f);
        }
    }
    
    // Check auto-stop conditions
    for (auto& op : operations_) {
        if (op.active && op.auto_stop_when_clear) {
            if (CheckVisibilityClear()) {
                op.active = false;
                DeactivateAllHeaters();
                LOG(INFO) << "Auto-stopping defrost operation " << op.id << " - visibility clear";
            }
        }
        
        // Check max duration
        if (op.active) {
            auto elapsed = std::chrono::steady_clock::now() - op.start_time;
            if (elapsed > op.max_duration) {
                op.active = false;
                DeactivateAllHeaters();
                LOG(INFO) << "Stopping defrost operation " << op.id << " - max duration reached";
            }
        }
    }
}

void DefrostServiceImpl::ApplyDefrostMode(defrost_mode_t mode) {
    // First deactivate all heaters
    DeactivateAllHeaters();
    
    switch (mode) {
        case defrost_mode_t::OFF:
            // Already deactivated
            break;
            
        case defrost_mode_t::WINDSHIELD_ONLY:
            surface_states_[surface_t::FRONT_WINDSHIELD].heater_active = true;
            surface_states_[surface_t::FRONT_WINDSHIELD].heater_level = 80;
            break;
            
        case defrost_mode_t::ALL_WINDOWS:
            for (auto& [surface, state] : surface_states_) {
                state.heater_active = true;
                state.heater_level = 60;
            }
            break;
            
        case defrost_mode_t::QUICK_CLEAR:
            // Maximum heat on critical surfaces
            surface_states_[surface_t::FRONT_WINDSHIELD].heater_active = true;
            surface_states_[surface_t::FRONT_WINDSHIELD].heater_level = 100;
            surface_states_[surface_t::SIDE_MIRRORS].heater_active = true;
            surface_states_[surface_t::SIDE_MIRRORS].heater_level = 100;
            surface_states_[surface_t::REAR_WINDOW].heater_active = true;
            surface_states_[surface_t::REAR_WINDOW].heater_level = 80;
            break;
    }
}

void DefrostServiceImpl::DeactivateAllHeaters() {
    for (auto& [surface, state] : surface_states_) {
        state.heater_active = false;
        state.heater_level = 0;
    }
}

bool DefrostServiceImpl::CheckVisibilityClear() {
    // Check if critical surfaces are clear
    auto windshield = surface_states_.find(surface_t::FRONT_WINDSHIELD);
    auto mirrors = surface_states_.find(surface_t::SIDE_MIRRORS);
    
    bool windshield_clear = (windshield != surface_states_.end() && 
                            windshield->second.visibility <= visibility_status_t::LIGHT_FOG);
    bool mirrors_clear = (mirrors != surface_states_.end() && 
                         mirrors->second.visibility <= visibility_status_t::LIGHT_FOG);
    
    return windshield_clear && mirrors_clear;
}

uint32_t DefrostServiceImpl::EstimateTimeToVisibility(visibility_status_t current) {
    switch (current) {
        case visibility_status_t::CLEAR:
            return 0;
        case visibility_status_t::LIGHT_FOG:
            return 30;  // 30 seconds
        case visibility_status_t::HEAVY_FOG:
            return 90;  // 1.5 minutes
        case visibility_status_t::FROST:
            return 180; // 3 minutes
        case visibility_status_t::ICE:
            return 300; // 5 minutes
        default:
            return 120; // 2 minutes default
    }
}

std::string DefrostServiceImpl::GenerateOperationId() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    
    std::stringstream ss;
    ss << "defrost_" << std::put_time(std::localtime(&time_t), "%Y%m%d_%H%M%S_")
       << std::setfill('0') << std::setw(4) << operation_counter_++;
    
    return ss.str();
}

uint32_t DefrostServiceImpl::SimulateVSSWrites(defrost_mode_t mode) {
    uint32_t signal_count = 0;
    
    // Log VSS writes based on mode
    switch (mode) {
        case defrost_mode_t::OFF:
            LOG(INFO) << "VSS Write: " << VSSPaths::WINDSHIELD_DEFROST << " = false";
            LOG(INFO) << "VSS Write: " << VSSPaths::REAR_WINDOW_DEFROST << " = false";
            LOG(INFO) << "VSS Write: " << VSSPaths::MIRROR_DEFROST_LEFT << " = false";
            LOG(INFO) << "VSS Write: " << VSSPaths::MIRROR_DEFROST_RIGHT << " = false";
            signal_count = 4;
            break;
            
        case defrost_mode_t::WINDSHIELD_ONLY:
            LOG(INFO) << "VSS Write: " << VSSPaths::WINDSHIELD_DEFROST << " = true";
            LOG(INFO) << "VSS Write: " << VSSPaths::WINDSHIELD_HEATER_LEVEL << " = 80";
            signal_count = 2;
            break;
            
        case defrost_mode_t::ALL_WINDOWS:
            LOG(INFO) << "VSS Write: " << VSSPaths::WINDSHIELD_DEFROST << " = true";
            LOG(INFO) << "VSS Write: " << VSSPaths::REAR_WINDOW_DEFROST << " = true";
            LOG(INFO) << "VSS Write: " << VSSPaths::MIRROR_DEFROST_LEFT << " = true";
            LOG(INFO) << "VSS Write: " << VSSPaths::MIRROR_DEFROST_RIGHT << " = true";
            LOG(INFO) << "VSS Write: " << VSSPaths::WINDSHIELD_HEATER_LEVEL << " = 60";
            LOG(INFO) << "VSS Write: " << VSSPaths::REAR_HEATER_LEVEL << " = 60";
            signal_count = 6;
            break;
            
        case defrost_mode_t::QUICK_CLEAR:
            LOG(INFO) << "VSS Write: " << VSSPaths::WINDSHIELD_DEFROST << " = true";
            LOG(INFO) << "VSS Write: " << VSSPaths::REAR_WINDOW_DEFROST << " = true";
            LOG(INFO) << "VSS Write: " << VSSPaths::MIRROR_DEFROST_LEFT << " = true";
            LOG(INFO) << "VSS Write: " << VSSPaths::MIRROR_DEFROST_RIGHT << " = true";
            LOG(INFO) << "VSS Write: " << VSSPaths::WINDSHIELD_HEATER_LEVEL << " = 100";
            LOG(INFO) << "VSS Write: " << VSSPaths::REAR_HEATER_LEVEL << " = 80";
            signal_count = 6;
            break;
    }
    
    return signal_count;
}

bool DefrostServiceImpl::RegisterWithDiscovery(
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
        service_info->set_name("defrost_service");
        service_info->set_version("1.0.0");
        service_info->set_description("Windshield and mirror defrost control service");
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
                &DefrostServiceImpl::HeartbeatLoop, this, discovery_endpoint);
            
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

void DefrostServiceImpl::HeartbeatLoop(const std::string& discovery_endpoint) {
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

} // namespace defrost_service
} // namespace swdv