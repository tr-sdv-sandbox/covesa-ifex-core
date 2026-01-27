#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <optional>
#include <variant>

namespace ifex {

// Basic IFEX type enumeration
enum class Type {
    UINT8,
    UINT16,
    UINT32,
    UINT64,
    INT8,
    INT16,
    INT32,
    INT64,
    FLOAT,
    DOUBLE,
    BOOLEAN,
    STRING,
    BYTES,
    STRUCT,
    ARRAY,
    ENUM
};

// Parameter definition
struct Parameter {
    std::string name;
    Type type;
    std::string type_name;  // For struct/enum types
    std::string description;
    bool is_optional = false;
    bool is_array = false;
    std::optional<std::string> default_value;
    
    // Constraints
    struct Constraints {
        std::optional<double> min;
        std::optional<double> max;
        std::optional<size_t> min_items;
        std::optional<size_t> max_items;
    } constraints;
};

// Method signature
struct MethodSignature {
    std::string service_name;
    std::string namespace_name;
    std::string method_name;
    std::string description;
    std::vector<Parameter> input_parameters;
    std::vector<Parameter> output_parameters;
    std::unordered_map<std::string, std::string> metadata;  // For extensions
};

// Service endpoint information
struct ServiceEndpoint {
    enum class Transport {
        GRPC,
        HTTP_REST,
        DBUS,
        SOMEIP,
        MQTT
    };
    
    std::string address;
    Transport transport = Transport::GRPC;
    std::unordered_map<std::string, std::string> properties;
};

// Service status enumeration
enum class ServiceStatus {
    AVAILABLE,    // Service is running and accepting requests
    STARTING,     // Service is starting up
    STOPPING,     // Service is shutting down
    UNAVAILABLE,  // Service is not available
    ERROR         // Service is in error state
};

// Complete service information
struct ServiceInfo {
    std::string name;
    std::string version;
    std::string description;
    ServiceEndpoint endpoint;
    std::vector<MethodSignature> methods;
    std::string ifex_schema;  // Original IFEX YAML
    
    // Runtime information
    std::chrono::steady_clock::time_point last_seen;
    ServiceStatus status = ServiceStatus::AVAILABLE;
    
    // Helper to check if service is available for new requests
    bool is_available() const {
        return status == ServiceStatus::AVAILABLE || 
               status == ServiceStatus::STARTING;
    }
};

// Service filter for discovery
struct ServiceFilter {
    std::optional<std::string> name_pattern;
    std::optional<std::string> has_method;
    std::optional<ServiceEndpoint::Transport> transport;
    std::unordered_map<std::string, std::string> metadata_filters;
    bool show_all = false;  // If true, show all services regardless of availability
};

// Method call request
struct CallRequest {
    std::string service_name;
    std::string method_name;
    std::string parameters_json;  // JSON-encoded parameters
    std::chrono::milliseconds timeout = std::chrono::milliseconds(5000);
};

// Method call response
struct CallResponse {
    enum class Status {
        SUCCESS,
        TIMEOUT,
        SERVICE_UNAVAILABLE,
        METHOD_NOT_FOUND,
        INVALID_PARAMETERS,
        INTERNAL_ERROR
    };
    
    bool success = false;
    Status status = Status::INTERNAL_ERROR;
    std::string result_json;  // JSON-encoded result
    std::string error_message;
    std::chrono::milliseconds duration;
};

// Validation result
struct ValidationResult {
    bool valid = true;
    std::vector<std::string> errors;
};

// Struct definition
struct StructDefinition {
    std::string name;
    std::string description;
    std::vector<Parameter> members;
};

// Enum option
struct EnumOption {
    std::string name;
    std::string description;
    int64_t value;
};

// Enum definition
struct EnumDefinition {
    std::string name;
    std::string description;
    Type datatype;  // The underlying type (UINT8, INT32, etc.)
    std::vector<EnumOption> options;
};

} // namespace ifex