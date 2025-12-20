#pragma once

#include "test-types-service.grpc.pb.h"
#include <grpcpp/grpcpp.h>
#include <glog/logging.h>
#include <nlohmann/json.hpp>
#include <numeric>
#include <cmath>

namespace test_types {

using json = nlohmann::json;

class TestTypesServer final {
public:
    TestTypesServer();
    void Start(const std::string& address);
    void Shutdown();
    
private:
    // Service implementations for each method
    class TestPrimitivesService final : public swdv::test_types_service::test_primitives_service::Service {
        grpc::Status test_primitives(grpc::ServerContext* context,
                                   const swdv::test_types_service::test_primitives_request* request,
                                   swdv::test_types_service::test_primitives_response* response) override;
    };
    
    class TestArraysService final : public swdv::test_types_service::test_arrays_service::Service {
        grpc::Status test_arrays(grpc::ServerContext* context,
                               const swdv::test_types_service::test_arrays_request* request,
                               swdv::test_types_service::test_arrays_response* response) override;
    };
    
    class TestEnumsService final : public swdv::test_types_service::test_enums_service::Service {
        grpc::Status test_enums(grpc::ServerContext* context,
                              const swdv::test_types_service::test_enums_request* request,
                              swdv::test_types_service::test_enums_response* response) override;
    };
    
    class TestNestedStructsService final : public swdv::test_types_service::test_nested_structs_service::Service {
        grpc::Status test_nested_structs(grpc::ServerContext* context,
                                       const swdv::test_types_service::test_nested_structs_request* request,
                                       swdv::test_types_service::test_nested_structs_response* response) override;
    };
    
    class TestOptionalFieldsService final : public swdv::test_types_service::test_optional_fields_service::Service {
        grpc::Status test_optional_fields(grpc::ServerContext* context,
                                        const swdv::test_types_service::test_optional_fields_request* request,
                                        swdv::test_types_service::test_optional_fields_response* response) override;
    };
    
    class TestSensorDataStreamService final : public swdv::test_types_service::test_sensor_data_stream_service::Service {
        grpc::Status test_sensor_data_stream(grpc::ServerContext* context,
                                           const swdv::test_types_service::test_sensor_data_stream_request* request,
                                           swdv::test_types_service::test_sensor_data_stream_response* response) override;
    };
    
    std::unique_ptr<grpc::Server> server_;
    TestPrimitivesService test_primitives_service_;
    TestArraysService test_arrays_service_;
    TestEnumsService test_enums_service_;
    TestNestedStructsService test_nested_structs_service_;
    TestOptionalFieldsService test_optional_fields_service_;
    TestSensorDataStreamService test_sensor_data_stream_service_;
};

} // namespace test_types