#include <gtest/gtest.h>
#include <glog/logging.h>
#include "ifex-dispatcher-service.grpc.pb.h"
#include "service-discovery-service.grpc.pb.h"
#include <grpcpp/grpcpp.h>
#include <nlohmann/json.hpp>
#include <thread>
#include <chrono>
#include "test_fixture.hpp"

using json = nlohmann::json;

class DispatcherIntegrationTest : public IntegrationTestFixture {
protected:
    void TearDown() override {
        // Clean up any registered services
        if (!registration_id_.empty()) {
            auto stub = swdv::service_discovery::unregister_service_service::NewStub(discovery_channel_);
            swdv::service_discovery::unregister_service_request request;
            request.set_registration_id(registration_id_);
            
            swdv::service_discovery::unregister_service_response response;
            grpc::ClientContext context;
            stub->unregister_service(&context, request, &response);
        }
        
        IntegrationTestFixture::TearDown();
    }
    
    std::string registration_id_;
};

TEST_F(DispatcherIntegrationTest, CallExistingService) {
    // The echo service should already be registered if the system is running
    auto stub = swdv::ifex_dispatcher::call_method_service::NewStub(dispatcher_channel_);
    
    swdv::ifex_dispatcher::call_method_request request;
    auto* call = request.mutable_call();
    
    call->set_service_name("echo_service");
    call->set_method_name("echo");
    
    json params;
    params["message"] = "Hello from integration test!";
    call->set_parameters(params.dump());
    call->set_timeout_ms(5000);
    
    swdv::ifex_dispatcher::call_method_response response;
    grpc::ClientContext context;
    
    auto status = stub->call_method(&context, request, &response);
    ASSERT_TRUE(status.ok()) << "RPC failed: " << status.error_message();
    
    const auto& result = response.result();
    EXPECT_EQ(result.status(), swdv::ifex_dispatcher::call_status_t::SUCCESS);
    EXPECT_FALSE(result.response().empty());
    
    // Parse the response
    json response_json = json::parse(result.response());
    EXPECT_EQ(response_json["response"], "Hello from integration test!");
}

TEST_F(DispatcherIntegrationTest, CallWithDelay) {
    auto stub = swdv::ifex_dispatcher::call_method_service::NewStub(dispatcher_channel_);
    
    swdv::ifex_dispatcher::call_method_request request;
    auto* call = request.mutable_call();
    
    call->set_service_name("echo_service");
    call->set_method_name("echo_with_delay");
    
    json params;
    params["message"] = "Delayed message";
    params["delay_ms"] = 100;  // 100ms delay
    call->set_parameters(params.dump());
    call->set_timeout_ms(5000);
    
    auto start = std::chrono::high_resolution_clock::now();
    
    swdv::ifex_dispatcher::call_method_response response;
    grpc::ClientContext context;
    
    auto status = stub->call_method(&context, request, &response);
    ASSERT_TRUE(status.ok()) << "RPC failed: " << status.error_message();
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    
    const auto& result = response.result();
    EXPECT_EQ(result.status(), swdv::ifex_dispatcher::call_status_t::SUCCESS);
    
    // Parse response
    json response_json = json::parse(result.response());
    EXPECT_EQ(response_json["response"], "Delayed message");
    EXPECT_GE(response_json["actual_delay_ms"].get<int>(), 100);
    
    // Total duration should be at least the delay
    EXPECT_GE(duration, 100);
}

TEST_F(DispatcherIntegrationTest, CallNonExistentService) {
    auto stub = swdv::ifex_dispatcher::call_method_service::NewStub(dispatcher_channel_);
    
    swdv::ifex_dispatcher::call_method_request request;
    auto* call = request.mutable_call();
    
    call->set_service_name("non_existent_service");
    call->set_method_name("some_method");
    call->set_parameters("{}");
    call->set_timeout_ms(5000);
    
    swdv::ifex_dispatcher::call_method_response response;
    grpc::ClientContext context;
    
    auto status = stub->call_method(&context, request, &response);
    ASSERT_TRUE(status.ok()) << "RPC itself should succeed";
    
    const auto& result = response.result();
    EXPECT_EQ(result.status(), swdv::ifex_dispatcher::call_status_t::SERVICE_UNAVAILABLE);
    EXPECT_FALSE(result.error_message().empty());
}

TEST_F(DispatcherIntegrationTest, CallWithInvalidParameters) {
    auto stub = swdv::ifex_dispatcher::call_method_service::NewStub(dispatcher_channel_);
    
    // Test calling echo service with missing required parameter
    swdv::ifex_dispatcher::call_method_request request;
    auto* call = request.mutable_call();
    
    call->set_service_name("echo_service");
    call->set_method_name("echo");
    call->set_parameters("{}");  // Missing required 'message' field
    call->set_timeout_ms(5000);
    
    swdv::ifex_dispatcher::call_method_response response;
    grpc::ClientContext context;
    
    auto status = stub->call_method(&context, request, &response);
    ASSERT_TRUE(status.ok()) << "RPC call should succeed";
    
    const auto& result = response.result();
    EXPECT_EQ(result.status(), swdv::ifex_dispatcher::call_status_t::INVALID_PARAMETERS);
    EXPECT_FALSE(result.error_message().empty());
    EXPECT_TRUE(result.response().empty());
}

// get_example_parameters is not implemented in our proto yet
// TEST_F(DispatcherIntegrationTest, GetExampleParameters) {
// }

TEST_F(DispatcherIntegrationTest, CallTestTypesService) {
    auto stub = swdv::ifex_dispatcher::call_method_service::NewStub(dispatcher_channel_);

    swdv::ifex_dispatcher::call_method_request request;
    auto* call = request.mutable_call();

    call->set_service_name("test_types_service");
    call->set_method_name("test_primitives");

    json params;
    json primitives;
    primitives["bool_val"] = true;
    primitives["int32_val"] = 42;
    primitives["int64_val"] = 1234567890LL;
    primitives["uint32_val"] = 100U;
    primitives["uint64_val"] = 200ULL;
    primitives["float_val"] = 3.14f;
    primitives["double_val"] = 2.718;
    primitives["string_val"] = "test";
    primitives["bytes_val"] = "dGVzdA==";
    params["primitives"] = primitives;

    call->set_parameters(params.dump());
    call->set_timeout_ms(5000);

    swdv::ifex_dispatcher::call_method_response response;
    grpc::ClientContext context;

    auto status = stub->call_method(&context, request, &response);
    ASSERT_TRUE(status.ok()) << "RPC call should succeed, got: " << status.error_message();

    const auto& result = response.result();
    EXPECT_EQ(result.status(), swdv::ifex_dispatcher::call_status_t::SUCCESS)
        << "Call should succeed, got error: " << result.error_message();
    EXPECT_FALSE(result.response().empty()) << "Should return response data";
}

TEST_F(DispatcherIntegrationTest, ConcurrentCalls) {
    auto stub = swdv::ifex_dispatcher::call_method_service::NewStub(dispatcher_channel_);
    
    const int num_threads = 5;
    const int calls_per_thread = 10;
    std::vector<std::thread> threads;
    std::atomic<int> success_count{0};
    std::atomic<int> total_calls{0};
    
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < calls_per_thread; ++i) {
                swdv::ifex_dispatcher::call_method_request request;
                auto* call = request.mutable_call();
                
                call->set_service_name("echo_service");
                call->set_method_name("echo");
                
                json params;
                params["message"] = "Thread " + std::to_string(t) + " call " + std::to_string(i);
                call->set_parameters(params.dump());
                call->set_timeout_ms(5000);
                
                swdv::ifex_dispatcher::call_method_response response;
                grpc::ClientContext context;
                
                auto status = stub->call_method(&context, request, &response);
                total_calls++;
                
                if (status.ok() && response.result().status() == swdv::ifex_dispatcher::call_status_t::SUCCESS) {
                    success_count++;
                }
            }
        });
    }
    
    // Wait for all threads to complete
    for (auto& t : threads) {
        t.join();
    }
    
    EXPECT_EQ(total_calls.load(), num_threads * calls_per_thread);
    EXPECT_EQ(success_count.load(), total_calls.load()) 
        << "All concurrent calls should succeed";
}