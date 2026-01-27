#pragma once

#include "cloud-discovery-service.grpc.pb.h"
#include "discovery-sync-envelope.pb.h"

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ifex::cloud {

/// In-memory cloud discovery service.
///
/// Receives discovery sync messages from vehicles via cloud-backend-transport,
/// stores schemas in memory (deduplicated by hash), and provides gRPC API for queries.
///
/// Protocol (content_id=201):
/// - Vehicle -> Cloud: Hash manifest ([service_name, schema_hash] pairs)
/// - Cloud -> Vehicle: Schema request (list of unknown hashes)
/// - Vehicle -> Cloud: Full IFEX YAML for requested hashes
class CloudDiscoveryService final
    : public swdv::cloud_discovery_service::get_vehicle_services_service::Service,
      public swdv::cloud_discovery_service::find_services_service::Service,
      public swdv::cloud_discovery_service::get_fleet_capabilities_service::Service,
      public swdv::cloud_discovery_service::get_schema_service::Service,
      public swdv::cloud_discovery_service::list_schemas_service::Service,
      public swdv::cloud_discovery_service::get_vehicle_sync_status_service::Service,
      public swdv::cloud_discovery_service::list_vehicles_service::Service,
      public swdv::cloud_discovery_service::healthy_service::Service {
public:
    struct Config {
        // Cloud backend transport endpoint for receiving/sending sync messages
        std::string transport_endpoint = "localhost:50100";

        // Content ID for discovery sync (201)
        uint32_t content_id = 201;
    };

    explicit CloudDiscoveryService(const Config& config);
    ~CloudDiscoveryService();

    // Non-copyable
    CloudDiscoveryService(const CloudDiscoveryService&) = delete;
    CloudDiscoveryService& operator=(const CloudDiscoveryService&) = delete;

    /// Start the service (connect to transport, subscribe to messages)
    bool Start();

    /// Stop the service
    void Stop();

    /// Check if connected to transport
    bool IsConnected() const { return connected_.load(); }

    // =========================================================================
    // gRPC Method Implementations
    // =========================================================================

    grpc::Status get_vehicle_services(
        grpc::ServerContext* context,
        const swdv::cloud_discovery_service::get_vehicle_services_request* request,
        swdv::cloud_discovery_service::get_vehicle_services_response* response) override;

    grpc::Status find_services(
        grpc::ServerContext* context,
        const swdv::cloud_discovery_service::find_services_request* request,
        swdv::cloud_discovery_service::find_services_response* response) override;

    grpc::Status get_fleet_capabilities(
        grpc::ServerContext* context,
        const swdv::cloud_discovery_service::get_fleet_capabilities_request* request,
        swdv::cloud_discovery_service::get_fleet_capabilities_response* response) override;

    grpc::Status get_schema(
        grpc::ServerContext* context,
        const swdv::cloud_discovery_service::get_schema_request* request,
        swdv::cloud_discovery_service::get_schema_response* response) override;

    grpc::Status list_schemas(
        grpc::ServerContext* context,
        const swdv::cloud_discovery_service::list_schemas_request* request,
        swdv::cloud_discovery_service::list_schemas_response* response) override;

    grpc::Status get_vehicle_sync_status(
        grpc::ServerContext* context,
        const swdv::cloud_discovery_service::get_vehicle_sync_status_request* request,
        swdv::cloud_discovery_service::get_vehicle_sync_status_response* response) override;

    grpc::Status list_vehicles(
        grpc::ServerContext* context,
        const swdv::cloud_discovery_service::list_vehicles_request* request,
        swdv::cloud_discovery_service::list_vehicles_response* response) override;

    grpc::Status healthy(
        grpc::ServerContext* context,
        const swdv::cloud_discovery_service::healthy_request* request,
        swdv::cloud_discovery_service::healthy_response* response) override;

private:
    // Handle incoming v2c message from vehicle
    void HandleV2cMessage(const std::string& vehicle_id, const std::vector<uint8_t>& payload);

    // Send schema request to vehicle (c2v)
    void SendSchemaRequest(const std::string& vehicle_id, const std::vector<std::string>& hashes);

    // Process hash manifest from vehicle
    void ProcessHashManifest(const std::string& vehicle_id,
                             const swdv::discovery_sync_envelope::hash_list_t& manifest);

    // Process schema response from vehicle
    void ProcessSchemas(const std::string& vehicle_id,
                        const swdv::discovery_sync_envelope::schema_map_t& schemas);

    // Helper to get current time in ms
    int64_t NowMs() const;

    Config config_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> running_{false};

    // Transport client (forward declared, implemented in cpp)
    class TransportClient;
    std::unique_ptr<TransportClient> transport_;
    std::thread subscription_thread_;

    // Schema storage (hash -> schema data)
    struct SchemaData {
        std::string schema_hash;
        std::string service_name;
        std::string version;
        std::string ifex_yaml;
        int64_t first_seen_ms = 0;
        std::unordered_set<std::string> vehicle_ids;  // Vehicles with this schema
    };
    mutable std::shared_mutex schemas_mutex_;
    std::unordered_map<std::string, SchemaData> schemas_;  // hash -> SchemaData

    // Per-vehicle state
    struct VehicleState {
        std::unordered_set<std::string> schema_hashes;  // Hashes this vehicle has
        int64_t last_sync_ms = 0;
        uint32_t state_checksum = 0;
    };
    mutable std::shared_mutex vehicles_mutex_;
    std::unordered_map<std::string, VehicleState> vehicles_;
};

}  // namespace ifex::cloud
