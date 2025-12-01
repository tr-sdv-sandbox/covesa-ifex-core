#pragma once

#include "ifex/types.hpp"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ifex {

class DiscoveryClient {
public:
    virtual ~DiscoveryClient() = default;
    
    // Create a discovery client
    static std::unique_ptr<DiscoveryClient> create(const std::string& discovery_endpoint);
    
    // Service registration
    virtual std::string register_service(
        const ServiceEndpoint& endpoint,
        const std::string& ifex_schema) = 0;
    
    // Service unregistration
    virtual bool unregister_service(const std::string& registration_id) = 0;
    
    // Send heartbeat
    virtual bool send_heartbeat(
        const std::string& registration_id,
        ServiceStatus status = ServiceStatus::AVAILABLE) = 0;
    
    // Service discovery
    virtual std::optional<ServiceInfo> get_service(const std::string& service_name) = 0;
    virtual std::vector<ServiceInfo> query_services(const ServiceFilter& filter = {}) = 0;
    
    // Status change notifications
    using StatusCallback = std::function<void(const std::string& service_name, ServiceStatus status)>;
    virtual void register_status_callback(StatusCallback callback) = 0;
};

} // namespace ifex