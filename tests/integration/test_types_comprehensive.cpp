#include "test_fixture.hpp"
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include "ifex-dispatcher-service.grpc.pb.h"

using json = nlohmann::json;

class TestTypesIntegrationTest : public IntegrationTestFixture {
};

// Test all primitive types
TEST_F(TestTypesIntegrationTest, TestAllPrimitives) {
    auto stub = swdv::ifex_dispatcher::call_method_service::NewStub(dispatcher_channel_);
    
    swdv::ifex_dispatcher::call_method_request request;
    auto* call = request.mutable_call();
    
    call->set_service_name("test_types_service");
    call->set_method_name("test_primitives");
    
    // Create test data with all primitive types
    json params;
    json primitives;
    primitives["bool_val"] = true;
    primitives["int32_val"] = -2147483648;  // Min int32
    primitives["int64_val"] = 9223372036854775807LL;  // Max int64
    primitives["uint32_val"] = 4294967295U;  // Max uint32
    primitives["uint64_val"] = 18446744073709551615ULL;  // Max uint64
    primitives["float_val"] = 3.14159f;
    primitives["double_val"] = 2.718281828459045;
    primitives["string_val"] = "Hello, IFEX! 🚀";
    primitives["bytes_val"] = "SGVsbG8gV29ybGQ=";  // Base64 encoded "Hello World"
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
    
    // Parse response
    json response_json = json::parse(result.response());
    EXPECT_TRUE(response_json["validation_passed"].get<bool>());
    
    // Verify echoed values
    const auto& echoed = response_json["echoed"];
    EXPECT_EQ(echoed["bool_val"].get<bool>(), true);
    EXPECT_EQ(echoed["int32_val"].get<int32_t>(), -2147483648);
    
    // Large 64-bit integers may be represented as strings in JSON
    if (echoed["int64_val"].is_string()) {
        EXPECT_EQ(echoed["int64_val"].get<std::string>(), "9223372036854775807");
    } else {
        EXPECT_EQ(echoed["int64_val"].get<int64_t>(), 9223372036854775807LL);
    }
    
    EXPECT_EQ(echoed["uint32_val"].get<uint32_t>(), 4294967295U);
    
    if (echoed["uint64_val"].is_string()) {
        EXPECT_EQ(echoed["uint64_val"].get<std::string>(), "18446744073709551615");
    } else {
        EXPECT_EQ(echoed["uint64_val"].get<uint64_t>(), 18446744073709551615ULL);
    }
    
    EXPECT_FLOAT_EQ(echoed["float_val"].get<float>(), 3.14159f);
    EXPECT_DOUBLE_EQ(echoed["double_val"].get<double>(), 2.718281828459045);
    EXPECT_EQ(echoed["string_val"].get<std::string>(), "Hello, IFEX! 🚀");
}

// Test array handling
TEST_F(TestTypesIntegrationTest, TestArrays) {
    auto stub = swdv::ifex_dispatcher::call_method_service::NewStub(dispatcher_channel_);
    
    swdv::ifex_dispatcher::call_method_request request;
    auto* call = request.mutable_call();
    
    call->set_service_name("test_types_service");
    call->set_method_name("test_arrays");
    
    json params;
    params["int_array"] = {1, 2, 3, 4, 5, -10, -20};
    params["string_array"] = {"Hello", "World", "from", "IFEX"};
    
    json point_array = json::array();
    point_array.push_back({{"x", 1.0}, {"y", 2.0}});
    point_array.push_back({{"x", 3.0}, {"y", 4.0}});
    point_array.push_back({{"x", 5.0}, {"y", 6.0}});
    params["point_array"] = point_array;
    
    call->set_parameters(params.dump());
    call->set_timeout_ms(5000);
    
    swdv::ifex_dispatcher::call_method_response response;
    grpc::ClientContext context;
    
    auto status = stub->call_method(&context, request, &response);
    ASSERT_TRUE(status.ok()) << "RPC call should succeed, got: " << status.error_message();
    
    const auto& result = response.result();
    EXPECT_EQ(result.status(), swdv::ifex_dispatcher::call_status_t::SUCCESS);
    
    json response_json = json::parse(result.response());
    
    // Handle int64 that might be string
    int64_t int_sum;
    if (response_json["int_sum"].is_string()) {
        int_sum = std::stoll(response_json["int_sum"].get<std::string>());
    } else {
        int_sum = response_json["int_sum"].get<int64_t>();
    }
    EXPECT_EQ(int_sum, -15);  // 1+2+3+4+5-10-20 = -15
    
    EXPECT_EQ(response_json["concatenated_strings"].get<std::string>(), "Hello World from IFEX");
    
    const auto& centroid = response_json["centroid"];
    EXPECT_DOUBLE_EQ(centroid["x"].get<double>(), 3.0);  // (1+3+5)/3 = 3
    EXPECT_DOUBLE_EQ(centroid["y"].get<double>(), 4.0);  // (2+4+6)/3 = 4
}

// Test enum handling
TEST_F(TestTypesIntegrationTest, TestEnums) {
    auto stub = swdv::ifex_dispatcher::call_method_service::NewStub(dispatcher_channel_);
    
    swdv::ifex_dispatcher::call_method_request request;
    auto* call = request.mutable_call();
    
    call->set_service_name("test_types_service");
    call->set_method_name("test_enums");
    
    json params;
    params["color"] = 2;  // BLUE
    params["status"] = 1;  // STATUS_ERROR
    
    call->set_parameters(params.dump());
    call->set_timeout_ms(5000);
    
    swdv::ifex_dispatcher::call_method_response response;
    grpc::ClientContext context;
    
    auto status = stub->call_method(&context, request, &response);
    ASSERT_TRUE(status.ok()) << "RPC call should succeed, got: " << status.error_message();
    
    const auto& result = response.result();
    EXPECT_EQ(result.status(), swdv::ifex_dispatcher::call_status_t::SUCCESS);
    
    json response_json = json::parse(result.response());
    EXPECT_EQ(response_json["color_name"].get<std::string>(), "BLUE");
    EXPECT_EQ(response_json["status_message"].get<std::string>(), "Generic error occurred");
    
    EXPECT_EQ(response_json["next_color"].get<int>(), 3);  // YELLOW
}

// Test nested structs
TEST_F(TestTypesIntegrationTest, TestNestedStructs) {
    auto stub = swdv::ifex_dispatcher::call_method_service::NewStub(dispatcher_channel_);
    
    swdv::ifex_dispatcher::call_method_request request;
    auto* call = request.mutable_call();
    
    call->set_service_name("test_types_service");
    call->set_method_name("test_nested_structs");
    
    json params;
    json nested;
    nested["id"] = "test-123";
    
    // Add primitives
    json primitives;
    primitives["bool_val"] = false;
    primitives["int32_val"] = 42;
    primitives["int64_val"] = 1234567890;
    primitives["uint32_val"] = 100;
    primitives["uint64_val"] = 200;
    primitives["float_val"] = 1.5f;
    primitives["double_val"] = 2.5;
    primitives["string_val"] = "nested test";
    primitives["bytes_val"] = "dGVzdA==";  // "test" in base64
    nested["primitives"] = primitives;
    
    // Add arrays
    json points = json::array();
    points.push_back({{"x", 1.0}, {"y", 2.0}, {"z", 3.0}});
    points.push_back({{"x", 4.0}, {"y", 5.0}, {"z", 6.0}});
    nested["points"] = points;
    
    json colors = {0, 1, 2};  // RED, GREEN, BLUE
    nested["colors"] = colors;
    
    nested["metadata"] = R"({"version": "1.0", "author": "test"})";
    
    params["nested"] = nested;
    
    call->set_parameters(params.dump());
    call->set_timeout_ms(5000);
    
    swdv::ifex_dispatcher::call_method_response response;
    grpc::ClientContext context;
    
    auto status = stub->call_method(&context, request, &response);
    ASSERT_TRUE(status.ok()) << "RPC call should succeed, got: " << status.error_message();
    
    const auto& result = response.result();
    EXPECT_EQ(result.status(), swdv::ifex_dispatcher::call_status_t::SUCCESS);
    
    json response_json = json::parse(result.response());
    EXPECT_EQ(response_json["point_count"].get<uint32_t>(), 2);
    EXPECT_EQ(response_json["color_count"].get<uint32_t>(), 3);
    EXPECT_TRUE(response_json["summary"].get<std::string>().find("test-123") != std::string::npos);
}

// Test optional fields
TEST_F(TestTypesIntegrationTest, TestOptionalFields) {
    auto stub = swdv::ifex_dispatcher::call_method_service::NewStub(dispatcher_channel_);
    
    // Test with all optional fields present
    {
        swdv::ifex_dispatcher::call_method_request request;
        auto* call = request.mutable_call();
        
        call->set_service_name("test_types_service");
        call->set_method_name("test_optional_fields");
        
        json params;
        params["required_string"] = "This is required";
        params["optional_int"] = 42;
        params["optional_struct"] = {{"x", 10.5}, {"y", 20.5}};
        params["optional_array"] = {"one", "two", "three"};
        
        call->set_parameters(params.dump());
        call->set_timeout_ms(5000);
        
        swdv::ifex_dispatcher::call_method_response response;
        grpc::ClientContext context;
        
        auto status = stub->call_method(&context, request, &response);
        ASSERT_TRUE(status.ok());
        
        const auto& result = response.result();
        EXPECT_EQ(result.status(), swdv::ifex_dispatcher::call_status_t::SUCCESS);
        
        json response_json = json::parse(result.response());
        EXPECT_EQ(response_json["received_count"].get<uint32_t>(), 4);  // All fields present
    }
    
    // Test with only required field
    {
        swdv::ifex_dispatcher::call_method_request request;
        auto* call = request.mutable_call();
        
        call->set_service_name("test_types_service");
        call->set_method_name("test_optional_fields");
        
        json params;
        params["required_string"] = "Only required field";
        
        call->set_parameters(params.dump());
        call->set_timeout_ms(5000);
        
        swdv::ifex_dispatcher::call_method_response response;
        grpc::ClientContext context;
        
        auto status = stub->call_method(&context, request, &response);
        ASSERT_TRUE(status.ok());
        
        const auto& result = response.result();
        EXPECT_EQ(result.status(), swdv::ifex_dispatcher::call_status_t::SUCCESS);
        
        json response_json = json::parse(result.response());
        EXPECT_EQ(response_json["received_count"].get<uint32_t>(), 1);  // Only required field
    }
}

// Test complex sensor data streaming
TEST_F(TestTypesIntegrationTest, TestSensorDataStream) {
    auto stub = swdv::ifex_dispatcher::call_method_service::NewStub(dispatcher_channel_);
    
    swdv::ifex_dispatcher::call_method_request request;
    auto* call = request.mutable_call();
    
    call->set_service_name("test_types_service");
    call->set_method_name("test_sensor_data_stream");
    
    json params;
    json sensor_readings = json::array();
    
    // Add various sensor readings
    sensor_readings.push_back({
        {"sensor_id", 1001},
        {"timestamp", 1640000000000LL},
        {"temperature", 25.5f},
        {"pressure", 101.3f},
        {"humidity", 60.0f},
        {"location", {{"x", 10.0}, {"y", 20.0}, {"z", 5.0}}},
        {"status", 0}  // STATUS_OK
    });
    
    sensor_readings.push_back({
        {"sensor_id", 1002},
        {"timestamp", 1640000001000LL},
        {"temperature", 26.0f},
        {"pressure", 101.2f},
        {"humidity", 65.0f},
        {"location", {{"x", 15.0}, {"y", 25.0}, {"z", 5.0}}},
        {"status", 1},  // STATUS_ERROR
        {"error_message", "Sensor calibration needed"}
    });
    
    sensor_readings.push_back({
        {"sensor_id", 1003},
        {"timestamp", 1640000002000LL},
        {"temperature", 24.5f},
        {"pressure", 101.4f},
        {"humidity", 55.0f},
        {"location", {{"x", 20.0}, {"y", 30.0}, {"z", 5.0}}},
        {"status", 2}  // STATUS_PENDING
    });
    
    params["sensor_readings"] = sensor_readings;
    params["filter_by_status"] = 1;  // Filter for STATUS_ERROR (non-zero to enable filtering)
    
    call->set_parameters(params.dump());
    call->set_timeout_ms(5000);
    
    swdv::ifex_dispatcher::call_method_response response;
    grpc::ClientContext context;
    
    auto status = stub->call_method(&context, request, &response);
    ASSERT_TRUE(status.ok()) << "RPC call should succeed, got: " << status.error_message();
    
    const auto& result = response.result();
    EXPECT_EQ(result.status(), swdv::ifex_dispatcher::call_status_t::SUCCESS);
    
    json response_json = json::parse(result.response());
    EXPECT_EQ(response_json["processed_count"].get<uint32_t>(), 1);  // Only STATUS_ERROR
    EXPECT_FLOAT_EQ(response_json["average_temperature"].get<float>(), 26.0f);  // Only sensor 1002
    EXPECT_EQ(response_json["error_sensors"].size(), 1);
    EXPECT_EQ(response_json["error_sensors"][0].get<uint32_t>(), 1002);
    
    // Verify summary report
    ASSERT_TRUE(response_json.contains("summary_report"));
    json summary = json::parse(response_json["summary_report"].get<std::string>());
    EXPECT_EQ(summary["total_readings"].get<int>(), 3);
    EXPECT_TRUE(summary["filtered"].get<bool>());
}

// Test error handling with invalid service
TEST_F(TestTypesIntegrationTest, TestInvalidService) {
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
    ASSERT_TRUE(status.ok()) << "RPC call should succeed even for non-existent service";
    
    const auto& result = response.result();
    EXPECT_EQ(result.status(), swdv::ifex_dispatcher::call_status_t::SERVICE_UNAVAILABLE);
    EXPECT_FALSE(result.error_message().empty());
}

// Test error handling with invalid method
TEST_F(TestTypesIntegrationTest, TestInvalidMethod) {
    auto stub = swdv::ifex_dispatcher::call_method_service::NewStub(dispatcher_channel_);
    
    swdv::ifex_dispatcher::call_method_request request;
    auto* call = request.mutable_call();
    
    call->set_service_name("test_types_service");
    call->set_method_name("non_existent_method");
    call->set_parameters("{}");
    call->set_timeout_ms(5000);
    
    swdv::ifex_dispatcher::call_method_response response;
    grpc::ClientContext context;
    
    auto status = stub->call_method(&context, request, &response);
    ASSERT_TRUE(status.ok());
    
    const auto& result = response.result();
    EXPECT_EQ(result.status(), swdv::ifex_dispatcher::call_status_t::METHOD_NOT_FOUND);
    EXPECT_FALSE(result.error_message().empty());
}

// Test timeout handling
TEST_F(TestTypesIntegrationTest, TestTimeout) {
    auto stub = swdv::ifex_dispatcher::call_method_service::NewStub(dispatcher_channel_);
    
    swdv::ifex_dispatcher::call_method_request request;
    auto* call = request.mutable_call();
    
    call->set_service_name("test_types_service");
    call->set_method_name("test_arrays");
    
    // Create large arrays to potentially slow down processing
    json params;
    json large_array;
    for (int i = 0; i < 10000; i++) {
        large_array.push_back(i);
    }
    params["int_array"] = large_array;
    params["string_array"] = {"test"};
    params["point_array"] = {{{"x", 0}, {"y", 0}}};
    
    call->set_parameters(params.dump());
    call->set_timeout_ms(1);  // Very short timeout
    
    swdv::ifex_dispatcher::call_method_response response;
    grpc::ClientContext context;
    
    auto status = stub->call_method(&context, request, &response);
    ASSERT_TRUE(status.ok());
    
    // The call might succeed if it's fast enough, or timeout
    const auto& result = response.result();
    if (result.status() != swdv::ifex_dispatcher::call_status_t::SUCCESS) {
        EXPECT_EQ(result.status(), swdv::ifex_dispatcher::call_status_t::TIMEOUT);
    }
}