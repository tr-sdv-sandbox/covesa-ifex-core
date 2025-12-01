#include "discovery_client_impl.hpp"
#include "service-discovery-service.grpc.pb.h"
#include <glog/logging.h>
#include <grpcpp/create_channel.h>
#include <unordered_map>

namespace ifex {

// Internal struct to hold stubs
struct DiscoveryClientImpl::Stubs {
    std::unique_ptr<swdv::service_discovery::get_service_service::Stub> get_service_stub;
    std::unique_ptr<swdv::service_discovery::register_service_service::Stub> register_service_stub;
    std::unique_ptr<swdv::service_discovery::unregister_service_service::Stub> unregister_service_stub;
    std::unique_ptr<swdv::service_discovery::query_services_service::Stub> query_services_stub;
    std::unique_ptr<swdv::service_discovery::heartbeat_service::Stub> heartbeat_stub;
};

DiscoveryClientImpl::DiscoveryClientImpl(const std::string& discovery_endpoint)
    : discovery_endpoint_(discovery_endpoint), stubs_(std::make_unique<Stubs>()) {
    LOG(INFO) << "Creating discovery client for endpoint: " << discovery_endpoint;
    
    // Create channel to discovery service
    channel_ = grpc::CreateChannel(discovery_endpoint, grpc::InsecureChannelCredentials());
    
    // Create stubs for all discovery services
    stubs_->get_service_stub = swdv::service_discovery::get_service_service::NewStub(channel_);
    stubs_->register_service_stub = swdv::service_discovery::register_service_service::NewStub(channel_);
    stubs_->unregister_service_stub = swdv::service_discovery::unregister_service_service::NewStub(channel_);
    stubs_->query_services_stub = swdv::service_discovery::query_services_service::NewStub(channel_);
    stubs_->heartbeat_stub = swdv::service_discovery::heartbeat_service::NewStub(channel_);
}
    
std::string DiscoveryClientImpl::register_service(
    const ServiceEndpoint& endpoint,
    const std::string& ifex_schema) {
    
    try {
        // Parse IFEX schema to get service info
        auto parser = Parser::create(ifex_schema);
        std::string service_name = parser->get_service_name();
        std::string service_version = parser->get_service_version();
        std::string service_description = parser->get_service_description();
        
        // Create request
        swdv::service_discovery::register_service_request request;
        auto* service_info = request.mutable_service_info();
        
        service_info->set_name(service_name);
        service_info->set_version(service_version);
        service_info->set_description(service_description);
        service_info->set_status(swdv::service_discovery::service_status_t::AVAILABLE);
        service_info->set_ifex_schema(ifex_schema);
        
        // Set endpoint
        auto* proto_endpoint = service_info->mutable_endpoint();
        proto_endpoint->set_address(endpoint.address);
        
        // Convert transport type
        switch (endpoint.transport) {
            case ServiceEndpoint::Transport::GRPC:
                proto_endpoint->set_transport(swdv::service_discovery::transport_type_t::GRPC);
                break;
            case ServiceEndpoint::Transport::HTTP_REST:
                proto_endpoint->set_transport(swdv::service_discovery::transport_type_t::HTTP_REST);
                break;
            case ServiceEndpoint::Transport::DBUS:
                proto_endpoint->set_transport(swdv::service_discovery::transport_type_t::DBUS);
                break;
            default:
                proto_endpoint->set_transport(swdv::service_discovery::transport_type_t::GRPC);
        }
        
        // Group methods by namespace
        auto all_methods = parser->get_all_methods();
        std::unordered_map<std::string, std::vector<MethodSignature>> methods_by_namespace;
        
        for (const auto& method : all_methods) {
            std::string ns_name = method.namespace_name.empty() ? "default" : method.namespace_name;
            methods_by_namespace[ns_name].push_back(method);
        }
        
        // Add namespaces and their methods
        for (const auto& [ns_name, methods] : methods_by_namespace) {
            auto* proto_ns = service_info->add_namespaces();
            proto_ns->set_name(ns_name);
            proto_ns->set_description(ns_name + " namespace");
            
            // Add methods for this namespace
            for (const auto& method : methods) {
                auto* proto_method = proto_ns->add_methods();
                proto_method->set_name(method.method_name);
                proto_method->set_description(method.description);
                
                // Add input parameters
                for (const auto& param : method.input_parameters) {
                    auto* proto_param = proto_method->add_input_parameters();
                    proto_param->set_name(param.name);
                    proto_param->set_description(param.description);
                    proto_param->set_is_optional(param.is_optional);
                    
                    // Convert type to parameter_type_t
                    proto_param->set_type(convert_type_to_proto(param.type));
                }
                
                // Add output parameters
                for (const auto& param : method.output_parameters) {
                    auto* proto_param = proto_method->add_output_parameters();
                    proto_param->set_name(param.name);
                    proto_param->set_description(param.description);
                    proto_param->set_is_optional(param.is_optional);
                    proto_param->set_type(convert_type_to_proto(param.type));
                }
            }
        }
        
        // Make the call
        swdv::service_discovery::register_service_response response;
        grpc::ClientContext context;
        
        auto status = stubs_->register_service_stub->register_service(&context, request, &response);
        
        if (status.ok()) {
            LOG(INFO) << "Successfully registered service " << service_name 
                      << " with ID: " << response.registration_id();
            return response.registration_id();
        } else {
            LOG(ERROR) << "Failed to register service: " << status.error_message();
            return "";
        }
        
    } catch (const std::exception& e) {
        LOG(ERROR) << "Exception during service registration: " << e.what();
        return "";
    }
}
    
bool DiscoveryClientImpl::unregister_service(const std::string& registration_id) {
    try {
        swdv::service_discovery::unregister_service_request request;
        request.set_registration_id(registration_id);
        
        swdv::service_discovery::unregister_service_response response;
        grpc::ClientContext context;
        
        auto status = stubs_->unregister_service_stub->unregister_service(&context, request, &response);
        
        if (status.ok()) {
            LOG(INFO) << "Successfully unregistered service with ID: " << registration_id;
            return true;
        } else {
            LOG(ERROR) << "Failed to unregister service: " << status.error_message();
            return false;
        }
    } catch (const std::exception& e) {
        LOG(ERROR) << "Exception during service unregistration: " << e.what();
        return false;
    }
}
    
bool DiscoveryClientImpl::send_heartbeat(
    const std::string& registration_id,
    ServiceStatus status) {
    
    try {
        swdv::service_discovery::heartbeat_request request;
        request.set_registration_id(registration_id);
        
        // Convert ServiceStatus enum to protobuf enum
        switch (status) {
            case ServiceStatus::AVAILABLE:
                request.set_status(swdv::service_discovery::service_status_t::AVAILABLE);
                break;
            case ServiceStatus::STARTING:
                request.set_status(swdv::service_discovery::service_status_t::STARTING);
                break;
            case ServiceStatus::STOPPING:
                request.set_status(swdv::service_discovery::service_status_t::STOPPING);
                break;
            case ServiceStatus::UNAVAILABLE:
                request.set_status(swdv::service_discovery::service_status_t::UNAVAILABLE);
                break;
            case ServiceStatus::ERROR:
                request.set_status(swdv::service_discovery::service_status_t::ERROR);
                break;
            default:
                request.set_status(swdv::service_discovery::service_status_t::AVAILABLE);
        }
        
        swdv::service_discovery::heartbeat_response response;
        grpc::ClientContext context;
        
        auto grpc_status = stubs_->heartbeat_stub->heartbeat(&context, request, &response);
        
        return grpc_status.ok();
        
    } catch (const std::exception& e) {
        LOG(ERROR) << "Exception during heartbeat: " << e.what();
        return false;
    }
}
    
std::optional<ServiceInfo> DiscoveryClientImpl::get_service(const std::string& service_name) {
    try {
        swdv::service_discovery::get_service_request request;
        request.set_service_name(service_name);
        
        swdv::service_discovery::get_service_response response;
        grpc::ClientContext context;
        
        auto status = stubs_->get_service_stub->get_service(&context, request, &response);
        
        if (status.ok()) {
            return convert_proto_to_service_info(response.service_info());
        } else {
            LOG(INFO) << "Service not found: " << service_name 
                      << " - " << status.error_message();
            return std::nullopt;
        }
    } catch (const std::exception& e) {
        LOG(ERROR) << "Exception during get_service: " << e.what();
        return std::nullopt;
    }
}
    
std::vector<ServiceInfo> DiscoveryClientImpl::query_services(const ServiceFilter& filter) {
    try {
        swdv::service_discovery::query_services_request request;
        auto* proto_filter = request.mutable_filter();
        
        // Convert filter to proto
        if (filter.name_pattern.has_value()) {
            proto_filter->set_name_pattern(filter.name_pattern.value());
        }
        if (filter.has_method.has_value()) {
            proto_filter->set_has_method(filter.has_method.value());
        }
        proto_filter->set_available_only(!filter.show_all);  // Use show_all flag
        
        // Convert transport filter if specified
        if (filter.transport.has_value()) {
            switch (filter.transport.value()) {
                case ServiceEndpoint::Transport::GRPC:
                    proto_filter->set_transport_type(swdv::service_discovery::transport_type_t::GRPC);
                    break;
                case ServiceEndpoint::Transport::HTTP_REST:
                    proto_filter->set_transport_type(swdv::service_discovery::transport_type_t::HTTP_REST);
                    break;
                case ServiceEndpoint::Transport::DBUS:
                    proto_filter->set_transport_type(swdv::service_discovery::transport_type_t::DBUS);
                    break;
                default:
                    proto_filter->set_transport_type(swdv::service_discovery::transport_type_t::GRPC);
            }
        }
        
        // Add metadata filters as extension paths
        for (const auto& [key, value] : filter.metadata_filters) {
            proto_filter->add_extension_paths(key + "=" + value);
        }
        
        swdv::service_discovery::query_services_response response;
        grpc::ClientContext context;
        
        auto status = stubs_->query_services_stub->query_services(&context, request, &response);
        
        std::vector<ServiceInfo> services;
        if (status.ok()) {
            for (const auto& proto_service : response.services()) {
                services.push_back(convert_proto_to_service_info(proto_service));
            }
        } else {
            LOG(ERROR) << "Failed to query services: " << status.error_message();
        }
        
        return services;
        
    } catch (const std::exception& e) {
        LOG(ERROR) << "Exception during query_services: " << e.what();
        return {};
    }
}
    
void DiscoveryClientImpl::register_status_callback(StatusCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    status_callback_ = callback;
    // TODO: Implement streaming/polling for status updates
}
    
ServiceInfo DiscoveryClientImpl::convert_proto_to_service_info(
    const swdv::service_discovery::service_info_t& proto_info) {
    
    ServiceInfo info;
    info.name = proto_info.name();
    info.version = proto_info.version();
    info.description = proto_info.description();
    info.ifex_schema = proto_info.ifex_schema();
    
    // Convert endpoint
    info.endpoint.address = proto_info.endpoint().address();
    switch (proto_info.endpoint().transport()) {
        case swdv::service_discovery::transport_type_t::GRPC:
            info.endpoint.transport = ServiceEndpoint::Transport::GRPC;
            break;
        case swdv::service_discovery::transport_type_t::HTTP_REST:
            info.endpoint.transport = ServiceEndpoint::Transport::HTTP_REST;
            break;
        case swdv::service_discovery::transport_type_t::DBUS:
            info.endpoint.transport = ServiceEndpoint::Transport::DBUS;
            break;
        default:
            info.endpoint.transport = ServiceEndpoint::Transport::GRPC;
    }
    
    // Convert protobuf status to ServiceStatus enum
    switch (proto_info.status()) {
        case swdv::service_discovery::service_status_t::AVAILABLE:
            info.status = ServiceStatus::AVAILABLE;
            break;
        case swdv::service_discovery::service_status_t::STARTING:
            info.status = ServiceStatus::STARTING;
            break;
        case swdv::service_discovery::service_status_t::STOPPING:
            info.status = ServiceStatus::STOPPING;
            break;
        case swdv::service_discovery::service_status_t::UNAVAILABLE:
            info.status = ServiceStatus::UNAVAILABLE;
            break;
        case swdv::service_discovery::service_status_t::ERROR:
            info.status = ServiceStatus::ERROR;
            break;
        default:
            info.status = ServiceStatus::UNAVAILABLE;
    }

    // Convert methods from namespaces
    for (const auto& ns : proto_info.namespaces()) {
        for (const auto& method : ns.methods()) {
            MethodSignature sig;
            sig.service_name = info.name;
            sig.namespace_name = ns.name();
            sig.method_name = method.name();
            sig.description = method.description();
            info.methods.push_back(sig);
        }
    }

    return info;
}

swdv::service_discovery::parameter_type_t DiscoveryClientImpl::convert_type_to_proto(
    Type type) {
    
    switch (type) {
        case Type::UINT8: return swdv::service_discovery::parameter_type_t::UINT8;
        case Type::UINT16: return swdv::service_discovery::parameter_type_t::UINT16;
        case Type::UINT32: return swdv::service_discovery::parameter_type_t::UINT32;
        case Type::UINT64: return swdv::service_discovery::parameter_type_t::UINT64;
        case Type::INT8: return swdv::service_discovery::parameter_type_t::INT8;
        case Type::INT16: return swdv::service_discovery::parameter_type_t::INT16;
        case Type::INT32: return swdv::service_discovery::parameter_type_t::INT32;
        case Type::INT64: return swdv::service_discovery::parameter_type_t::INT64;
        case Type::FLOAT: return swdv::service_discovery::parameter_type_t::FLOAT;
        case Type::DOUBLE: return swdv::service_discovery::parameter_type_t::DOUBLE;
        case Type::BOOLEAN: return swdv::service_discovery::parameter_type_t::BOOLEAN;
        case Type::STRING: return swdv::service_discovery::parameter_type_t::STRING;
        case Type::BYTES: return swdv::service_discovery::parameter_type_t::BYTES;
        case Type::STRUCT: return swdv::service_discovery::parameter_type_t::STRUCT;
        case Type::ARRAY: return swdv::service_discovery::parameter_type_t::ARRAY;
        case Type::ENUM: return swdv::service_discovery::parameter_type_t::ENUM;
        default: return swdv::service_discovery::parameter_type_t::STRING;
    }
}

// Factory method
std::unique_ptr<DiscoveryClient> DiscoveryClient::create(const std::string& discovery_endpoint) {
    return std::make_unique<DiscoveryClientImpl>(discovery_endpoint);
}

} // namespace ifex