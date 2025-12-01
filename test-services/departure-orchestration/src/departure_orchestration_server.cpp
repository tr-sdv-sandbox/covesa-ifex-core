#include "departure_orchestration_server.hpp"
#include "ifex/network.hpp"
#include <iomanip>
#include <sstream>
#include <fstream>
#include <filesystem>

namespace swdv {
namespace departure_orchestration_service {

using namespace std::chrono_literals;

DepartureOrchestrationServiceImpl::DepartureOrchestrationServiceImpl() {
    LOG(INFO) << "Initializing Departure Orchestration Service";
}

DepartureOrchestrationServiceImpl::~DepartureOrchestrationServiceImpl() {
    Stop();
}

void DepartureOrchestrationServiceImpl::Start() {
    LOG(INFO) << "Starting Departure Orchestration Service";
    running_ = true;
}

void DepartureOrchestrationServiceImpl::Stop() {
    LOG(INFO) << "Stopping Departure Orchestration Service";
    running_ = false;
    
    // Cancel all active schedules
    std::lock_guard<std::mutex> lock(schedules_mutex_);
    for (auto& [id, entry] : schedules_) {
        entry->status = schedule_status_t::CANCELLED;
        if (entry->preparation_thread.joinable()) {
            entry->preparation_thread.join();
        }
    }
    schedules_.clear();
    
    // Stop heartbeat thread
    if (heartbeat_thread_.joinable()) {
        heartbeat_thread_.join();
    }
}

bool DepartureOrchestrationServiceImpl::RegisterWithDiscovery(
    const std::string& discovery_endpoint, 
    int port, 
    const std::string& service_name,
    const std::string& ifex_schema) {
    
    try {
        std::string primary_ip = ifex::network::get_primary_ip_address();
        
        // Create discovery channel
        auto channel = grpc::CreateChannel(discovery_endpoint, grpc::InsecureChannelCredentials());
        auto register_stub = swdv::service_discovery::register_service_service::NewStub(channel);
        heartbeat_stub_ = swdv::service_discovery::heartbeat_service::NewStub(channel);
        
        // Prepare service info
        service_info_ = std::make_shared<swdv::service_discovery::service_info_t>();
        service_info_->set_name(service_name);
        service_info_->set_version("1.0.0");
        service_info_->set_description("Orchestrates vehicle departure preparation");
        service_info_->set_status(swdv::service_discovery::AVAILABLE);
        
        // Load and set IFEX schema content - required for registration
        std::string schema_content;
        
        // Check if we have a composite schema built from discovery
        {
            std::lock_guard<std::mutex> lock(schema_mutex_);
            if (!composite_schema_.empty()) {
                schema_content = composite_schema_;
                LOG(INFO) << "Using composite schema with discovered types (" << schema_content.size() << " bytes)";
            }
        }
        
        // Fall back to static schema file if no composite schema
        if (schema_content.empty() && !ifex_schema.empty()) {
            std::ifstream schema_file(ifex_schema);
            if (schema_file.is_open()) {
                schema_content = std::string((std::istreambuf_iterator<char>(schema_file)),
                                           std::istreambuf_iterator<char>());
                LOG(INFO) << "Loaded IFEX schema from: " << ifex_schema << " (" << schema_content.size() << " bytes)";
            } else {
                LOG(ERROR) << "Failed to open IFEX schema file: " << ifex_schema;
                return false;
            }
        }
        
        if (schema_content.empty()) {
            LOG(ERROR) << "No schema available for service registration";
            return false;
        }
        
        service_info_->set_ifex_schema(schema_content);
        
        // Set endpoint
        auto* endpoint = service_info_->mutable_endpoint();
        endpoint->set_address(primary_ip + ":" + std::to_string(port));
        endpoint->set_transport(swdv::service_discovery::GRPC);
        
        // Register service
        swdv::service_discovery::register_service_request req;
        *req.mutable_service_info() = *service_info_;
        
        swdv::service_discovery::register_service_response resp;
        grpc::ClientContext context;
        
        auto status = register_stub->register_service(&context, req, &resp);
        if (!status.ok()) {
            LOG(ERROR) << "Failed to register service: " << status.error_message();
            return false;
        }
        
        service_id_ = resp.registration_id();
        LOG(INFO) << "Registered with discovery service, ID: " << service_id_;
        
        // Start heartbeat thread
        heartbeat_thread_ = std::thread([this]() {
            while (running_) {
                swdv::service_discovery::heartbeat_request hb_req;
                hb_req.set_registration_id(service_id_);
                hb_req.set_status(swdv::service_discovery::AVAILABLE);
                
                swdv::service_discovery::heartbeat_response hb_resp;
                grpc::ClientContext hb_context;
                
                auto status = heartbeat_stub_->heartbeat(&hb_context, hb_req, &hb_resp);
                if (!status.ok()) {
                    LOG(ERROR) << "Heartbeat failed: " << status.error_message();
                }
                
                std::this_thread::sleep_for(10s);
            }
        });
        
        return true;
    } catch (const std::exception& e) {
        LOG(ERROR) << "Failed to register with discovery: " << e.what();
        return false;
    }
}

void DepartureOrchestrationServiceImpl::SetDispatcherEndpoint(const std::string& dispatcher_endpoint) {
    dispatcher_endpoint_ = dispatcher_endpoint;
    LOG(INFO) << "Dispatcher endpoint set to: " << dispatcher_endpoint;
    
    // Discover schemas after dispatcher is set
    DiscoverDependentSchemas();
}

void DepartureOrchestrationServiceImpl::DiscoverDependentSchemas() {
    LOG(INFO) << "Discovering schemas from dependent services...";
    
    std::lock_guard<std::mutex> lock(schema_mutex_);
    
    // List of services we depend on
    std::vector<std::string> service_names = {
        "beverage_service",
        "climate_comfort_service",
        "defrost_service"
    };
    
    // Create discovery client to get service info
    // Extract discovery endpoint from dispatcher endpoint (assumes same host)
    std::string discovery_endpoint = dispatcher_endpoint_;
    size_t pos = discovery_endpoint.find_last_of(':');
    if (pos != std::string::npos) {
        discovery_endpoint = discovery_endpoint.substr(0, pos) + ":50051"; // Default discovery port
    }
    
    auto channel = grpc::CreateChannel(discovery_endpoint, grpc::InsecureChannelCredentials());
    auto stub = swdv::service_discovery::get_service_service::NewStub(channel);
    
    for (const auto& service_name : service_names) {
        try {
            swdv::service_discovery::get_service_request req;
            req.set_service_name(service_name);
            
            swdv::service_discovery::get_service_response resp;
            grpc::ClientContext context;
            
            auto status = stub->get_service(&context, req, &resp);
            if (status.ok() && resp.has_service_info()) {
                const auto& info = resp.service_info();
                if (!info.ifex_schema().empty()) {
                    LOG(INFO) << "Got schema for " << service_name << " (" 
                              << info.ifex_schema().size() << " bytes)";
                    
                    // Store the service info
                    DependentService dep_service;
                    dep_service.name = service_name;
                    dep_service.ifex_schema = info.ifex_schema();
                    
                    // Parse the schema
                    dep_service.parser = ifex::Parser::create(info.ifex_schema());
                    
                    dependent_services_[service_name] = std::move(dep_service);
                } else {
                    LOG(WARNING) << "Service " << service_name << " has no IFEX schema";
                }
            } else {
                LOG(WARNING) << "Failed to get service info for " << service_name 
                             << ": " << status.error_message();
            }
        } catch (const std::exception& e) {
            LOG(ERROR) << "Error discovering schema for " << service_name << ": " << e.what();
        }
    }
    
    // Build composite schema after discovering all services
    composite_schema_ = BuildCompositeSchema();
    
    LOG(INFO) << "Schema discovery complete. Found " << dependent_services_.size() 
              << " dependent services";
    
    // Re-register with discovery if we have a new composite schema
    if (!composite_schema_.empty() && service_info_) {
        LOG(INFO) << "Re-registering service with composite schema";
        service_info_->set_ifex_schema(composite_schema_);
        
        // Update registration with new schema
        // Note: In a production system, you'd want to properly update the registration
        // For now, the next heartbeat will include the updated service info
    }
}

std::string DepartureOrchestrationServiceImpl::BuildCompositeSchema() {
    LOG(INFO) << "Building composite schema from discovered services...";
    
    try {
        // Load our base schema - try multiple paths based on common patterns
        std::vector<std::string> schema_paths = {
            "./ifex/departure-orchestration-service.ifex.yml",  // CMake build dir pattern
            "./departure-orchestration-service.ifex.yml",       // Local dir
            "../departure-orchestration-service.ifex.yml",      // Parent dir
        };
        
        // Also check environment variable
        const char* schema_dir = std::getenv("IFEX_SCHEMA_DIR");
        if (schema_dir) {
            schema_paths.insert(schema_paths.begin(), 
                std::string(schema_dir) + "/departure-orchestration-service.ifex.yml");
        }
        
        std::ifstream schema_file;
        for (const auto& path : schema_paths) {
            schema_file.open(path);
            if (schema_file.is_open()) {
                LOG(INFO) << "Found base schema at: " << path;
                break;
            }
        }
        
        if (!schema_file.is_open()) {
            LOG(ERROR) << "Failed to open base schema file from any of the tried paths";
            LOG(ERROR) << "Current working directory: " << std::filesystem::current_path();
            return "";
        }
        
        YAML::Node base_schema = YAML::Load(schema_file);
        
        // Extract relevant types from dependent services
        YAML::Node imported_enums;
        YAML::Node imported_structs;
        
        for (const auto& [service_name, dep_service] : dependent_services_) {
            if (!dep_service.parser) continue;
            
            // Get all enumerations from this service
            auto enums = dep_service.parser->get_all_enum_definitions();
            for (const auto& enum_def : enums) {
                // Only import specific enums we use
                if ((service_name == "beverage_service" && 
                     (enum_def.name == "beverage_type_t" || enum_def.name == "strength_level_t")) ||
                    (service_name == "climate_comfort_service" && 
                     enum_def.name == "comfort_mode_t") ||
                    (service_name == "defrost_service" && 
                     enum_def.name == "defrost_mode_t")) {
                    
                    LOG(INFO) << "Importing enum " << enum_def.name << " from " << service_name;
                    
                    YAML::Node enum_node;
                    enum_node["name"] = enum_def.name;
                    enum_node["datatype"] = "uint8";
                    enum_node["description"] = enum_def.description + " (from " + service_name + ")";
                    
                    YAML::Node options;
                    for (const auto& opt : enum_def.options) {
                        YAML::Node option;
                        option["name"] = opt.name;
                        option["value"] = opt.value;
                        option["description"] = opt.description;
                        options.push_back(option);
                    }
                    enum_node["options"] = options;
                    
                    imported_enums.push_back(enum_node);
                }
            }
        }
        
        // Add imported enums to our namespace
        if (imported_enums.size() > 0) {
            YAML::Node namespaces = base_schema["namespaces"];
            if (namespaces && namespaces[0]) {
                YAML::Node existing_enums = namespaces[0]["enumerations"];
                if (!existing_enums) {
                    namespaces[0]["enumerations"] = YAML::Node(YAML::NodeType::Sequence);
                    existing_enums = namespaces[0]["enumerations"];
                }
                
                // Prepend imported enums before our own
                for (const auto& enum_node : imported_enums) {
                    existing_enums.push_back(enum_node);
                }
            }
        }
        
        // Update struct definitions to use proper enum types
        YAML::Node namespaces2 = base_schema["namespaces"];
        if (namespaces2 && namespaces2[0] && namespaces2[0]["structs"]) {
            YAML::Node structs = namespaces2[0]["structs"];
            for (size_t i = 0; i < structs.size(); ++i) {
                if (structs[i]["name"].as<std::string>() == "beverage_request_t") {
                    // Update beverage_type to use the enum
                    YAML::Node members = structs[i]["members"];
                    for (size_t j = 0; j < members.size(); ++j) {
                        if (members[j]["name"].as<std::string>() == "beverage_type") {
                            members[j]["datatype"] = "beverage_type_t";
                            members[j]["description"] = "Type of beverage to prepare";
                            members[j].remove("constraints"); // Remove string constraints
                        } else if (members[j]["name"].as<std::string>() == "strength") {
                            members[j]["datatype"] = "strength_level_t";
                            members[j]["description"] = "Beverage strength";
                            members[j].remove("constraints"); // Remove numeric constraints
                        }
                    }
                }
            }
        }
        
        // Write composite schema to string
        std::stringstream ss;
        ss << base_schema;
        
        LOG(INFO) << "Composite schema built successfully";
        return ss.str();
        
    } catch (const std::exception& e) {
        LOG(ERROR) << "Failed to build composite schema: " << e.what();
        return "";
    }
}

bool DepartureOrchestrationServiceImpl::CallServiceMethod(
    const std::string& service_name,
    const std::string& namespace_name,
    const std::string& method_name,
    const nlohmann::json& params) {
    
    if (dispatcher_endpoint_.empty()) {
        LOG(ERROR) << "Dispatcher endpoint not set";
        return false;
    }
    
    try {
        // Create dispatcher channel
        auto channel = grpc::CreateChannel(dispatcher_endpoint_, grpc::InsecureChannelCredentials());
        auto stub = swdv::ifex_dispatcher::call_method_service::NewStub(channel);
        
        // Prepare request
        swdv::ifex_dispatcher::call_method_request request;
        auto* call = request.mutable_call();
        call->set_service_name(service_name);
        // Namespace is part of method name in IFEX
        call->set_method_name(method_name);
        call->set_parameters(params.dump());
        call->set_timeout_ms(30000); // 30 second timeout
        
        // Make the call
        swdv::ifex_dispatcher::call_method_response response;
        grpc::ClientContext context;
        
        LOG(INFO) << "Calling " << service_name << "." << namespace_name << "." << method_name;
        LOG(INFO) << "Parameters: " << params.dump();
        
        auto status = stub->call_method(&context, request, &response);
        if (!status.ok()) {
            LOG(ERROR) << "RPC failed: " << status.error_message();
            return false;
        }
        
        const auto& result = response.result();
        if (result.status() != swdv::ifex_dispatcher::call_status_t::SUCCESS) {
            LOG(ERROR) << "Service call failed: " << result.error_message();
            return false;
        }
        
        LOG(INFO) << "Service call succeeded: " << result.response();
        return true;
        
    } catch (const std::exception& e) {
        LOG(ERROR) << "Exception calling service: " << e.what();
        return false;
    }
}

grpc::Status DepartureOrchestrationServiceImpl::schedule_departure(
    grpc::ServerContext* context,
    const schedule_departure_request* request,
    schedule_departure_response* response) {
    
    try {
        // Extract schedule from request
        const auto& schedule = request->schedule();
        
        // Validate departure time
        int64_t current_time = GetCurrentTimestamp();
        if (schedule.departure_time() <= current_time) {
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, 
                              "Departure time must be in the future");
        }
        
        // Generate schedule ID and create timeline
        std::string schedule_id = GenerateScheduleId();
        auto timeline = CreatePreparationTimeline(schedule);
        
        // Create schedule entry
        auto entry = std::make_unique<ScheduleEntry>();
        entry->schedule_id = schedule_id;
        entry->schedule = schedule;
        entry->status = schedule_status_t::SCHEDULED;
        entry->preparation_start_time = timeline.empty() ? 0 : timeline[0].scheduled_start_time();
        entry->completion_percent = 0;
        entry->timeline = timeline;
        
        // Start preparation thread
        entry->preparation_thread = std::thread(
            &DepartureOrchestrationServiceImpl::ExecuteSchedule, 
            this, 
            schedule_id
        );
        
        // Store schedule
        {
            std::lock_guard<std::mutex> lock(schedules_mutex_);
            schedules_[schedule_id] = std::move(entry);
        }
        
        // Build response
        response->set_schedule_id(schedule_id);
        response->set_accepted(true);
        
        // Add timeline to response
        for (const auto& step : timeline) {
            *response->add_preparation_timeline() = step;
        }
        
        return grpc::Status::OK;
        
    } catch (const std::exception& e) {
        LOG(ERROR) << "Error in schedule_departure: " << e.what();
        return grpc::Status(grpc::StatusCode::INTERNAL, e.what());
    }
}

grpc::Status DepartureOrchestrationServiceImpl::get_schedule_status(
    grpc::ServerContext* context,
    const get_schedule_status_request* request,
    get_schedule_status_response* response) {
    
    try {
        const std::string& schedule_id = request->schedule_id();
        
        std::lock_guard<std::mutex> lock(schedules_mutex_);
        auto it = schedules_.find(schedule_id);
        if (it == schedules_.end()) {
            return grpc::Status(grpc::StatusCode::NOT_FOUND, "Schedule not found");
        }
        
        auto& entry = it->second;
        
        // Set status info
        auto* status_info = response->mutable_status_info();
        status_info->set_schedule_id(entry->schedule_id);
        status_info->set_status(entry->status);
        status_info->set_departure_time(entry->schedule.departure_time());
        status_info->set_preparation_start_time(entry->preparation_start_time);
        status_info->set_current_step(entry->current_step);
        status_info->set_completion_percent(entry->completion_percent);
        
        // Add active steps
        int64_t current_time = GetCurrentTimestamp();
        for (const auto& step : entry->timeline) {
            if (step.scheduled_start_time() <= current_time && 
                current_time < step.scheduled_start_time() + step.duration_seconds()) {
                *response->add_active_steps() = step;
            }
        }
        
        return grpc::Status::OK;
        
    } catch (const std::exception& e) {
        LOG(ERROR) << "Error in get_schedule_status: " << e.what();
        return grpc::Status(grpc::StatusCode::INTERNAL, e.what());
    }
}

grpc::Status DepartureOrchestrationServiceImpl::cancel_schedule(
    grpc::ServerContext* context,
    const cancel_schedule_request* request,
    cancel_schedule_response* response) {
    
    try {
        const std::string& schedule_id = request->schedule_id();
        
        std::lock_guard<std::mutex> lock(schedules_mutex_);
        auto it = schedules_.find(schedule_id);
        if (it == schedules_.end()) {
            return grpc::Status(grpc::StatusCode::NOT_FOUND, "Schedule not found");
        }
        
        auto& entry = it->second;
        entry->status = schedule_status_t::CANCELLED;
        
        response->set_success(true);
        
        // List systems that may need shutdown
        if (entry->status == schedule_status_t::PREPARING) {
            response->add_systems_to_shutdown("climate_comfort");
            response->add_systems_to_shutdown("defrost");
            response->add_systems_to_shutdown("beverage");
        }
        
        return grpc::Status::OK;
        
    } catch (const std::exception& e) {
        LOG(ERROR) << "Error in cancel_schedule: " << e.what();
        return grpc::Status(grpc::StatusCode::INTERNAL, e.what());
    }
}

grpc::Status DepartureOrchestrationServiceImpl::modify_schedule(
    grpc::ServerContext* context,
    const modify_schedule_request* request,
    modify_schedule_response* response) {
    
    try {
        const std::string& schedule_id = request->schedule_id();
        
        std::lock_guard<std::mutex> lock(schedules_mutex_);
        auto it = schedules_.find(schedule_id);
        if (it == schedules_.end()) {
            return grpc::Status(grpc::StatusCode::NOT_FOUND, "Schedule not found");
        }
        
        auto& entry = it->second;
        
        // Can only modify scheduled (not yet started) schedules
        if (entry->status != schedule_status_t::SCHEDULED) {
            return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, 
                              "Can only modify schedules that haven't started");
        }
        
        // Apply modifications
        bool modified = false;
        if (request->new_departure_time() > 0) {
            entry->schedule.set_departure_time(request->new_departure_time());
            modified = true;
        }
        
        if (request->has_new_comfort_prefs()) {
            *entry->schedule.mutable_comfort_prefs() = request->new_comfort_prefs();
            modified = true;
        }
        
        // Recreate timeline if modified
        if (modified) {
            entry->timeline = CreatePreparationTimeline(entry->schedule);
            entry->preparation_start_time = entry->timeline.empty() ? 0 : entry->timeline[0].scheduled_start_time();
        }
        
        response->set_success(modified);
        
        // Add updated timeline to response
        for (const auto& step : entry->timeline) {
            *response->add_updated_timeline() = step;
        }
        
        return grpc::Status::OK;
        
    } catch (const std::exception& e) {
        LOG(ERROR) << "Error in modify_schedule: " << e.what();
        return grpc::Status(grpc::StatusCode::INTERNAL, e.what());
    }
}

grpc::Status DepartureOrchestrationServiceImpl::get_active_schedules(
    grpc::ServerContext* context,
    const get_active_schedules_request* request,
    get_active_schedules_response* response) {
    
    try {
        std::lock_guard<std::mutex> lock(schedules_mutex_);
        for (const auto& [id, entry] : schedules_) {
            if (entry->status != schedule_status_t::COMPLETED &&
                entry->status != schedule_status_t::CANCELLED) {
                
                auto* info = response->add_schedules();
                info->set_schedule_id(entry->schedule_id);
                info->set_status(entry->status);
                info->set_departure_time(entry->schedule.departure_time());
                info->set_preparation_start_time(entry->preparation_start_time);
                info->set_current_step(entry->current_step);
                info->set_completion_percent(entry->completion_percent);
            }
        }
        
        return grpc::Status::OK;
        
    } catch (const std::exception& e) {
        LOG(ERROR) << "Error in get_active_schedules: " << e.what();
        return grpc::Status(grpc::StatusCode::INTERNAL, e.what());
    }
}

std::vector<preparation_step_t> DepartureOrchestrationServiceImpl::CreatePreparationTimeline(
    const departure_schedule_t& schedule) {
    
    std::vector<preparation_step_t> timeline;
    
    // Work backwards from departure time
    int64_t current_time = schedule.departure_time();
    
    // Route calculation (5 minutes before departure)
    if (!schedule.calculate_route_to().empty()) {
        current_time -= 300; // 5 minutes
        preparation_step_t step;
        step.set_step_type(preparation_step_type_t::ROUTE_CALCULATION);
        step.set_scheduled_start_time(current_time);
        step.set_duration_seconds(60); // 1 minute
        step.set_description("Calculate route to " + schedule.calculate_route_to());
        timeline.push_back(step);
    }
    
    // Beverage preparation (10 minutes before departure)
    if (schedule.beverage_request().prepare_beverage()) {
        current_time -= 600; // 10 minutes
        preparation_step_t step;
        step.set_step_type(preparation_step_type_t::BEVERAGE_START);
        step.set_scheduled_start_time(current_time);
        step.set_duration_seconds(300); // 5 minutes
        step.set_description("Prepare " + schedule.beverage_request().beverage_type());
        timeline.push_back(step);
    }
    
    // Battery preconditioning (15 minutes before departure)
    if (schedule.precondition_battery()) {
        current_time -= 900; // 15 minutes
        preparation_step_t step;
        step.set_step_type(preparation_step_type_t::BATTERY_PRECONDITIONING);
        step.set_scheduled_start_time(current_time);
        step.set_duration_seconds(600); // 10 minutes
        step.set_description("Precondition battery for optimal range");
        timeline.push_back(step);
    }
    
    // Defrost (20 minutes before departure if auto_defrost)
    if (schedule.auto_defrost()) {
        current_time -= 1200; // 20 minutes
        preparation_step_t step;
        step.set_step_type(preparation_step_type_t::DEFROST_START);
        step.set_scheduled_start_time(current_time);
        step.set_duration_seconds(600); // 10 minutes
        step.set_description("Defrost windows and mirrors");
        timeline.push_back(step);
    }
    
    // Seat heating (25 minutes before departure)
    if (schedule.comfort_prefs().seat_temperature_level() != 0) {
        current_time -= 1500; // 25 minutes
        preparation_step_t step;
        step.set_step_type(preparation_step_type_t::SEAT_HEATING_START);
        step.set_scheduled_start_time(current_time);
        step.set_duration_seconds(300); // 5 minutes
        step.set_description("Heat/cool seats to level " + 
                           std::to_string(schedule.comfort_prefs().seat_temperature_level()));
        timeline.push_back(step);
    }
    
    // Climate start (30 minutes before departure)
    current_time -= 1800; // 30 minutes
    preparation_step_t climate_step;
    climate_step.set_step_type(preparation_step_type_t::CLIMATE_START);
    climate_step.set_scheduled_start_time(current_time);
    climate_step.set_duration_seconds(1200); // 20 minutes
    climate_step.set_description("Set cabin temperature to " + 
                              std::to_string(schedule.comfort_prefs().cabin_temperature_celsius()) + "°C");
    timeline.push_back(climate_step);
    
    // Sort timeline by start time
    std::sort(timeline.begin(), timeline.end(), 
              [](const auto& a, const auto& b) {
                  return a.scheduled_start_time() < b.scheduled_start_time();
              });
    
    return timeline;
}

void DepartureOrchestrationServiceImpl::ExecuteSchedule(const std::string& schedule_id) {
    LOG(INFO) << "Starting execution of schedule: " << schedule_id;
    
    while (running_) {
        std::unique_lock<std::mutex> lock(schedules_mutex_);
        auto it = schedules_.find(schedule_id);
        if (it == schedules_.end() || it->second->status == schedule_status_t::CANCELLED) {
            LOG(INFO) << "Schedule " << schedule_id << " cancelled or not found";
            return;
        }
        
        auto& entry = it->second;
        int64_t current_time = GetCurrentTimestamp();
        
        // Check if we should start preparation
        if (entry->status == schedule_status_t::SCHEDULED &&
            current_time >= entry->preparation_start_time) {
            entry->status = schedule_status_t::PREPARING;
            LOG(INFO) << "Starting preparation for schedule: " << schedule_id;
        }
        
        // Execute steps that are due
        if (entry->status == schedule_status_t::PREPARING) {
            bool all_complete = true;
            int completed_steps = 0;
            
            for (const auto& step : entry->timeline) {
                if (current_time >= step.scheduled_start_time()) {
                    // Check if step is in progress or complete
                    if (current_time < step.scheduled_start_time() + step.duration_seconds()) {
                        if (entry->current_step != step.description()) {
                            entry->current_step = step.description();
                            lock.unlock();
                            ExecutePreparationStep(schedule_id, step);
                            lock.lock();
                        }
                        all_complete = false;
                    } else {
                        completed_steps++;
                    }
                } else {
                    all_complete = false;
                }
            }
            
            // Update completion percentage
            if (!entry->timeline.empty()) {
                entry->completion_percent = (completed_steps * 100) / entry->timeline.size();
            }
            
            // Check if all steps are complete
            if (all_complete && current_time >= entry->schedule.departure_time()) {
                entry->status = schedule_status_t::READY;
                LOG(INFO) << "Vehicle ready for departure: " << schedule_id;
                
                // Mark as completed after a short delay
                std::this_thread::sleep_for(5min);
                entry->status = schedule_status_t::COMPLETED;
                return;
            }
        }
        
        lock.unlock();
        std::this_thread::sleep_for(10s);
    }
}

void DepartureOrchestrationServiceImpl::ExecutePreparationStep(
    const std::string& schedule_id, 
    const preparation_step_t& step) {
    
    LOG(INFO) << "Executing step: " << step.description();
    
    try {
        bool success = false;
        
        switch (step.step_type()) {
            case preparation_step_type_t::CLIMATE_START: {
                std::lock_guard<std::mutex> lock(schedules_mutex_);
                auto& entry = schedules_[schedule_id];
                success = StartClimateComfort(
                    entry->schedule.comfort_prefs().cabin_temperature_celsius(),
                    entry->schedule.comfort_prefs().seat_temperature_level() != 0,
                    entry->schedule.comfort_prefs().steering_wheel_heating()
                );
                break;
            }
            
            case preparation_step_type_t::DEFROST_START:
                success = StartDefrost();
                break;
                
            case preparation_step_type_t::SEAT_HEATING_START:
                // Already handled by climate comfort
                success = true;
                break;
                
            case preparation_step_type_t::BEVERAGE_START: {
                std::lock_guard<std::mutex> lock(schedules_mutex_);
                auto& entry = schedules_[schedule_id];
                success = PrepareBeverage(entry->schedule.beverage_request());
                break;
            }
            
            case preparation_step_type_t::BATTERY_PRECONDITIONING:
                success = PreconditionBattery();
                break;
                
            case preparation_step_type_t::ROUTE_CALCULATION: {
                std::lock_guard<std::mutex> lock(schedules_mutex_);
                auto& entry = schedules_[schedule_id];
                success = CalculateRoute(entry->schedule.calculate_route_to());
                break;
            }
        }
        
        if (!success) {
            LOG(ERROR) << "Failed to execute step: " << step.description();
        }
        
    } catch (const std::exception& e) {
        LOG(ERROR) << "Exception executing step: " << e.what();
    }
}

bool DepartureOrchestrationServiceImpl::StartClimateComfort(
    float temperature, bool seat_heating, bool steering_wheel_heating) {
    
    // Map temperature to comfort level
    int comfort_level = 1; // NORMAL
    if (temperature < 20.0) {
        comfort_level = 0; // COZY
    } else if (temperature > 24.0) {
        comfort_level = 2; // FRESH
    }
    
    nlohmann::json params;
    params["departure_time_minutes"] = 30; // Prepare for 30 minutes
    params["target_comfort"]["temperature_preference"] = comfort_level;
    params["target_comfort"]["seat_heating"] = seat_heating;
    params["target_comfort"]["steering_wheel_heating"] = steering_wheel_heating;
    
    return CallServiceMethod("climate_comfort_service", "climate", "prepare_cabin_comfort", params);
}

bool DepartureOrchestrationServiceImpl::StartDefrost() {
    nlohmann::json params;
    params["targets"] = nlohmann::json::array({"WINDSHIELD", "REAR_WINDOW", "SIDE_MIRRORS"});
    params["intensity"] = 1; // RAPID
    params["auto_stop"] = true;
    
    return CallServiceMethod("defrost_service", "defrost", "start_defrost_operation", params);
}

bool DepartureOrchestrationServiceImpl::PrepareBeverage(const beverage_request_t& request) {
    if (!request.prepare_beverage()) {
        return true;
    }
    
    nlohmann::json params;
    
    // With our new schema discovery, beverage_type should now be an enum value
    // The protobuf generated code will handle the enum correctly
    params["beverage_type"] = request.beverage_type();
    params["strength"] = request.strength();
    
    // Create full config object as expected by beverage service
    nlohmann::json config;
    config["type"] = request.beverage_type();
    config["strength"] = request.strength();
    config["temperature_celsius"] = 85;  // Default temperature
    config["size_ml"] = 250;  // Default size
    
    params["config"] = config;
    
    return CallServiceMethod("beverage_service", "beverage", "prepare_beverage", params);
}

bool DepartureOrchestrationServiceImpl::PreconditionBattery() {
    // This would call a battery management service
    // For now, simulate success
    LOG(INFO) << "Simulating battery preconditioning";
    return true;
}

bool DepartureOrchestrationServiceImpl::CalculateRoute(const std::string& destination) {
    // This would call a navigation service
    // For now, simulate success
    LOG(INFO) << "Simulating route calculation to: " << destination;
    return true;
}

std::string DepartureOrchestrationServiceImpl::GenerateScheduleId() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    
    std::stringstream ss;
    ss << "departure_" << std::put_time(std::localtime(&time_t), "%Y%m%d_%H%M%S_")
       << std::setfill('0') << std::setw(4) << schedule_counter_++;
    
    return ss.str();
}

int64_t DepartureOrchestrationServiceImpl::GetCurrentTimestamp() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

} // namespace departure_orchestration_service
} // namespace swdv