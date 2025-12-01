#include "discovery_server.hpp"
#include <ifex/parser.hpp>
#include <glog/logging.h>
#include <grpcpp/server_builder.h>
#include <yaml-cpp/yaml.h>
#include <sstream>
#include <unordered_map>

namespace ifex::reference {

DiscoveryServer::DiscoveryServer() {
    LOG(INFO) << "Initializing Discovery Server";
}

DiscoveryServer::~DiscoveryServer() {
    if (server_) {
        Shutdown();
    }
}

void DiscoveryServer::Start(const std::string& listen_address) {
    grpc::ServerBuilder builder;
    builder.AddListeningPort(listen_address, grpc::InsecureServerCredentials());
    
    // Register all service implementations
    builder.RegisterService(static_cast<swdv::service_discovery::get_service_service::Service*>(this));
    builder.RegisterService(static_cast<swdv::service_discovery::register_service_service::Service*>(this));
    builder.RegisterService(static_cast<swdv::service_discovery::unregister_service_service::Service*>(this));
    builder.RegisterService(static_cast<swdv::service_discovery::query_services_service::Service*>(this));
    builder.RegisterService(static_cast<swdv::service_discovery::heartbeat_service::Service*>(this));
    
    server_ = builder.BuildAndStart();
    LOG(INFO) << "Discovery server listening on " << listen_address;
}

void DiscoveryServer::Shutdown() {
    if (server_) {
        LOG(INFO) << "Shutting down discovery server";
        server_->Shutdown();
    }
}

void DiscoveryServer::Wait() {
    if (server_) {
        server_->Wait();
    }
}

// Helper method to generate unique registration IDs
std::string DiscoveryServer::generate_registration_id() {
    return "reg_" + std::to_string(next_registration_id_.fetch_add(1));
}

// get_service implementation
grpc::Status DiscoveryServer::get_service(
    grpc::ServerContext* context,
    const swdv::service_discovery::get_service_request* request,
    swdv::service_discovery::get_service_response* response) {
    
    LOG(INFO) << "get_service called for: " << request->service_name();
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Look up service by name
    auto range = services_by_name_.equal_range(request->service_name());
    
    for (auto it = range.first; it != range.second; ++it) {
        const auto& reg_id = it->second;
        auto service_it = registered_services_.find(reg_id);
        
        if (service_it != registered_services_.end() && service_it->second.is_available()) {
            // Found an available service - populate service_info
            auto* service_info = response->mutable_service_info();
            service_info->set_name(service_it->second.name);
            service_info->set_version(service_it->second.version);
            service_info->set_description(service_it->second.description);
            service_info->set_status(service_it->second.status);
            service_info->set_ifex_schema(service_it->second.ifex_schema);
            
            // Set endpoint
            auto* endpoint = service_info->mutable_endpoint();
            endpoint->set_address(service_it->second.address);
            
            // Convert transport string to enum
            if (service_it->second.transport == "GRPC") {
                endpoint->set_transport(swdv::service_discovery::transport_type_t::GRPC);
            } else if (service_it->second.transport == "HTTP_REST") {
                endpoint->set_transport(swdv::service_discovery::transport_type_t::HTTP_REST);
            } else if (service_it->second.transport == "DBUS") {
                endpoint->set_transport(swdv::service_discovery::transport_type_t::DBUS);
            } else {
                endpoint->set_transport(swdv::service_discovery::transport_type_t::GRPC);
            }
            
            // Copy namespaces
            for (const auto& ns : service_it->second.namespaces) {
                *service_info->add_namespaces() = ns;
            }
            
            // Set last heartbeat timestamp
            auto time_since_epoch = service_it->second.last_heartbeat.time_since_epoch();
            auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(time_since_epoch).count();
            service_info->set_last_heartbeat(millis);
            
            LOG(INFO) << "Found service " << request->service_name() 
                      << " at " << service_it->second.address;
            return grpc::Status::OK;
        }
    }
    
    // Service not found - return NOT_FOUND status
    LOG(INFO) << "Service " << request->service_name() << " not found or not available";
    return grpc::Status(grpc::StatusCode::NOT_FOUND, 
                       "Service not found: " + request->service_name());
}

// register_service implementation
grpc::Status DiscoveryServer::register_service(
    grpc::ServerContext* context,
    const swdv::service_discovery::register_service_request* request,
    swdv::service_discovery::register_service_response* response) {
    
    LOG(INFO) << "register_service called";
    
    const auto& service_info = request->service_info();
    
    // Create registration entry
    ServiceRegistration registration;
    registration.registration_id = generate_registration_id();
    registration.name = service_info.name();
    registration.version = service_info.version();
    registration.description = service_info.description();
    registration.address = service_info.endpoint().address();
    
    // Convert transport enum to string for internal storage
    switch (service_info.endpoint().transport()) {
        case swdv::service_discovery::transport_type_t::GRPC:
            registration.transport = "GRPC";
            break;
        case swdv::service_discovery::transport_type_t::HTTP_REST:
            registration.transport = "HTTP_REST";
            break;
        case swdv::service_discovery::transport_type_t::DBUS:
            registration.transport = "DBUS";
            break;
        case swdv::service_discovery::transport_type_t::SOMEIP:
            registration.transport = "SOMEIP";
            break;
        case swdv::service_discovery::transport_type_t::MQTT:
            registration.transport = "MQTT";
            break;
        default:
            registration.transport = "GRPC";
    }
    
    registration.last_heartbeat = std::chrono::system_clock::now();
    registration.status = service_info.status();
    // Status is already set above
    registration.ifex_schema = service_info.ifex_schema();

    // Copy namespaces from request if provided
    for (const auto& ns : service_info.namespaces()) {
        registration.namespaces.push_back(ns);
    }

    // If no namespaces were provided, try to parse them from ifex_schema
    if (registration.namespaces.empty() && !registration.ifex_schema.empty()) {
        try {
            auto parser = ifex::Parser::create(registration.ifex_schema);
            if (parser) {
                auto methods = parser->get_all_methods();

                // Group methods by namespace
                std::unordered_map<std::string, std::vector<ifex::MethodSignature>> methods_by_ns;
                for (const auto& method : methods) {
                    std::string ns_name = method.namespace_name.empty() ? "default" : method.namespace_name;
                    methods_by_ns[ns_name].push_back(method);
                }

                // Create namespace protos
                for (const auto& [ns_name, ns_methods] : methods_by_ns) {
                    swdv::service_discovery::namespace_info_t proto_ns;
                    proto_ns.set_name(ns_name);
                    proto_ns.set_description(ns_name + " namespace");

                    for (const auto& method : ns_methods) {
                        auto* proto_method = proto_ns.add_methods();
                        proto_method->set_name(method.method_name);
                        proto_method->set_description(method.description);
                    }

                    registration.namespaces.push_back(proto_ns);
                }

                LOG(INFO) << "Parsed " << methods.size() << " methods from IFEX schema for " << registration.name;
            }
        } catch (const std::exception& e) {
            LOG(WARNING) << "Could not parse IFEX schema: " << e.what();
        }
    }
    
    // Store registration
    std::lock_guard<std::mutex> lock(mutex_);
    registered_services_[registration.registration_id] = registration;
    services_by_name_.emplace(registration.name, registration.registration_id);
    
    // Set response
    response->set_registration_id(registration.registration_id);
    
    LOG(INFO) << "Service " << registration.name << " registered with ID: " 
              << registration.registration_id;
    
    return grpc::Status::OK;
}

// unregister_service implementation
grpc::Status DiscoveryServer::unregister_service(
    grpc::ServerContext* context,
    const swdv::service_discovery::unregister_service_request* request,
    swdv::service_discovery::unregister_service_response* response) {
    
    LOG(INFO) << "unregister_service called for ID: " << request->registration_id();
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = registered_services_.find(request->registration_id());
    if (it != registered_services_.end()) {
        // Remove from name index
        auto range = services_by_name_.equal_range(it->second.name);
        for (auto name_it = range.first; name_it != range.second; ) {
            if (name_it->second == request->registration_id()) {
                name_it = services_by_name_.erase(name_it);
            } else {
                ++name_it;
            }
        }
        
        // Remove from main registry
        registered_services_.erase(it);
        
        LOG(INFO) << "Service unregistered successfully";
        return grpc::Status::OK;
    } else {
        LOG(WARNING) << "Registration ID not found: " << request->registration_id();
        return grpc::Status(grpc::StatusCode::NOT_FOUND, 
                           "Registration ID not found");
    }
}

// query_services implementation
grpc::Status DiscoveryServer::query_services(
    grpc::ServerContext* context,
    const swdv::service_discovery::query_services_request* request,
    swdv::service_discovery::query_services_response* response) {
    
    LOG(INFO) << "query_services called";
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    for (const auto& [reg_id, service] : registered_services_) {
        if (matches_filter(service, request->filter())) {
            auto* service_info = response->add_services();
            
            service_info->set_name(service.name);
            service_info->set_version(service.version);
            service_info->set_description(service.description);
            service_info->set_ifex_schema(service.ifex_schema);
            
            // Set actual status (not just available/unavailable)
            service_info->set_status(service.status);
            
            // Set endpoint
            auto* endpoint = service_info->mutable_endpoint();
            endpoint->set_address(service.address);
            // Set transport enum based on string
            if (service.transport == "GRPC") {
                endpoint->set_transport(swdv::service_discovery::transport_type_t::GRPC);
            } else if (service.transport == "HTTP_REST") {
                endpoint->set_transport(swdv::service_discovery::transport_type_t::HTTP_REST);
            } else {
                endpoint->set_transport(swdv::service_discovery::transport_type_t::GRPC);
            }
            
            // Copy namespaces
            for (const auto& ns : service.namespaces) {
                *service_info->add_namespaces() = ns;
            }
            
            // Set last heartbeat
            auto time_since_epoch = service.last_heartbeat.time_since_epoch();
            auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(time_since_epoch).count();
            service_info->set_last_heartbeat(millis);
        }
    }
    
    LOG(INFO) << "Found " << response->services_size() << " matching services";
    
    return grpc::Status::OK;
}

// heartbeat implementation
grpc::Status DiscoveryServer::heartbeat(
    grpc::ServerContext* context,
    const swdv::service_discovery::heartbeat_request* request,
    swdv::service_discovery::heartbeat_response* response) {
    
    LOG(INFO) << "heartbeat called for ID: " << request->registration_id() 
              << " status: " << request->status();
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = registered_services_.find(request->registration_id());
    if (it != registered_services_.end()) {
        it->second.last_heartbeat = std::chrono::system_clock::now();
        
        // Update status
        it->second.status = request->status();
        
        // Status is already updated above, availability is derived from status
        
        LOG(INFO) << "Heartbeat updated for service " << it->second.name 
                  << " with status: " << request->status();
        return grpc::Status::OK;
    } else {
        LOG(WARNING) << "Registration ID not found: " << request->registration_id();
        return grpc::Status(grpc::StatusCode::NOT_FOUND, 
                           "Registration ID not found");
    }
}


// Helper to check if a service matches the filter
bool DiscoveryServer::matches_filter(
    const ServiceRegistration& service,
    const swdv::service_discovery::service_filter_t& filter) {
    
    // Check name pattern
    if (!filter.name_pattern().empty()) {
        if (service.name.find(filter.name_pattern()) == std::string::npos) {
            return false;
        }
    }
    
    // Check method existence
    if (!filter.has_method().empty()) {
        bool found = false;
        for (const auto& ns : service.namespaces) {
            for (const auto& method : ns.methods()) {
                if (method.name() == filter.has_method()) {
                    found = true;
                    break;
                }
            }
            if (found) break;
        }
        if (!found) return false;
    }
    
    // Check transport type
    if (filter.transport_type() != swdv::service_discovery::transport_type_t::GRPC) {
        // Convert our string transport to enum for comparison
        swdv::service_discovery::transport_type_t service_transport = swdv::service_discovery::transport_type_t::GRPC;
        if (service.transport == "HTTP_REST") {
            service_transport = swdv::service_discovery::transport_type_t::HTTP_REST;
        } else if (service.transport == "DBUS") {
            service_transport = swdv::service_discovery::transport_type_t::DBUS;
        }
        // Add other transports as needed
        
        if (service_transport != filter.transport_type()) {
            return false;
        }
    }
    
    // Check availability
    if (filter.available_only() && !service.is_available()) {
        return false;
    }
    
    // Check extension paths
    for (const auto& ext_path : filter.extension_paths()) {
        if (!check_extension_path(service.ifex_schema, ext_path)) {
            return false;
        }
    }
    
    return true;
}

// Helper to check extension paths in IFEX YAML
bool DiscoveryServer::check_extension_path(
    const std::string& ifex_yaml,
    const std::string& extension_path) {
    
    try {
        YAML::Node root = YAML::Load(ifex_yaml);
        
        // Parse extension path (e.g., "x-scheduling.enabled=true")
        size_t eq_pos = extension_path.find('=');
        if (eq_pos == std::string::npos) {
            // Just check existence
            std::istringstream path_stream(extension_path);
            std::string segment;
            YAML::Node current = root;
            
            while (std::getline(path_stream, segment, '.')) {
                if (!current[segment]) {
                    return false;
                }
                current = current[segment];
            }
            return true;
        } else {
            // Check value
            std::string path = extension_path.substr(0, eq_pos);
            std::string expected_value = extension_path.substr(eq_pos + 1);
            
            // Navigate to the path
            std::istringstream path_stream(path);
            std::string segment;
            YAML::Node current = root;
            
            while (std::getline(path_stream, segment, '.')) {
                // Check in namespaces/methods if not at root
                if (!current[segment] && current["namespaces"]) {
                    // Look in methods of all namespaces
                    bool found = false;
                    for (const auto& ns : current["namespaces"]) {
                        if (ns["methods"]) {
                            for (const auto& method : ns["methods"]) {
                                if (method[segment]) {
                                    current = method[segment];
                                    found = true;
                                    break;
                                }
                            }
                        }
                        if (found) break;
                    }
                    if (!found) return false;
                } else if (!current[segment]) {
                    return false;
                } else {
                    current = current[segment];
                }
            }
            
            // Check value
            std::string actual_value = current.as<std::string>();
            return actual_value == expected_value;
        }
    } catch (const std::exception& e) {
        LOG(WARNING) << "Failed to check extension path: " << e.what();
        return false;
    }
}

} // namespace ifex::reference