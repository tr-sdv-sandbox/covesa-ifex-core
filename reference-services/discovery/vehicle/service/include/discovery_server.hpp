#pragma once

#include "discovery-service.grpc.pb.h"
#include <grpcpp/grpcpp.h>
#include <memory>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <chrono>

namespace ifex::reference {

// Service registration entry
struct ServiceRegistration {
    std::string registration_id;
    std::string name;
    std::string version;
    std::string description;
    std::string address;
    std::string transport;
    std::chrono::system_clock::time_point last_heartbeat;
    swdv::service_discovery::service_status_t status;

    // Store IFEX interface definition
    std::vector<swdv::service_discovery::namespace_info_t> namespaces;
    std::string ifex_schema;

    // SHA-256 hash of ifex_schema (64 hex chars) for efficient cloud sync
    std::string schema_hash;

    // Helper to check if service is available for new requests
    bool is_available() const {
        return status == swdv::service_discovery::service_status_t::AVAILABLE ||
               status == swdv::service_discovery::service_status_t::STARTING;
    }
};

// Combined service that implements multiple IFEX-generated services
class DiscoveryServer final :
    public swdv::service_discovery::get_service_service::Service,
    public swdv::service_discovery::register_service_service::Service,
    public swdv::service_discovery::unregister_service_service::Service,
    public swdv::service_discovery::query_services_service::Service,
    public swdv::service_discovery::heartbeat_service::Service,
    public swdv::service_discovery::get_service_hashes_service::Service,
    public swdv::service_discovery::get_schemas_by_hash_service::Service {
public:
    DiscoveryServer();
    ~DiscoveryServer();
    
    // Server lifecycle
    void Start(const std::string& listen_address);
    void Shutdown();
    void Wait();
    
    // get_service_service methods
    grpc::Status get_service(grpc::ServerContext* context,
                           const swdv::service_discovery::get_service_request* request,
                           swdv::service_discovery::get_service_response* response) override;

    // register_service_service methods  
    grpc::Status register_service(grpc::ServerContext* context,
                               const swdv::service_discovery::register_service_request* request,
                               swdv::service_discovery::register_service_response* response) override;

    // unregister_service_service methods
    grpc::Status unregister_service(grpc::ServerContext* context,
                                  const swdv::service_discovery::unregister_service_request* request,
                                  swdv::service_discovery::unregister_service_response* response) override;

    // query_services_service methods
    grpc::Status query_services(grpc::ServerContext* context,
                             const swdv::service_discovery::query_services_request* request,
                             swdv::service_discovery::query_services_response* response) override;

    // heartbeat_service methods
    grpc::Status heartbeat(grpc::ServerContext* context,
                        const swdv::service_discovery::heartbeat_request* request,
                        swdv::service_discovery::heartbeat_response* response) override;

    // get_service_hashes_service methods (for efficient cloud sync)
    grpc::Status get_service_hashes(grpc::ServerContext* context,
                        const swdv::service_discovery::get_service_hashes_request* request,
                        swdv::service_discovery::get_service_hashes_response* response) override;

    // get_schemas_by_hash_service methods (for efficient cloud sync)
    grpc::Status get_schemas_by_hash(grpc::ServerContext* context,
                        const swdv::service_discovery::get_schemas_by_hash_request* request,
                        swdv::service_discovery::get_schemas_by_hash_response* response) override;


private:
    std::unique_ptr<grpc::Server> server_;
    std::mutex mutex_;
    std::atomic<int> next_registration_id_{1};
    
    // Service storage - indexed by registration ID
    std::unordered_map<std::string, ServiceRegistration> registered_services_;
    
    // Secondary index by service name
    std::unordered_multimap<std::string, std::string> services_by_name_;

    // Index by schema hash for efficient hash-based lookups
    std::unordered_multimap<std::string, std::string> services_by_hash_;

    // Helper methods
    std::string generate_registration_id();
    bool matches_filter(const ServiceRegistration& service,
                       const swdv::service_discovery::service_filter_t& filter);
    bool check_extension_path(const std::string& ifex_yaml,
                            const std::string& extension_path);

    // Compute SHA-256 hash of schema (returns 64 hex chars)
    static std::string compute_schema_hash(const std::string& ifex_schema);
};

} // namespace ifex::reference