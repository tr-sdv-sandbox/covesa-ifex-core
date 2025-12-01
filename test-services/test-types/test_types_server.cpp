#include "test_types_server.hpp"
#include <sstream>
#include <iomanip>

namespace test_types {

TestTypesServer::TestTypesServer() {
    LOG(INFO) << "Initializing Test Types Server";
}

void TestTypesServer::Start(const std::string& address) {
    grpc::ServerBuilder builder;
    builder.AddListeningPort(address, grpc::InsecureServerCredentials());
    
    // Register all services
    builder.RegisterService(&test_primitives_service_);
    builder.RegisterService(&test_arrays_service_);
    builder.RegisterService(&test_enums_service_);
    builder.RegisterService(&test_nested_structs_service_);
    builder.RegisterService(&test_optional_fields_service_);
    builder.RegisterService(&test_sensor_data_stream_service_);
    
    server_ = builder.BuildAndStart();
    LOG(INFO) << "Test Types server listening on " << address;
}

void TestTypesServer::Shutdown() {
    if (server_) {
        server_->Shutdown();
        LOG(INFO) << "Test Types server shut down";
    }
}

// TestPrimitivesService implementation
grpc::Status TestTypesServer::TestPrimitivesService::test_primitives(
    grpc::ServerContext* context,
    const swdv::test_types_service::test_primitives_request* request,
    swdv::test_types_service::test_primitives_response* response) {
    
    LOG(INFO) << "test_primitives called";
    
    // Echo back all primitive values
    *response->mutable_echoed() = request->primitives();
    
    // Validate the values
    bool validation_passed = true;
    const auto& p = request->primitives();
    
    // Perform some basic validation
    if (p.string_val().empty()) validation_passed = false;
    if (p.int32_val() == 0 && p.int64_val() == 0) validation_passed = false; // At least one should be non-zero
    
    response->set_validation_passed(validation_passed);
    
    return grpc::Status::OK;
}

// TestArraysService implementation
grpc::Status TestTypesServer::TestArraysService::test_arrays(
    grpc::ServerContext* context,
    const swdv::test_types_service::test_arrays_request* request,
    swdv::test_types_service::test_arrays_response* response) {
    
    LOG(INFO) << "test_arrays called";
    LOG(INFO) << "Received " << request->int_array_size() << " integers";
    LOG(INFO) << "Received " << request->string_array_size() << " strings";
    LOG(INFO) << "Received " << request->point_array_size() << " points";
    
    // Sum integers
    int64_t sum = 0;
    for (int i = 0; i < request->int_array_size(); i++) {
        sum += request->int_array(i);
    }
    response->set_int_sum(sum);
    
    // Concatenate strings
    std::stringstream ss;
    for (int i = 0; i < request->string_array_size(); i++) {
        if (i > 0) ss << " ";
        ss << request->string_array(i);
    }
    response->set_concatenated_strings(ss.str());
    
    // Calculate centroid of points
    double x_sum = 0, y_sum = 0;
    int count = request->point_array_size();
    if (count > 0) {
        for (int i = 0; i < count; i++) {
            x_sum += request->point_array(i).x();
            y_sum += request->point_array(i).y();
        }
        response->mutable_centroid()->set_x(x_sum / count);
        response->mutable_centroid()->set_y(y_sum / count);
    } else {
        response->mutable_centroid()->set_x(0);
        response->mutable_centroid()->set_y(0);
    }
    
    return grpc::Status::OK;
}

// TestEnumsService implementation
grpc::Status TestTypesServer::TestEnumsService::test_enums(
    grpc::ServerContext* context,
    const swdv::test_types_service::test_enums_request* request,
    swdv::test_types_service::test_enums_response* response) {
    
    LOG(INFO) << "test_enums called";
    LOG(INFO) << "Color: " << request->color();
    LOG(INFO) << "Status: " << request->status();
    
    // Convert color to string
    switch (request->color()) {
        case swdv::test_types_service::RED:
            response->set_color_name("RED");
            response->set_next_color(swdv::test_types_service::GREEN);
            break;
        case swdv::test_types_service::GREEN:
            response->set_color_name("GREEN");
            response->set_next_color(swdv::test_types_service::BLUE);
            break;
        case swdv::test_types_service::BLUE:
            response->set_color_name("BLUE");
            response->set_next_color(swdv::test_types_service::YELLOW);
            break;
        case swdv::test_types_service::YELLOW:
            response->set_color_name("YELLOW");
            response->set_next_color(swdv::test_types_service::RED);
            break;
        default:
            response->set_color_name("UNKNOWN");
            response->set_next_color(swdv::test_types_service::RED);
    }
    
    // Convert status to message
    switch (request->status()) {
        case swdv::test_types_service::STATUS_OK:
            response->set_status_message("Operation successful");
            break;
        case swdv::test_types_service::STATUS_ERROR:
            response->set_status_message("Generic error occurred");
            break;
        case swdv::test_types_service::STATUS_PENDING:
            response->set_status_message("Operation is pending");
            break;
        case swdv::test_types_service::STATUS_CANCELLED:
            response->set_status_message("Operation was cancelled");
            break;
        default:
            response->set_status_message("Unknown status");
    }
    
    return grpc::Status::OK;
}

// TestNestedStructsService implementation
grpc::Status TestTypesServer::TestNestedStructsService::test_nested_structs(
    grpc::ServerContext* context,
    const swdv::test_types_service::test_nested_structs_request* request,
    swdv::test_types_service::test_nested_structs_response* response) {
    
    LOG(INFO) << "test_nested_structs called";
    
    const auto& nested = request->nested();
    
    // Create summary
    std::stringstream ss;
    ss << "ID: " << nested.id() << ", ";
    ss << "Points: " << nested.points_size() << ", ";
    ss << "Colors: " << nested.colors_size() << ", ";
    ss << "Has metadata: " << (!nested.metadata().empty() ? "yes" : "no");
    
    response->set_summary(ss.str());
    response->set_point_count(nested.points_size());
    response->set_color_count(nested.colors_size());
    
    return grpc::Status::OK;
}

// TestOptionalFieldsService implementation
grpc::Status TestTypesServer::TestOptionalFieldsService::test_optional_fields(
    grpc::ServerContext* context,
    const swdv::test_types_service::test_optional_fields_request* request,
    swdv::test_types_service::test_optional_fields_response* response) {
    
    LOG(INFO) << "test_optional_fields called";
    
    uint32_t received_count = 1; // required_string is always present
    std::stringstream ss;
    ss << "Required string: '" << request->required_string() << "'";
    
    // In proto3, optional fields always have a value (default if not set)
    // We'll check for non-default values
    if (request->optional_int() != 0) {
        received_count++;
        ss << ", Optional int: " << request->optional_int();
    }
    
    // For message types, check if they have been set
    if (request->optional_struct().x() != 0 || request->optional_struct().y() != 0) {
        received_count++;
        ss << ", Optional struct: (" << request->optional_struct().x() 
           << ", " << request->optional_struct().y() << ")";
    }
    
    if (request->optional_array_size() > 0) {
        received_count++;
        ss << ", Optional array size: " << request->optional_array_size();
    }
    
    response->set_received_count(received_count);
    response->set_summary(ss.str());
    
    return grpc::Status::OK;
}

// TestSensorDataStreamService implementation
grpc::Status TestTypesServer::TestSensorDataStreamService::test_sensor_data_stream(
    grpc::ServerContext* context,
    const swdv::test_types_service::test_sensor_data_stream_request* request,
    swdv::test_types_service::test_sensor_data_stream_response* response) {
    
    LOG(INFO) << "test_sensor_data_stream called";
    LOG(INFO) << "Received " << request->sensor_readings_size() << " sensor readings";
    
    uint32_t processed_count = 0;
    float temperature_sum = 0;
    std::vector<uint32_t> error_sensors;
    
    json summary;
    summary["total_readings"] = request->sensor_readings_size();
    // Check if filter is set (non-default value)
    bool has_filter = (request->filter_by_status() != swdv::test_types_service::status_t(0));
    summary["filtered"] = has_filter;
    
    for (int i = 0; i < request->sensor_readings_size(); i++) {
        const auto& reading = request->sensor_readings(i);
        
        // Apply filter if specified (check for non-default value)
        if (has_filter && reading.status() != request->filter_by_status()) {
            continue;
        }
        
        processed_count++;
        temperature_sum += reading.temperature();
        
        if (reading.status() == swdv::test_types_service::STATUS_ERROR) {
            error_sensors.push_back(reading.sensor_id());
        }
    }
    
    response->set_processed_count(processed_count);
    
    if (processed_count > 0) {
        response->set_average_temperature(temperature_sum / processed_count);
    } else {
        response->set_average_temperature(0);
    }
    
    for (uint32_t sensor_id : error_sensors) {
        response->add_error_sensors(sensor_id);
    }
    
    summary["processed_count"] = processed_count;
    summary["average_temperature"] = response->average_temperature();
    summary["error_sensor_count"] = error_sensors.size();
    
    response->set_summary_report(summary.dump());
    
    return grpc::Status::OK;
}

} // namespace test_types