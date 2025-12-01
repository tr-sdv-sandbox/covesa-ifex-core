#include "dispatcher_server.hpp"
#include "service-discovery-service.grpc.pb.h"
#include <ifex/parser.hpp>
#include <ifex/network.hpp>
#include <glog/logging.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/create_channel.h>
#include <sstream>
#include <chrono>

namespace ifex::reference {

using json = nlohmann::json;

DispatcherServer::DispatcherServer(const std::string& discovery_endpoint)
    : discovery_endpoint_(discovery_endpoint) {
    LOG(INFO) << "Initializing Dispatcher Server with discovery at " << discovery_endpoint;
    
    // Initialize core components
    discovery_client_ = ifex::DiscoveryClient::create(discovery_endpoint);
    dynamic_caller_ = ifex::DynamicCaller::create(discovery_endpoint);
}

DispatcherServer::~DispatcherServer() {
    if (server_) {
        Shutdown();
    }
}

void DispatcherServer::Start(const std::string& listen_address) {
    grpc::ServerBuilder builder;
    builder.AddListeningPort(listen_address, grpc::InsecureServerCredentials());
    
    // Register all service implementations
    builder.RegisterService(static_cast<swdv::ifex_dispatcher::call_method_service::Service*>(this));
    
    server_ = builder.BuildAndStart();
    LOG(INFO) << "Dispatcher server listening on " << listen_address;
}

void DispatcherServer::Shutdown() {
    if (server_) {
        LOG(INFO) << "Shutting down dispatcher server";
        server_->Shutdown();
    }
}

void DispatcherServer::Wait() {
    if (server_) {
        server_->Wait();
    }
}

// call_method implementation
grpc::Status DispatcherServer::call_method(
    grpc::ServerContext* context,
    const swdv::ifex_dispatcher::call_method_request* request,
    swdv::ifex_dispatcher::call_method_response* response) {
    
    const auto& call_request = request->call();
    
    LOG(INFO) << "Calling method: " << call_request.service_name() 
              << "." << call_request.method_name();
    
    auto start_time = std::chrono::high_resolution_clock::now();
    
    // Find the target service endpoint
    std::string service_endpoint = FindServiceEndpoint(call_request.service_name());
    if (service_endpoint.empty()) {
        LOG(ERROR) << "Service endpoint not found for: " << call_request.service_name()
                   << " (method: " << call_request.method_name() << ")";
        response->mutable_result()->set_status(swdv::ifex_dispatcher::SERVICE_UNAVAILABLE);
        response->mutable_result()->set_error_message("Service not found: " + call_request.service_name());
        response->mutable_result()->set_duration_ms(0);
        return grpc::Status::OK;
    }

    // Get service IFEX schema for validation and dynamic calling
    std::string ifex_schema = GetServiceSchema(call_request.service_name());
    if (ifex_schema.empty()) {
        LOG(ERROR) << "Service schema not found for: " << call_request.service_name()
                   << " (method: " << call_request.method_name() << ")";
        response->mutable_result()->set_status(swdv::ifex_dispatcher::SERVICE_UNAVAILABLE);
        response->mutable_result()->set_error_message("Service schema not available: " + call_request.service_name());
        response->mutable_result()->set_duration_ms(0);
        return grpc::Status::OK;
    }

    // Parse parameters
    LOG(INFO) << "About to parse parameters for " << call_request.service_name() 
              << "." << call_request.method_name();
    json parameters;
    try {
        if (!call_request.parameters().empty()) {
            parameters = json::parse(call_request.parameters());
            LOG(INFO) << "Successfully parsed parameters: " << parameters.dump(2);
        } else {
            LOG(INFO) << "No parameters provided";
        }
    } catch (const json::parse_error& e) {
        LOG(ERROR) << "Failed to parse JSON parameters: " << e.what();
        response->mutable_result()->set_status(swdv::ifex_dispatcher::INVALID_PARAMETERS);
        response->mutable_result()->set_error_message("Invalid JSON parameters: " + std::string(e.what()));
        response->mutable_result()->set_duration_ms(0);
        return grpc::Status::OK;
    }

    // Validate parameters against IFEX schema
    std::vector<std::string> validation_errors;
    LOG(INFO) << "About to validate parameters against IFEX schema";
    if (!ValidateMethodParameters(call_request.service_name(), call_request.method_name(), 
                                parameters, ifex_schema, validation_errors)) {
        LOG(INFO) << "Validation failed with " << validation_errors.size() << " errors";
        
        // Check if this is a method not found error
        bool method_not_found = false;
        for (const auto& error : validation_errors) {
            if (error.find("Method") != std::string::npos && 
                error.find("not found") != std::string::npos) {
                method_not_found = true;
                break;
            }
        }
        
        std::stringstream error_msg;
        error_msg << "Parameter validation failed: ";
        for (const auto& error : validation_errors) {
            LOG(INFO) << "Validation error: " << error;
            error_msg << error << "; ";
        }
        
        if (method_not_found) {
            LOG(INFO) << "Setting METHOD_NOT_FOUND response";
            response->mutable_result()->set_status(swdv::ifex_dispatcher::METHOD_NOT_FOUND);
        } else {
            LOG(INFO) << "Setting INVALID_PARAMETERS response";
            response->mutable_result()->set_status(swdv::ifex_dispatcher::INVALID_PARAMETERS);
        }
        response->mutable_result()->set_error_message(error_msg.str());
        response->mutable_result()->set_duration_ms(0);
        LOG(INFO) << "Returning from call_method due to validation failure";
        return grpc::Status::OK;
    }
    LOG(INFO) << "Validation passed";

    // Execute the actual method call via dynamic caller
    LOG(INFO) << "About to call dynamic_caller for " << call_request.service_name() 
              << "." << call_request.method_name();
    
    ifex::CallRequest dynamic_request;
    dynamic_request.service_name = call_request.service_name();
    dynamic_request.method_name = call_request.method_name();
    dynamic_request.parameters_json = call_request.parameters();
    if (call_request.timeout_ms() > 0) {
        dynamic_request.timeout = std::chrono::milliseconds(call_request.timeout_ms());
    }
    
    ifex::ServiceEndpoint endpoint;
    endpoint.address = service_endpoint;
    endpoint.transport = ifex::ServiceEndpoint::Transport::GRPC;
    
    LOG(INFO) << "Calling dynamic_caller_->call_method...";
    auto dynamic_response = dynamic_caller_->call_method(dynamic_request, endpoint, ifex_schema);
    LOG(INFO) << "dynamic_caller_->call_method returned";
    
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

    // Populate response from dynamic caller result
    response->mutable_result()->set_service_endpoint(service_endpoint);
    response->mutable_result()->set_duration_ms(static_cast<uint32_t>(duration.count()));
    
    if (dynamic_response.success) {
        response->mutable_result()->set_status(swdv::ifex_dispatcher::SUCCESS);
        response->mutable_result()->set_response(dynamic_response.result_json);
    } else {
        // Map internal status to dispatcher status
        switch (dynamic_response.status) {
            case ifex::CallResponse::Status::TIMEOUT:
                response->mutable_result()->set_status(swdv::ifex_dispatcher::TIMEOUT);
                break;
            case ifex::CallResponse::Status::SERVICE_UNAVAILABLE:
                response->mutable_result()->set_status(swdv::ifex_dispatcher::SERVICE_UNAVAILABLE);
                break;
            case ifex::CallResponse::Status::METHOD_NOT_FOUND:
                response->mutable_result()->set_status(swdv::ifex_dispatcher::METHOD_NOT_FOUND);
                break;
            case ifex::CallResponse::Status::INVALID_PARAMETERS:
                response->mutable_result()->set_status(swdv::ifex_dispatcher::INVALID_PARAMETERS);
                break;
            default:
                response->mutable_result()->set_status(swdv::ifex_dispatcher::FAILED);
                break;
        }
        response->mutable_result()->set_error_message(dynamic_response.error_message);
    }

    return grpc::Status::OK;
}


std::string DispatcherServer::FindServiceEndpoint(const std::string& service_name) {
    try {
        LOG(INFO) << "Looking up endpoint for service: " << service_name;
        
        // Use discovery client from core library
        auto service_info = discovery_client_->get_service(service_name);
        
        if (service_info.has_value()) {
            LOG(INFO) << "Found endpoint for " << service_name << ": " << service_info->endpoint.address;
            return service_info->endpoint.address;
        } else {
            LOG(WARNING) << "No endpoint found for service: " << service_name;
        }
    } catch (const std::exception& e) {
        LOG(ERROR) << "Failed to find service " << service_name << ": " << e.what();
    }
    return "";
}

std::string DispatcherServer::GetServiceSchema(const std::string& service_name) {
    try {
        // Use discovery client from core library
        ifex::ServiceFilter filter;
        filter.name_pattern = service_name;
        
        auto services = discovery_client_->query_services(filter);
        
        for (const auto& service : services) {
            if (service.name == service_name) {
                LOG(INFO) << "Found IFEX schema for " << service_name << ", length: " << service.ifex_schema.length();
                return service.ifex_schema;
            }
        }
        
        LOG(WARNING) << "No schema found for service: " << service_name;
    } catch (const std::exception& e) {
        LOG(ERROR) << "Failed to get schema for service " << service_name << ": " << e.what();
    }
    return "";
}

bool DispatcherServer::ValidateMethodParameters(const std::string& service_name,
                                              const std::string& method_name,
                                              const json& parameters,
                                              const std::string& ifex_schema,
                                              std::vector<std::string>& errors) {
    LOG(INFO) << "ValidateMethodParameters: Starting validation for " << service_name << "." << method_name;
    try {
        if (ifex_schema.empty()) {
            errors.push_back("No IFEX schema available for service: " + service_name);
            return false;
        }

        // Basic JSON validation
        if (!parameters.is_object() && !parameters.is_null()) {
            errors.push_back("Parameters must be a JSON object");
            return false;
        }

        // Parse IFEX schema and validate parameters using IFEX parser
        LOG(INFO) << "ValidateMethodParameters: Creating parser for IFEX schema";
        auto parser = ifex::Parser::create(ifex_schema);
        if (!parser) {
            errors.push_back("Failed to parse IFEX schema for service: " + service_name);
            return false;
        }
        LOG(INFO) << "ValidateMethodParameters: Parser created successfully";

        // Check if method exists
        LOG(INFO) << "ValidateMethodParameters: Checking if method exists: " << method_name;
        if (!parser->has_method(method_name)) {
            errors.push_back("Method '" + method_name + "' not found in service: " + service_name);
            return false;
        }
        LOG(INFO) << "ValidateMethodParameters: Method exists";

        // Validate parameters against IFEX schema
        LOG(INFO) << "ValidateMethodParameters: About to validate parameters";
        auto validation_result = parser->validate_parameters(method_name, parameters.dump());
        LOG(INFO) << "ValidateMethodParameters: Validation completed, valid=" << validation_result.valid;
        
        if (!validation_result.valid) {
            // Add all validation errors
            errors.insert(errors.end(), validation_result.errors.begin(), validation_result.errors.end());
            return false;
        }

        return true;
        
    } catch (const std::exception& e) {
        errors.push_back("Validation error: " + std::string(e.what()));
        return false;
    }
}

bool DispatcherServer::RegisterWithDiscovery(int port, const std::string& ifex_schema) {
    try {
        LOG(INFO) << "Registering IFEX Dispatcher with service discovery on port " << port;
        
        // Get primary IP address instead of using localhost
        std::string primary_ip = ifex::network::get_primary_ip_address();
        if (primary_ip.empty()) {
            LOG(WARNING) << "Could not determine primary IP address, falling back to localhost";
            primary_ip = "localhost";
        }
        
        // Create channel to discovery service
        auto channel = grpc::CreateChannel(discovery_endpoint_, grpc::InsecureChannelCredentials());
        auto stub = swdv::service_discovery::register_service_service::NewStub(channel);
        
        // Create service info
        swdv::service_discovery::register_service_request request;
        auto* service_info = request.mutable_service_info();
        
        service_info->set_name("ifex-dispatcher");
        service_info->set_version("1.0.0");
        service_info->set_description("IFEX Dynamic Method Dispatcher Service");
        service_info->set_status(swdv::service_discovery::service_status_t::AVAILABLE);
        service_info->set_ifex_schema(ifex_schema);
        
        // Set endpoint with actual IP address
        auto* endpoint = service_info->mutable_endpoint();
        endpoint->set_address(primary_ip + ":" + std::to_string(port));
        endpoint->set_transport(swdv::service_discovery::transport_type_t::GRPC);
        
        LOG(INFO) << "Registering with endpoint: " << endpoint->address();
        
        // Make the call
        swdv::service_discovery::register_service_response response;
        grpc::ClientContext context;
        
        auto status = stub->register_service(&context, request, &response);
        
        if (status.ok() && !response.registration_id().empty()) {
            LOG(INFO) << "Successfully registered with service discovery, ID: " << response.registration_id();
            return true;
        } else {
            LOG(ERROR) << "Failed to register with service discovery: " << status.error_message();
            return false;
        }
        
    } catch (const std::exception& e) {
        LOG(ERROR) << "Registration failed: " << e.what();
        return false;
    }
}

} // namespace ifex::reference