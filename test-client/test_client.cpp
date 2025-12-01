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

class TestClient {
public:
    TestClient(const std::string& discovery_endpoint) 
        : discovery_endpoint_(discovery_endpoint) {
        LOG(INFO) << "Initializing test client with discovery at: " << discovery_endpoint;
    }
    
    void Run() {
        std::cout << "\n=== IFEX Test Client ===" << std::endl;
        std::cout << "Discovery endpoint: " << discovery_endpoint_ << std::endl;
        
        // Step 1: Discover dispatcher service
        std::cout << "\n1. Discovering dispatcher service..." << std::endl;
        std::string dispatcher_endpoint = DiscoverService("ifex-dispatcher");
        if (dispatcher_endpoint.empty()) {
            LOG(ERROR) << "Failed to discover dispatcher service";
            return;
        }
        std::cout << "   Found dispatcher at: " << dispatcher_endpoint << std::endl;
        
        // Step 2: Discover echo service
        std::cout << "\n2. Discovering echo service..." << std::endl;
        std::string echo_service_name = DiscoverService("echo_service");
        if (echo_service_name.empty()) {
            LOG(ERROR) << "Failed to discover echo service";
            return;
        }
        std::cout << "   Found echo service" << std::endl;
        
        // Step 3: Query all available services
        std::cout << "\n3. Querying all available services..." << std::endl;
        QueryAllServices();
        
        // Step 4: Test echo service via dispatcher
        std::cout << "\n4. Testing echo service via dispatcher..." << std::endl;
        TestEchoViaDispatcher(dispatcher_endpoint);
        
        // Step 5: Test echo with delay via dispatcher
        std::cout << "\n5. Testing echo_with_delay via dispatcher..." << std::endl;
        TestEchoWithDelayViaDispatcher(dispatcher_endpoint);
        
        // Step 6: Test concat via dispatcher
        std::cout << "\n6. Testing concat via dispatcher..." << std::endl;
        TestConcatViaDispatcher(dispatcher_endpoint);
        
        // Step 7: Parameter validation removed from dispatcher
        // Validation now happens at service level during actual calls
        
        std::cout << "\n=== Test completed successfully! ===" << std::endl;
    }

private:
    std::string discovery_endpoint_;
    
    std::string DiscoverService(const std::string& service_name) {
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
                std::cout << "   Service: " << service_info.name() << std::endl;
                std::cout << "   Version: " << service_info.version() << std::endl;
                std::cout << "   Status: " << service_info.status() << std::endl;
                std::cout << "   Endpoint: " << service_info.endpoint().address() << std::endl;
                return service_info.endpoint().address();
            } else {
                LOG(ERROR) << "Failed to get service " << service_name << ": " << status.error_message();
                return "";
            }
        } catch (const std::exception& e) {
            LOG(ERROR) << "Exception discovering service: " << e.what();
            return "";
        }
    }
    
    void QueryAllServices() {
        try {
            auto channel = grpc::CreateChannel(discovery_endpoint_, grpc::InsecureChannelCredentials());
            auto stub = swdv::service_discovery::query_services_service::NewStub(channel);
            
            swdv::service_discovery::query_services_request request;
            // Empty filter to get all services
            
            swdv::service_discovery::query_services_response response;
            grpc::ClientContext context;
            
            auto status = stub->query_services(&context, request, &response);
            
            if (status.ok()) {
                std::cout << "   Found " << response.services_size() << " services:" << std::endl;
                for (const auto& service : response.services()) {
                    std::cout << "   - " << service.name() 
                              << " v" << service.version()
                              << " at " << service.endpoint().address()
                              << " (" << service.description() << ")" << std::endl;
                }
            } else {
                LOG(ERROR) << "Failed to query services: " << status.error_message();
            }
        } catch (const std::exception& e) {
            LOG(ERROR) << "Exception querying services: " << e.what();
        }
    }
    
    void TestEchoViaDispatcher(const std::string& dispatcher_endpoint) {
        try {
            auto channel = grpc::CreateChannel(dispatcher_endpoint, grpc::InsecureChannelCredentials());
            auto stub = swdv::ifex_dispatcher::call_method_service::NewStub(channel);
            
            // Prepare parameters
            json params;
            params["message"] = "Hello, IFEX!";
            
            swdv::ifex_dispatcher::call_method_request request;
            auto* call = request.mutable_call();
            call->set_service_name("echo_service");
            call->set_method_name("echo");
            call->set_parameters(params.dump());
            call->set_timeout_ms(5000);
            
            std::cout << "   Calling echo with message: " << params["message"] << std::endl;
            std::cout << "   JSON parameters: " << params.dump(2) << std::endl;
            
            swdv::ifex_dispatcher::call_method_response response;
            grpc::ClientContext context;
            
            auto start = std::chrono::high_resolution_clock::now();
            auto status = stub->call_method(&context, request, &response);
            auto end = std::chrono::high_resolution_clock::now();
            
            if (status.ok()) {
                const auto& result = response.result();
                std::cout << "   Status: " << result.status() << std::endl;
                std::cout << "   Response: " << result.response() << std::endl;
                std::cout << "   Duration: " << result.duration_ms() << "ms" << std::endl;
                std::cout << "   Service endpoint: " << result.service_endpoint() << std::endl;
                
                if (!result.error_message().empty()) {
                    std::cout << "   Error: " << result.error_message() << std::endl;
                }
            } else {
                LOG(ERROR) << "RPC failed: " << status.error_message();
            }
        } catch (const std::exception& e) {
            LOG(ERROR) << "Exception calling echo: " << e.what();
        }
    }
    
    void TestEchoWithDelayViaDispatcher(const std::string& dispatcher_endpoint) {
        try {
            auto channel = grpc::CreateChannel(dispatcher_endpoint, grpc::InsecureChannelCredentials());
            auto stub = swdv::ifex_dispatcher::call_method_service::NewStub(channel);
            
            // Prepare parameters
            json params;
            params["message"] = "Delayed Hello!";
            params["delay_ms"] = 500;
            
            swdv::ifex_dispatcher::call_method_request request;
            auto* call = request.mutable_call();
            call->set_service_name("echo_service");
            call->set_method_name("echo_with_delay");
            call->set_parameters(params.dump());
            call->set_timeout_ms(5000);
            
            std::cout << "   Calling echo_with_delay with message: " << params["message"] 
                      << ", delay: " << params["delay_ms"] << "ms" << std::endl;
            std::cout << "   JSON parameters: " << params.dump(2) << std::endl;
            
            swdv::ifex_dispatcher::call_method_response response;
            grpc::ClientContext context;
            
            auto start = std::chrono::high_resolution_clock::now();
            auto status = stub->call_method(&context, request, &response);
            auto end = std::chrono::high_resolution_clock::now();
            
            if (status.ok()) {
                const auto& result = response.result();
                std::cout << "   Status: " << result.status() << std::endl;
                std::cout << "   Response: " << result.response() << std::endl;
                std::cout << "   Total duration: " << result.duration_ms() << "ms" << std::endl;
                
                if (!result.error_message().empty()) {
                    std::cout << "   Error: " << result.error_message() << std::endl;
                }
            } else {
                LOG(ERROR) << "RPC failed: " << status.error_message();
            }
        } catch (const std::exception& e) {
            LOG(ERROR) << "Exception calling echo_with_delay: " << e.what();
        }
    }
    
    void TestConcatViaDispatcher(const std::string& dispatcher_endpoint) {
        try {
            auto channel = grpc::CreateChannel(dispatcher_endpoint, grpc::InsecureChannelCredentials());
            auto stub = swdv::ifex_dispatcher::call_method_service::NewStub(channel);
            
            // Prepare parameters
            json params;
            params["first"] = "Hello";
            params["second"] = "World";
            params["separator"] = ", ";
            
            swdv::ifex_dispatcher::call_method_request request;
            auto* call = request.mutable_call();
            call->set_service_name("echo_service");
            call->set_method_name("concat");
            call->set_parameters(params.dump());
            call->set_timeout_ms(5000);
            
            std::cout << "   Calling concat with: \"" << params["first"] 
                      << "\" + \"" << params["separator"] 
                      << "\" + \"" << params["second"] << "\"" << std::endl;
            std::cout << "   JSON parameters: " << params.dump(2) << std::endl;
            
            swdv::ifex_dispatcher::call_method_response response;
            grpc::ClientContext context;
            
            auto status = stub->call_method(&context, request, &response);
            
            if (status.ok()) {
                const auto& result = response.result();
                std::cout << "   Status: " << result.status() << std::endl;
                std::cout << "   Response: " << result.response() << std::endl;
                std::cout << "   Duration: " << result.duration_ms() << "ms" << std::endl;
                
                if (!result.error_message().empty()) {
                    std::cout << "   Error: " << result.error_message() << std::endl;
                }
            } else {
                LOG(ERROR) << "RPC failed: " << status.error_message();
            }
        } catch (const std::exception& e) {
            LOG(ERROR) << "Exception calling concat: " << e.what();
        }
    }
    
    // Parameter validation removed from dispatcher - validation now happens at service level
    /*void TestParameterValidation(const std::string& dispatcher_endpoint) {
        try {
            auto channel = grpc::CreateChannel(dispatcher_endpoint, grpc::InsecureChannelCredentials());
            auto stub = swdv::ifex_dispatcher::validate_parameters_service::NewStub(channel);
            
            // Test valid parameters
            {
                json params;
                params["message"] = "Test message";
                
                swdv::ifex_dispatcher::validate_parameters_request request;
                request.set_service_name("echo_service");
                request.set_method_name("echo");
                request.set_parameters(params.dump());
                
                swdv::ifex_dispatcher::validate_parameters_response response;
                grpc::ClientContext context;
                
                std::cout << "   Validating correct parameters for echo..." << std::endl;
                auto status = stub->validate_parameters(&context, request, &response);
                
                if (status.ok()) {
                    std::cout << "   Valid: " << (response.is_valid() ? "YES" : "NO") << std::endl;
                    if (!response.is_valid()) {
                        for (const auto& error : response.errors()) {
                            std::cout << "   Error in " << error.field_name() 
                                      << ": " << error.error_message() << std::endl;
                        }
                    }
                }
            }
            
            // Test invalid parameters (missing required field)
            {
                json params;
                // Missing required "message" field
                
                swdv::ifex_dispatcher::validate_parameters_request request;
                request.set_service_name("echo_service");
                request.set_method_name("echo");
                request.set_parameters(params.dump());
                
                swdv::ifex_dispatcher::validate_parameters_response response;
                grpc::ClientContext context;
                
                std::cout << "   Validating incorrect parameters (missing required field)..." << std::endl;
                auto status = stub->validate_parameters(&context, request, &response);
                
                if (status.ok()) {
                    std::cout << "   Valid: " << (response.is_valid() ? "YES" : "NO") << std::endl;
                    if (!response.is_valid()) {
                        for (const auto& error : response.errors()) {
                            std::cout << "   Error in " << error.field_name() 
                                      << ": " << error.error_message() << std::endl;
                        }
                    }
                }
            }
        } catch (const std::exception& e) {
            LOG(ERROR) << "Exception testing parameter validation: " << e.what();
        }
    }*/
};

int main(int argc, char* argv[]) {
    // Initialize Google logging
    google::InitGoogleLogging(argv[0]);
    google::InstallFailureSignalHandler();
    
    // Parse command line arguments
    std::string discovery_endpoint = "localhost:50051";
    
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg.find("--discovery=") == 0) {
            discovery_endpoint = arg.substr(12);
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0] << " [options]\n"
                      << "Options:\n"
                      << "  --discovery=ENDPOINT  Discovery service endpoint (default: localhost:50051)\n"
                      << "  --help, -h           Show this help message\n";
            return 0;
        }
    }
    
    // Give services time to start if just launched
    std::this_thread::sleep_for(std::chrono::seconds(1));
    
    try {
        TestClient client(discovery_endpoint);
        client.Run();
    } catch (const std::exception& e) {
        LOG(ERROR) << "Test client failed: " << e.what();
        return 1;
    }
    
    return 0;
}