#include <gtest/gtest.h>
#include <glog/logging.h>
#include "service-discovery-service.grpc.pb.h"
#include <grpcpp/grpcpp.h>
#include <thread>
#include <chrono>
#include "test_fixture.hpp"

class DiscoveryIntegrationTest : public IntegrationTestFixture {
protected:
    // Use the test fixture's channels and addresses
};

TEST_F(DiscoveryIntegrationTest, RegisterAndGetService) {
    // Create stub for register service
    auto register_stub = swdv::service_discovery::register_service_service::NewStub(discovery_channel_);
    
    // Register a test service
    swdv::service_discovery::register_service_request register_request;
    auto* service_info = register_request.mutable_service_info();
    
    service_info->set_name("test_service");
    service_info->set_version("1.0.0");
    service_info->set_description("Test service for integration testing");
    service_info->set_status(swdv::service_discovery::service_status_t::AVAILABLE);
    service_info->set_ifex_schema(R"(
name: test_service
version: 1.0.0
description: Test service
methods:
  test_method:
    description: Test method
    input: []
    output: []
)");
    
    auto* endpoint = service_info->mutable_endpoint();
    endpoint->set_address("localhost:12345");
    endpoint->set_transport(swdv::service_discovery::transport_type_t::GRPC);
    
    swdv::service_discovery::register_service_response register_response;
    grpc::ClientContext register_context;
    
    auto status = register_stub->register_service(&register_context, register_request, &register_response);
    ASSERT_TRUE(status.ok()) << "Failed to register service: " << status.error_message();
    ASSERT_FALSE(register_response.registration_id().empty());
    
    std::string registration_id = register_response.registration_id();
    LOG(INFO) << "Registered service with ID: " << registration_id;
    
    // Now try to get the service
    auto get_stub = swdv::service_discovery::get_service_service::NewStub(discovery_channel_);
    
    swdv::service_discovery::get_service_request get_request;
    get_request.set_service_name("test_service");
    
    swdv::service_discovery::get_service_response get_response;
    grpc::ClientContext get_context;
    
    status = get_stub->get_service(&get_context, get_request, &get_response);
    ASSERT_TRUE(status.ok()) << "Failed to get service: " << status.error_message();
    
    const auto& retrieved_info = get_response.service_info();
    EXPECT_EQ(retrieved_info.name(), "test_service");
    EXPECT_EQ(retrieved_info.version(), "1.0.0");
    EXPECT_EQ(retrieved_info.endpoint().address(), "localhost:12345");
    
    // Clean up - unregister the service
    auto unregister_stub = swdv::service_discovery::unregister_service_service::NewStub(discovery_channel_);
    
    swdv::service_discovery::unregister_service_request unregister_request;
    unregister_request.set_registration_id(registration_id);
    
    swdv::service_discovery::unregister_service_response unregister_response;
    grpc::ClientContext unregister_context;
    
    status = unregister_stub->unregister_service(&unregister_context, unregister_request, &unregister_response);
    EXPECT_TRUE(status.ok()) << "Failed to unregister service: " << status.error_message();
}

TEST_F(DiscoveryIntegrationTest, QueryServices) {
    // Register multiple test services
    auto register_stub = swdv::service_discovery::register_service_service::NewStub(discovery_channel_);
    std::vector<std::string> registration_ids;
    
    // Register service 1
    {
        swdv::service_discovery::register_service_request request;
        auto* service_info = request.mutable_service_info();
        service_info->set_name("query_test_1");
        service_info->set_version("1.0.0");
        service_info->set_description("First query test service");
        service_info->set_status(swdv::service_discovery::service_status_t::AVAILABLE);
        service_info->set_ifex_schema("name: query_test_1\nversion: 1.0.0\nmethods: []");
        
        auto* endpoint = service_info->mutable_endpoint();
        endpoint->set_address("localhost:20001");
        endpoint->set_transport(swdv::service_discovery::transport_type_t::GRPC);
        
        swdv::service_discovery::register_service_response response;
        grpc::ClientContext context;
        
        auto status = register_stub->register_service(&context, request, &response);
        ASSERT_TRUE(status.ok());
        registration_ids.push_back(response.registration_id());
    }
    
    // Register service 2
    {
        swdv::service_discovery::register_service_request request;
        auto* service_info = request.mutable_service_info();
        service_info->set_name("query_test_2");
        service_info->set_version("2.0.0");
        service_info->set_description("Second query test service");
        service_info->set_status(swdv::service_discovery::service_status_t::AVAILABLE);
        service_info->set_ifex_schema("name: query_test_2\nversion: 2.0.0\nmethods: []");
        
        auto* endpoint = service_info->mutable_endpoint();
        endpoint->set_address("localhost:20002");
        endpoint->set_transport(swdv::service_discovery::transport_type_t::GRPC);
        
        swdv::service_discovery::register_service_response response;
        grpc::ClientContext context;
        
        auto status = register_stub->register_service(&context, request, &response);
        ASSERT_TRUE(status.ok());
        registration_ids.push_back(response.registration_id());
    }
    
    // Query all services
    auto query_stub = swdv::service_discovery::query_services_service::NewStub(discovery_channel_);
    
    swdv::service_discovery::query_services_request query_request;
    // Empty filter to get all services
    
    swdv::service_discovery::query_services_response query_response;
    grpc::ClientContext query_context;
    
    auto status = query_stub->query_services(&query_context, query_request, &query_response);
    ASSERT_TRUE(status.ok()) << "Failed to query services: " << status.error_message();
    
    // Should find at least our two test services
    EXPECT_GE(query_response.services_size(), 2);
    
    int found_count = 0;
    for (const auto& service : query_response.services()) {
        if (service.name() == "query_test_1" || service.name() == "query_test_2") {
            found_count++;
        }
    }
    EXPECT_EQ(found_count, 2) << "Should find both test services";
    
    // Clean up
    auto unregister_stub = swdv::service_discovery::unregister_service_service::NewStub(discovery_channel_);
    for (const auto& id : registration_ids) {
        swdv::service_discovery::unregister_service_request request;
        request.set_registration_id(id);
        
        swdv::service_discovery::unregister_service_response response;
        grpc::ClientContext context;
        
        unregister_stub->unregister_service(&context, request, &response);
    }
}

