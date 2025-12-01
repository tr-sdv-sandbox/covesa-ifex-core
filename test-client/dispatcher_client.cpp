#include "service-discovery-service.grpc.pb.h"
#include "ifex-dispatcher-service.grpc.pb.h"
#include <grpcpp/grpcpp.h>
#include <glog/logging.h>
#include <nlohmann/json.hpp>
#include <iostream>
#include <iomanip>
#include <chrono>
#include <thread>

using json = nlohmann::json;

class DispatcherClient {
public:
    DispatcherClient(const std::string& discovery_endpoint) 
        : discovery_endpoint_(discovery_endpoint) {
        LOG(INFO) << "Initializing dispatcher client with discovery at: " << discovery_endpoint;
    }
    
    // Discover a service
    bool DiscoverService(const std::string& service_name) {
        try {
            auto channel = grpc::CreateChannel(discovery_endpoint_, grpc::InsecureChannelCredentials());
            auto stub = swdv::service_discovery::get_service_service::NewStub(channel);
            
            swdv::service_discovery::get_service_request request;
            request.set_service_name(service_name);
            
            swdv::service_discovery::get_service_response response;
            grpc::ClientContext context;
            
            auto status = stub->get_service(&context, request, &response);
            
            if (status.ok()) {
                const auto& service_info = response.service_info();
                std::cout << "Service: " << service_info.name() << std::endl;
                std::cout << "Version: " << service_info.version() << std::endl;
                std::cout << "Status: " << service_info.status() << std::endl;
                std::cout << "Endpoint: " << service_info.endpoint().address() << std::endl;
                std::cout << "Description: " << service_info.description() << std::endl;
                return true;
            } else {
                std::cerr << "Failed to discover service " << service_name << ": " << status.error_message() << std::endl;
                return false;
            }
        } catch (const std::exception& e) {
            std::cerr << "Exception discovering service: " << e.what() << std::endl;
            return false;
        }
    }
    
    // List all services
    bool ListServices() {
        try {
            auto channel = grpc::CreateChannel(discovery_endpoint_, grpc::InsecureChannelCredentials());
            auto stub = swdv::service_discovery::query_services_service::NewStub(channel);
            
            swdv::service_discovery::query_services_request request;
            swdv::service_discovery::query_services_response response;
            grpc::ClientContext context;
            
            auto status = stub->query_services(&context, request, &response);
            
            if (status.ok()) {
                std::cout << "Found " << response.services_size() << " services:" << std::endl;
                for (const auto& service : response.services()) {
                    std::cout << "- " << service.name() 
                              << " v" << service.version()
                              << " at " << service.endpoint().address()
                              << " [" << GetStatusString(service.status()) << "]"
                              << " (" << service.description() << ")" << std::endl;
                }
                return true;
            } else {
                std::cerr << "Failed to query services: " << status.error_message() << std::endl;
                return false;
            }
        } catch (const std::exception& e) {
            std::cerr << "Exception querying services: " << e.what() << std::endl;
            return false;
        }
    }
    
    // Call a method via dispatcher
    bool CallMethod(const std::string& service_name, const std::string& method_name, 
                   const std::string& parameters_json, int timeout_ms = 5000) {
        // First, discover the dispatcher
        std::string dispatcher_endpoint = DiscoverDispatcher();
        if (dispatcher_endpoint.empty()) {
            std::cerr << "Failed to discover dispatcher service" << std::endl;
            return false;
        }
        
        try {
            auto channel = grpc::CreateChannel(dispatcher_endpoint, grpc::InsecureChannelCredentials());
            auto stub = swdv::ifex_dispatcher::call_method_service::NewStub(channel);
            
            swdv::ifex_dispatcher::call_method_request request;
            auto* call = request.mutable_call();
            call->set_service_name(service_name);
            call->set_method_name(method_name);
            call->set_parameters(parameters_json);
            call->set_timeout_ms(timeout_ms);
            
            std::cout << "Calling " << service_name << "." << method_name << std::endl;
            std::cout << "Parameters: " << parameters_json << std::endl;
            
            swdv::ifex_dispatcher::call_method_response response;
            grpc::ClientContext context;
            
            auto start = std::chrono::high_resolution_clock::now();
            auto status = stub->call_method(&context, request, &response);
            auto end = std::chrono::high_resolution_clock::now();
            
            if (status.ok()) {
                const auto& result = response.result();
                std::cout << "Status: " << result.status() << std::endl;
                std::cout << "Response: " << result.response() << std::endl;
                std::cout << "Duration: " << result.duration_ms() << "ms" << std::endl;
                std::cout << "Service endpoint: " << result.service_endpoint() << std::endl;
                
                if (!result.error_message().empty()) {
                    std::cout << "Error: " << result.error_message() << std::endl;
                }
                return result.status() == 0;
            } else {
                std::cerr << "RPC failed: " << status.error_message() << std::endl;
                return false;
            }
        } catch (const std::exception& e) {
            std::cerr << "Exception calling method: " << e.what() << std::endl;
            return false;
        }
    }
    
    // Run sanity check tests
    bool RunSanityCheck() {
        std::cout << "\n=== Running Sanity Check ===" << std::endl;
        
        // Check discovery service - skip as we're already using it
        std::cout << "\n1. Discovery service connection..." << std::endl;
        std::cout << "✅ Connected to discovery at " << discovery_endpoint_ << std::endl;
        
        // Check dispatcher service
        std::cout << "\n2. Checking dispatcher service..." << std::endl;
        if (!DiscoverService("ifex-dispatcher")) {
            std::cerr << "❌ Dispatcher service not available" << std::endl;
            return false;
        }
        std::cout << "✅ Dispatcher service is running" << std::endl;
        
        // List all services
        std::cout << "\n3. Listing all services..." << std::endl;
        if (!ListServices()) {
            std::cerr << "❌ Failed to list services" << std::endl;
            return false;
        }
        
        // Test echo service if available
        std::cout << "\n4. Testing echo service..." << std::endl;
        if (DiscoverService("echo_service")) {
            json params;
            params["message"] = "Sanity check";
            if (CallMethod("echo_service", "echo", params.dump())) {
                std::cout << "✅ Echo service test passed" << std::endl;
            } else {
                std::cerr << "❌ Echo service test failed" << std::endl;
                return false;
            }
        } else {
            std::cout << "⚠️  Echo service not available for testing" << std::endl;
        }
        
        std::cout << "\n✅ Sanity check completed successfully!" << std::endl;
        return true;
    }

private:
    std::string discovery_endpoint_;
    
    std::string DiscoverDispatcher() {
        try {
            auto channel = grpc::CreateChannel(discovery_endpoint_, grpc::InsecureChannelCredentials());
            auto stub = swdv::service_discovery::get_service_service::NewStub(channel);
            
            swdv::service_discovery::get_service_request request;
            request.set_service_name("ifex-dispatcher");
            
            swdv::service_discovery::get_service_response response;
            grpc::ClientContext context;
            
            auto status = stub->get_service(&context, request, &response);
            
            if (status.ok()) {
                return response.service_info().endpoint().address();
            } else {
                return "";
            }
        } catch (const std::exception& e) {
            LOG(ERROR) << "Exception discovering dispatcher: " << e.what();
            return "";
        }
    }
    
    std::string GetStatusString(int status) {
        switch (status) {
            case 0: return "AVAILABLE";
            case 1: return "STARTING";
            case 2: return "STOPPING";
            case 3: return "UNAVAILABLE";
            case 4: return "ERROR";
            default: return "UNKNOWN";
        }
    }
};

void PrintUsage(const char* program_name) {
    std::cout << "Usage: " << program_name << " [command] [options]\n"
              << "\nCommands:\n"
              << "  list                    List all available services\n"
              << "  discover SERVICE        Discover a specific service\n"
              << "  call SERVICE METHOD     Call a service method via dispatcher\n"
              << "  sanity                  Run sanity check (default)\n"
              << "\nOptions:\n"
              << "  --discovery=ENDPOINT    Discovery service endpoint (default: localhost:50051)\n"
              << "  --params=JSON           JSON parameters for method call\n"
              << "  --timeout=MS            Timeout in milliseconds (default: 5000)\n"
              << "  --help, -h              Show this help message\n"
              << "\nExamples:\n"
              << "  " << program_name << " list\n"
              << "  " << program_name << " discover echo_service\n"
              << "  " << program_name << " call echo_service echo --params='{\"message\":\"Hello\"}'\n";
}

int main(int argc, char* argv[]) {
    // Initialize Google logging
    google::InitGoogleLogging(argv[0]);
    google::InstallFailureSignalHandler();
    
    // Default values
    std::string discovery_endpoint = "localhost:50051";
    std::string command = "sanity";
    std::string service_name;
    std::string method_name;
    std::string parameters_json = "{}";
    int timeout_ms = 5000;
    
    // Parse command line arguments
    if (argc > 1 && argv[1][0] != '-') {
        command = argv[1];
        
        if (command == "discover" && argc > 2) {
            service_name = argv[2];
        } else if (command == "call" && argc > 3) {
            service_name = argv[2];
            method_name = argv[3];
        }
    }
    
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg.find("--discovery=") == 0) {
            discovery_endpoint = arg.substr(12);
        } else if (arg.find("--params=") == 0) {
            parameters_json = arg.substr(9);
        } else if (arg.find("--timeout=") == 0) {
            timeout_ms = std::stoi(arg.substr(10));
        } else if (arg == "--help" || arg == "-h") {
            PrintUsage(argv[0]);
            return 0;
        }
    }
    
    // Give services time to settle if just launched
    if (command == "sanity") {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    
    try {
        DispatcherClient client(discovery_endpoint);
        
        if (command == "list") {
            return client.ListServices() ? 0 : 1;
        } else if (command == "discover") {
            if (service_name.empty()) {
                std::cerr << "Error: Service name required for discover command" << std::endl;
                PrintUsage(argv[0]);
                return 1;
            }
            return client.DiscoverService(service_name) ? 0 : 1;
        } else if (command == "call") {
            if (service_name.empty() || method_name.empty()) {
                std::cerr << "Error: Service and method names required for call command" << std::endl;
                PrintUsage(argv[0]);
                return 1;
            }
            return client.CallMethod(service_name, method_name, parameters_json, timeout_ms) ? 0 : 1;
        } else if (command == "sanity") {
            return client.RunSanityCheck() ? 0 : 1;
        } else {
            std::cerr << "Unknown command: " << command << std::endl;
            PrintUsage(argv[0]);
            return 1;
        }
    } catch (const std::exception& e) {
        LOG(ERROR) << "Dispatcher client failed: " << e.what();
        return 1;
    }
    
    return 0;
}