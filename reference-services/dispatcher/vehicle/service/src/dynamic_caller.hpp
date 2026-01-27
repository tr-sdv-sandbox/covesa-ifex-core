#pragma once

#include "ifex/types.hpp"
#include <memory>
#include <string>

namespace ifex {

class DynamicCaller {
public:
    virtual ~DynamicCaller() = default;
    
    // Create a dynamic caller
    static std::unique_ptr<DynamicCaller> create(const std::string& discovery_endpoint);
    
    // Call a method on a service
    virtual CallResponse call_method(
        const CallRequest& request,
        const std::string& ifex_schema) = 0;
    
    // Call with explicit endpoint (bypass discovery)
    virtual CallResponse call_method(
        const CallRequest& request,
        const ServiceEndpoint& endpoint,
        const std::string& ifex_schema) = 0;
    
    // Generate example parameters for a method
    virtual std::string generate_example_parameters(
        const std::string& service_name,
        const std::string& method_name,
        const std::string& ifex_schema) = 0;
};

} // namespace ifex