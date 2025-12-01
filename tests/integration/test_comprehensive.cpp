#include <gtest/gtest.h>
#include <glog/logging.h>
#include "ifex-dispatcher-service.grpc.pb.h"
#include "service-discovery-service.grpc.pb.h"
#include <grpcpp/grpcpp.h>
#include <nlohmann/json.hpp>
#include <thread>
#include <chrono>
#include <random>
#include <atomic>
#include <set>
#include "test_fixture.hpp"

using json = nlohmann::json;
using namespace std::chrono_literals;

class ComprehensiveIntegrationTest : public IntegrationTestFixture {
protected:
    void SetUp() override {
        IntegrationTestFixture::SetUp();
        registered_services_.clear();
    }
    
    void TearDown() override {
        // Clean up all registered services
        auto unregister_stub = swdv::service_discovery::unregister_service_service::NewStub(discovery_channel_);
        
        for (const auto& id : registered_services_) {
            swdv::service_discovery::unregister_service_request request;
            request.set_registration_id(id);
            
            swdv::service_discovery::unregister_service_response response;
            grpc::ClientContext context;
            unregister_stub->unregister_service(&context, request, &response);
        }
        
        IntegrationTestFixture::TearDown();
    }
    
    std::string RegisterTestService(const std::string& name, int port, 
                                   const std::string& version = "1.0.0",
                                   swdv::service_discovery::service_status_t status = 
                                       swdv::service_discovery::service_status_t::AVAILABLE) {
        auto stub = swdv::service_discovery::register_service_service::NewStub(discovery_channel_);
        
        swdv::service_discovery::register_service_request request;
        auto* service_info = request.mutable_service_info();
        
        service_info->set_name(name);
        service_info->set_version(version);
        service_info->set_description("Test service " + name);
        service_info->set_status(status);
        service_info->set_ifex_schema(R"(
name: )" + name + R"(
version: )" + version + R"(
methods:
  test_method:
    description: Test method
    input:
      - name: message
        type: string
    output:
      - name: response
        type: string
)");
        
        auto* endpoint = service_info->mutable_endpoint();
        endpoint->set_address("localhost:" + std::to_string(port));
        endpoint->set_transport(swdv::service_discovery::transport_type_t::GRPC);
        
        swdv::service_discovery::register_service_response response;
        grpc::ClientContext context;
        
        auto status_rpc = stub->register_service(&context, request, &response);
        if (status_rpc.ok() && !response.registration_id().empty()) {
            registered_services_.insert(response.registration_id());
            return response.registration_id();
        }
        
        return "";
    }
    
    std::set<std::string> registered_services_;
};

// Test 1: Service Lifecycle Management
TEST_F(ComprehensiveIntegrationTest, ServiceLifecycle) {
    // Register a service
    std::string reg_id = RegisterTestService("lifecycle_test", 30001);
    ASSERT_FALSE(reg_id.empty());
    
    // Verify service is registered
    auto get_stub = swdv::service_discovery::get_service_service::NewStub(discovery_channel_);
    
    swdv::service_discovery::get_service_request get_request;
    get_request.set_service_name("lifecycle_test");
    
    swdv::service_discovery::get_service_response get_response;
    grpc::ClientContext get_context;
    
    auto status = get_stub->get_service(&get_context, get_request, &get_response);
    ASSERT_TRUE(status.ok());
    EXPECT_EQ(get_response.service_info().name(), "lifecycle_test");
    EXPECT_EQ(get_response.service_info().status(), swdv::service_discovery::service_status_t::AVAILABLE);
    
    // Heartbeat - update service status
    auto heartbeat_stub = swdv::service_discovery::heartbeat_service::NewStub(discovery_channel_);
    
    swdv::service_discovery::heartbeat_request hb_request;
    hb_request.set_registration_id(reg_id);
    hb_request.set_status(swdv::service_discovery::service_status_t::STOPPING);
    
    swdv::service_discovery::heartbeat_response hb_response;
    grpc::ClientContext hb_context;
    
    status = heartbeat_stub->heartbeat(&hb_context, hb_request, &hb_response);
    ASSERT_TRUE(status.ok());
    
    // Verify service is no longer available via get_service (since it's STOPPING)
    grpc::ClientContext get_context2;
    status = get_stub->get_service(&get_context2, get_request, &get_response);
    EXPECT_FALSE(status.ok()) << "STOPPING service should not be returned by get_service";
    
    // But we can still see it via query_services with its actual status
    auto query_stub = swdv::service_discovery::query_services_service::NewStub(discovery_channel_);
    swdv::service_discovery::query_services_request query_request;
    auto* filter = query_request.mutable_filter();
    filter->set_name_pattern("lifecycle_test");
    
    swdv::service_discovery::query_services_response query_response;
    grpc::ClientContext query_context;
    
    status = query_stub->query_services(&query_context, query_request, &query_response);
    ASSERT_TRUE(status.ok());
    ASSERT_EQ(query_response.services_size(), 1);
    EXPECT_EQ(query_response.services(0).status(), swdv::service_discovery::service_status_t::STOPPING)
        << "query_services should return the actual service status";
    
    // Unregister
    auto unregister_stub = swdv::service_discovery::unregister_service_service::NewStub(discovery_channel_);
    
    swdv::service_discovery::unregister_service_request unreg_request;
    unreg_request.set_registration_id(reg_id);
    
    swdv::service_discovery::unregister_service_response unreg_response;
    grpc::ClientContext unreg_context;
    
    status = unregister_stub->unregister_service(&unreg_context, unreg_request, &unreg_response);
    ASSERT_TRUE(status.ok());
    
    // Verify service is gone
    grpc::ClientContext get_context3;
    status = get_stub->get_service(&get_context3, get_request, &get_response);
    EXPECT_FALSE(status.ok());
    
    registered_services_.erase(reg_id);
}

// Test 2: Service Discovery Under Load
TEST_F(ComprehensiveIntegrationTest, DiscoveryUnderLoad) {
    const int num_services = 50;
    std::vector<std::string> service_names;
    
    // Register many services
    for (int i = 0; i < num_services; ++i) {
        std::string name = "load_test_service_" + std::to_string(i);
        service_names.push_back(name);
        
        std::string reg_id = RegisterTestService(name, 40000 + i);
        ASSERT_FALSE(reg_id.empty()) << "Failed to register " << name;
    }
    
    // Query all services multiple times concurrently
    const int num_threads = 10;
    const int queries_per_thread = 20;
    std::atomic<int> success_count{0};
    std::atomic<int> found_services{0};
    
    std::vector<std::thread> threads;
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&]() {
            auto query_stub = swdv::service_discovery::query_services_service::NewStub(discovery_channel_);
            
            for (int q = 0; q < queries_per_thread; ++q) {
                swdv::service_discovery::query_services_request request;
                swdv::service_discovery::query_services_response response;
                grpc::ClientContext context;
                
                auto status = query_stub->query_services(&context, request, &response);
                if (status.ok()) {
                    success_count++;
                    
                    // Count our test services
                    int count = 0;
                    for (const auto& service : response.services()) {
                        if (service.name().find("load_test_service_") == 0) {
                            count++;
                        }
                    }
                    found_services += count;
                }
            }
        });
    }
    
    // Wait for completion
    for (auto& t : threads) {
        t.join();
    }
    
    EXPECT_EQ(success_count.load(), num_threads * queries_per_thread);
    
    // Each query should find all services
    int expected_total = num_threads * queries_per_thread * num_services;
    EXPECT_EQ(found_services.load(), expected_total) 
        << "Should find all services in each query";
}

// Test 3: Service Failover Simulation
TEST_F(ComprehensiveIntegrationTest, ServiceFailover) {
    // Register primary service
    std::string primary_id = RegisterTestService("failover_service", 35001, "1.0.0");
    ASSERT_FALSE(primary_id.empty());
    
    // Register backup service (same name, different version/port)
    std::string backup_id = RegisterTestService("failover_service", 35002, "1.0.1");
    ASSERT_FALSE(backup_id.empty());
    
    // Query should return both
    auto query_stub = swdv::service_discovery::query_services_service::NewStub(discovery_channel_);
    
    swdv::service_discovery::query_services_request query_request;
    auto* filter = query_request.mutable_filter();
    filter->set_name_pattern("failover_service");
    
    swdv::service_discovery::query_services_response query_response;
    grpc::ClientContext query_context;
    
    auto status = query_stub->query_services(&query_context, query_request, &query_response);
    ASSERT_TRUE(status.ok());
    EXPECT_EQ(query_response.services_size(), 2);
    
    // Mark primary as unavailable
    auto heartbeat_stub = swdv::service_discovery::heartbeat_service::NewStub(discovery_channel_);
    
    swdv::service_discovery::heartbeat_request hb_request;
    hb_request.set_registration_id(primary_id);
    hb_request.set_status(swdv::service_discovery::service_status_t::UNAVAILABLE);
    
    swdv::service_discovery::heartbeat_response hb_response;
    grpc::ClientContext hb_context;
    
    status = heartbeat_stub->heartbeat(&hb_context, hb_request, &hb_response);
    ASSERT_TRUE(status.ok());
    
    // Get service should now return the available one
    auto get_stub = swdv::service_discovery::get_service_service::NewStub(discovery_channel_);
    
    swdv::service_discovery::get_service_request get_request;
    get_request.set_service_name("failover_service");
    
    swdv::service_discovery::get_service_response get_response;
    grpc::ClientContext get_context;
    
    status = get_stub->get_service(&get_context, get_request, &get_response);
    ASSERT_TRUE(status.ok());
    EXPECT_EQ(get_response.service_info().version(), "1.0.1");
    EXPECT_EQ(get_response.service_info().endpoint().address(), "localhost:35002");
}

// Test 4: Dispatcher Error Recovery
TEST_F(ComprehensiveIntegrationTest, DispatcherErrorRecovery) {
    auto dispatcher_stub = swdv::ifex_dispatcher::call_method_service::NewStub(dispatcher_channel_);
    
    // Test timeout handling
    swdv::ifex_dispatcher::call_method_request timeout_request;
    auto* timeout_call = timeout_request.mutable_call();
    
    timeout_call->set_service_name("echo_service");
    timeout_call->set_method_name("echo_with_delay");
    
    json timeout_params;
    timeout_params["message"] = "This should timeout";
    timeout_params["delay_ms"] = 2000;  // 2 second delay
    timeout_call->set_parameters(timeout_params.dump());
    timeout_call->set_timeout_ms(500);  // 500ms timeout
    
    swdv::ifex_dispatcher::call_method_response timeout_response;
    grpc::ClientContext timeout_context;
    
    auto start = std::chrono::steady_clock::now();
    auto status = dispatcher_stub->call_method(&timeout_context, timeout_request, &timeout_response);
    auto duration = std::chrono::steady_clock::now() - start;
    
    ASSERT_TRUE(status.ok());
    EXPECT_EQ(timeout_response.result().status(), swdv::ifex_dispatcher::call_status_t::TIMEOUT);
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(duration).count(), 1000);
    
    // Test malformed JSON parameters
    swdv::ifex_dispatcher::call_method_request malformed_request;
    auto* malformed_call = malformed_request.mutable_call();
    
    malformed_call->set_service_name("echo_service");
    malformed_call->set_method_name("echo");
    malformed_call->set_parameters("{invalid json");  // Malformed JSON
    malformed_call->set_timeout_ms(5000);
    
    swdv::ifex_dispatcher::call_method_response malformed_response;
    grpc::ClientContext malformed_context;
    
    status = dispatcher_stub->call_method(&malformed_context, malformed_request, &malformed_response);
    ASSERT_TRUE(status.ok());
    EXPECT_EQ(malformed_response.result().status(), swdv::ifex_dispatcher::call_status_t::INVALID_PARAMETERS);
}

// Test 5: Concurrent Service Registration/Deregistration
TEST_F(ComprehensiveIntegrationTest, ConcurrentRegistrationStress) {
    const int num_threads = 10;
    const int operations_per_thread = 20;
    std::atomic<int> register_success{0};
    std::atomic<int> unregister_success{0};
    std::mutex registration_mutex;
    std::map<std::string, std::string> active_registrations;  // service_name -> registration_id
    
    std::vector<std::thread> threads;
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&, t]() {
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<> dis(0, 1);
            
            for (int op = 0; op < operations_per_thread; ++op) {
                std::string service_name = "stress_test_" + std::to_string(t) + "_" + std::to_string(op);
                
                if (dis(gen) == 0) {
                    // Register operation
                    auto stub = swdv::service_discovery::register_service_service::NewStub(discovery_channel_);
                    
                    swdv::service_discovery::register_service_request request;
                    auto* service_info = request.mutable_service_info();
                    
                    service_info->set_name(service_name);
                    service_info->set_version("1.0.0");
                    service_info->set_description("Stress test service");
                    service_info->set_status(swdv::service_discovery::service_status_t::AVAILABLE);
                    service_info->set_ifex_schema("name: " + service_name + "\nversion: 1.0.0\nmethods: []");
                    
                    auto* endpoint = service_info->mutable_endpoint();
                    endpoint->set_address("localhost:" + std::to_string(50000 + t * 100 + op));
                    endpoint->set_transport(swdv::service_discovery::transport_type_t::GRPC);
                    
                    swdv::service_discovery::register_service_response response;
                    grpc::ClientContext context;
                    
                    if (stub->register_service(&context, request, &response).ok()) {
                        register_success++;
                        
                        std::lock_guard<std::mutex> lock(registration_mutex);
                        active_registrations[service_name] = response.registration_id();
                        registered_services_.insert(response.registration_id());
                    }
                } else {
                    // Unregister operation - pick a random registered service
                    std::lock_guard<std::mutex> lock(registration_mutex);
                    if (!active_registrations.empty()) {
                        auto it = active_registrations.begin();
                        std::advance(it, std::uniform_int_distribution<>(0, active_registrations.size() - 1)(gen));
                        
                        auto stub = swdv::service_discovery::unregister_service_service::NewStub(discovery_channel_);
                        
                        swdv::service_discovery::unregister_service_request request;
                        request.set_registration_id(it->second);
                        
                        swdv::service_discovery::unregister_service_response response;
                        grpc::ClientContext context;
                        
                        if (stub->unregister_service(&context, request, &response).ok()) {
                            unregister_success++;
                            registered_services_.erase(it->second);
                            active_registrations.erase(it);
                        }
                    }
                }
                
                // Small delay to spread out operations
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });
    }
    
    // Wait for completion
    for (auto& t : threads) {
        t.join();
    }
    
    LOG(INFO) << "Register success: " << register_success.load() 
              << ", Unregister success: " << unregister_success.load();
    
    // Verify consistency - query all services
    auto query_stub = swdv::service_discovery::query_services_service::NewStub(discovery_channel_);
    
    swdv::service_discovery::query_services_request query_request;
    swdv::service_discovery::query_services_response query_response;
    grpc::ClientContext query_context;
    
    auto status = query_stub->query_services(&query_context, query_request, &query_response);
    ASSERT_TRUE(status.ok());
    
    // Count stress test services
    int stress_service_count = 0;
    for (const auto& service : query_response.services()) {
        if (service.name().find("stress_test_") == 0) {
            stress_service_count++;
        }
    }
    
    EXPECT_EQ(stress_service_count, active_registrations.size()) 
        << "Service count should match active registrations";
}

// Test 6: Large Payload Handling
TEST_F(ComprehensiveIntegrationTest, LargePayloadHandling) {
    auto dispatcher_stub = swdv::ifex_dispatcher::call_method_service::NewStub(dispatcher_channel_);
    
    // Generate large message
    std::string large_message(1024 * 100, 'A');  // 100KB of 'A's
    
    swdv::ifex_dispatcher::call_method_request request;
    auto* call = request.mutable_call();
    
    call->set_service_name("echo_service");
    call->set_method_name("echo");
    
    json params;
    params["message"] = large_message;
    call->set_parameters(params.dump());
    call->set_timeout_ms(10000);  // 10 second timeout for large payload
    
    swdv::ifex_dispatcher::call_method_response response;
    grpc::ClientContext context;
    
    auto status = dispatcher_stub->call_method(&context, request, &response);
    ASSERT_TRUE(status.ok()) << "RPC failed: " << status.error_message();
    
    const auto& result = response.result();
    EXPECT_EQ(result.status(), swdv::ifex_dispatcher::call_status_t::SUCCESS);
    
    // Verify response contains the large message
    json response_json = json::parse(result.response());
    EXPECT_EQ(response_json["response"].get<std::string>(), large_message);
}

// Test 7: Service Version Compatibility
TEST_F(ComprehensiveIntegrationTest, ServiceVersioning) {
    // Register multiple versions of the same service
    std::string v1_id = RegisterTestService("versioned_service", 36001, "1.0.0");
    std::string v2_id = RegisterTestService("versioned_service", 36002, "2.0.0");
    std::string v3_id = RegisterTestService("versioned_service", 36003, "3.0.0-beta");
    
    ASSERT_FALSE(v1_id.empty());
    ASSERT_FALSE(v2_id.empty());
    ASSERT_FALSE(v3_id.empty());
    
    // Query all versions
    auto query_stub = swdv::service_discovery::query_services_service::NewStub(discovery_channel_);
    
    swdv::service_discovery::query_services_request query_request;
    auto* filter = query_request.mutable_filter();
    filter->set_name_pattern("versioned_service");
    
    swdv::service_discovery::query_services_response query_response;
    grpc::ClientContext query_context;
    
    auto status = query_stub->query_services(&query_context, query_request, &query_response);
    ASSERT_TRUE(status.ok());
    EXPECT_EQ(query_response.services_size(), 3);
    
    // Verify all versions are present
    std::set<std::string> found_versions;
    for (const auto& service : query_response.services()) {
        found_versions.insert(service.version());
    }
    
    EXPECT_EQ(found_versions.size(), 3);
    EXPECT_TRUE(found_versions.count("1.0.0") > 0);
    EXPECT_TRUE(found_versions.count("2.0.0") > 0);
    EXPECT_TRUE(found_versions.count("3.0.0-beta") > 0);
    
    // Get service should return one (implementation specific - usually latest stable)
    auto get_stub = swdv::service_discovery::get_service_service::NewStub(discovery_channel_);
    
    swdv::service_discovery::get_service_request get_request;
    get_request.set_service_name("versioned_service");
    
    swdv::service_discovery::get_service_response get_response;
    grpc::ClientContext get_context;
    
    status = get_stub->get_service(&get_context, get_request, &get_response);
    ASSERT_TRUE(status.ok());
    
    // Should get one of the registered versions
    EXPECT_TRUE(found_versions.count(get_response.service_info().version()) > 0);
}