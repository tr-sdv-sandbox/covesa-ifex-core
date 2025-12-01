#include <glog/logging.h>
#include <grpcpp/grpcpp.h>
#include <signal.h>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "departure_orchestration_server.hpp"
#include "service-discovery-service.grpc.pb.h"

namespace {
std::unique_ptr<grpc::Server> server;
volatile sig_atomic_t shutdown_requested = 0;

void SignalHandler(int signal) {
    LOG(WARNING) << "Received signal " << signal << ", initiating shutdown...";
    shutdown_requested = 1;
    if (server) {
        server->Shutdown();
    }
}

void PrintUsage(const char* program_name) {
    std::cerr << "Usage: " << program_name << " [options]\n"
              << "Options:\n"
              << "  --address=<addr:port>       Service address (default: 0.0.0.0:50063)\n"
              << "  --discovery_address=<addr>  Discovery service address (default: localhost:50051)\n"
              << "  --ifex_schema=<path>        Path to IFEX schema file\n"
              << "  --help                      Show this help message\n";
}

struct Config {
    std::string address = "0.0.0.0:50063";
    std::string discovery_address = "localhost:50051";
    std::string ifex_schema = "";  // Will be set based on env or args
};

Config ParseArgs(int argc, char* argv[]) {
    Config config;
    
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        
        if (arg == "--help" || arg == "-h") {
            PrintUsage(argv[0]);
            exit(0);
        }
        
        size_t eq_pos = arg.find('=');
        if (eq_pos != std::string::npos) {
            std::string key = arg.substr(0, eq_pos);
            std::string value = arg.substr(eq_pos + 1);
            
            if (key == "--address") {
                config.address = value;
            } else if (key == "--discovery_address") {
                config.discovery_address = value;
            } else if (key == "--ifex_schema") {
                config.ifex_schema = value;
            }
        }
    }
    
    return config;
}

// Find dispatcher endpoint through discovery
std::string FindDispatcherEndpoint(const std::string& discovery_endpoint) {
    try {
        auto channel = grpc::CreateChannel(discovery_endpoint, grpc::InsecureChannelCredentials());
        auto stub = swdv::service_discovery::query_services_service::NewStub(channel);
        
        swdv::service_discovery::query_services_request request;
        auto* filter = request.mutable_filter();
        filter->set_name_pattern("ifex-dispatcher");
        
        swdv::service_discovery::query_services_response response;
        grpc::ClientContext context;
        
        auto status = stub->query_services(&context, request, &response);
        if (!status.ok()) {
            LOG(ERROR) << "Failed to query discovery service: " << status.error_message();
            return "";
        }
        
        if (response.services_size() == 0) {
            LOG(ERROR) << "No dispatcher service found";
            return "";
        }
        
        // Use the first healthy dispatcher
        for (const auto& service : response.services()) {
            if (service.status() == swdv::service_discovery::AVAILABLE) {
                return service.endpoint().address();
            }
        }
        
        return "";
    } catch (const std::exception& e) {
        LOG(ERROR) << "Exception finding dispatcher: " << e.what();
        return "";
    }
}

} // namespace

int main(int argc, char* argv[]) {
    // Initialize logging
    google::InitGoogleLogging(argv[0]);
    FLAGS_logtostderr = 1;
    
    // Parse command line arguments
    Config config = ParseArgs(argc, argv);
    
    // If no schema specified, check environment variable
    if (config.ifex_schema.empty()) {
        const char* schema_dir = std::getenv("IFEX_SCHEMA_DIR");
        if (schema_dir) {
            config.ifex_schema = std::string(schema_dir) + "/departure-orchestration-service.ifex.yml";
        } else {
            // Default to build directory structure
            config.ifex_schema = "./ifex/departure-orchestration-service.ifex.yml";
        }
    }
    
    // Install signal handlers
    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);
    
    LOG(INFO) << "Starting IFEX Departure Orchestration Service";
    LOG(INFO) << "Service address: " << config.address;
    LOG(INFO) << "Discovery address: " << config.discovery_address;
    
    try {
        // Create service implementation
        auto service = std::make_shared<swdv::departure_orchestration_service::DepartureOrchestrationServiceImpl>();
        
        // Start service
        service->Start();
        
        // Find and set dispatcher endpoint
        std::string dispatcher_endpoint = FindDispatcherEndpoint(config.discovery_address);
        if (!dispatcher_endpoint.empty()) {
            service->SetDispatcherEndpoint(dispatcher_endpoint);
            LOG(INFO) << "Found dispatcher at: " << dispatcher_endpoint;
        } else {
            LOG(WARNING) << "Could not find dispatcher - service calls will fail";
        }
        
        // Build and start server
        grpc::ServerBuilder builder;
        builder.AddListeningPort(config.address, grpc::InsecureServerCredentials());
        
        // Register all service interfaces
        builder.RegisterService(
            static_cast<swdv::departure_orchestration_service::schedule_departure_service::Service*>(service.get()));
        builder.RegisterService(
            static_cast<swdv::departure_orchestration_service::get_schedule_status_service::Service*>(service.get()));
        builder.RegisterService(
            static_cast<swdv::departure_orchestration_service::cancel_schedule_service::Service*>(service.get()));
        builder.RegisterService(
            static_cast<swdv::departure_orchestration_service::modify_schedule_service::Service*>(service.get()));
        builder.RegisterService(
            static_cast<swdv::departure_orchestration_service::get_active_schedules_service::Service*>(service.get()));
        
        server = builder.BuildAndStart();
        if (!server) {
            LOG(ERROR) << "Failed to start gRPC server";
            return 1;
        }
        
        LOG(INFO) << "Server listening on " << config.address;
        
        // Extract port from address
        size_t colon_pos = config.address.find_last_of(':');
        int port = 50063;
        if (colon_pos != std::string::npos) {
            port = std::stoi(config.address.substr(colon_pos + 1));
        }
        
        // Register with discovery service
        if (!service->RegisterWithDiscovery(config.discovery_address, port, "departure_orchestration_service", config.ifex_schema)) {
            LOG(WARNING) << "Failed to register with discovery service";
            // Continue anyway - service can still work without discovery
        }
        
        // Wait for shutdown
        while (!shutdown_requested) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        
        LOG(INFO) << "Shutting down service...";
        service->Stop();
        server->Shutdown();
        
    } catch (const std::exception& e) {
        LOG(ERROR) << "Exception in main: " << e.what();
        return 1;
    }
    
    LOG(INFO) << "Service shutdown complete";
    return 0;
}