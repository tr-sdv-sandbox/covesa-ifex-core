/**
 * @file cloud_discovery_service.cpp
 * @brief In-memory cloud discovery service implementation
 */

#include "cloud_discovery_service.hpp"
#include "cloud-backend-transport-service.grpc.pb.h"

#include <glog/logging.h>
#include <grpcpp/grpcpp.h>
#include <yaml-cpp/yaml.h>

#include <chrono>

namespace ifex::cloud {

namespace transport_pb = swdv::cloud_backend_transport_service;
namespace sync_pb = swdv::discovery_sync_envelope;

// =============================================================================
// TransportClient - wrapper for cloud backend transport
// =============================================================================

class CloudDiscoveryService::TransportClient {
public:
    TransportClient(const std::string& endpoint, uint32_t content_id)
        : endpoint_(endpoint), content_id_(content_id) {
        channel_ = grpc::CreateChannel(endpoint, grpc::InsecureChannelCredentials());
    }

    bool Connect() {
        auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(5);
        return channel_->WaitForConnected(deadline);
    }

    bool IsConnected() {
        auto state = channel_->GetState(false);
        return state == GRPC_CHANNEL_READY;
    }

    // Subscribe to vehicle messages (blocking call that should run in a thread)
    void SubscribeToMessages(
        std::function<void(const std::string&, const std::vector<uint8_t>&)> callback,
        std::atomic<bool>& running) {

        auto stub = transport_pb::on_vehicle_message_service::NewStub(channel_);

        while (running.load()) {
            grpc::ClientContext context;
            transport_pb::on_vehicle_message_subscribe_request request;
            request.set_content_id(content_id_);  // Specify content_id for on-demand MQTT subscription

            auto reader = stub->subscribe(&context, request);
            transport_pb::on_vehicle_message msg;

            while (reader->Read(&msg) && running.load()) {
                const auto& vehicle_msg = msg.message();
                std::vector<uint8_t> payload(
                    vehicle_msg.payload().begin(),
                    vehicle_msg.payload().end());
                callback(vehicle_msg.vehicle_id(), payload);
            }

            auto status = reader->Finish();
            if (!status.ok() && running.load()) {
                LOG(WARNING) << "Message subscription ended: " << status.error_message();
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
    }

    // Send message to vehicle
    bool SendToVehicle(const std::string& vehicle_id, const std::vector<uint8_t>& payload) {
        auto stub = transport_pb::send_to_vehicle_service::NewStub(channel_);

        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));

        transport_pb::send_to_vehicle_request request;
        auto* req = request.mutable_request();
        req->set_vehicle_id(vehicle_id);
        req->set_content_id(content_id_);  // Required for c2v routing
        req->set_payload(payload.data(), payload.size());
        req->set_persistence(transport_pb::VOLATILE);

        transport_pb::send_to_vehicle_response response;
        auto status = stub->send_to_vehicle(&context, request, &response);

        if (!status.ok()) {
            LOG(WARNING) << "Failed to send to vehicle " << vehicle_id
                         << ": " << status.error_message();
            return false;
        }

        return response.result().status() == transport_pb::OK;
    }

private:
    std::string endpoint_;
    uint32_t content_id_;
    std::shared_ptr<grpc::Channel> channel_;
};

// =============================================================================
// CloudDiscoveryService
// =============================================================================

CloudDiscoveryService::CloudDiscoveryService(const Config& config)
    : config_(config) {
    LOG(INFO) << "Creating CloudDiscoveryService";
    LOG(INFO) << "  Transport endpoint: " << config_.transport_endpoint;
    LOG(INFO) << "  Content ID: " << config_.content_id;
}

CloudDiscoveryService::~CloudDiscoveryService() {
    Stop();
}

bool CloudDiscoveryService::Start() {
    if (running_.load()) {
        LOG(WARNING) << "CloudDiscoveryService already running";
        return true;
    }

    LOG(INFO) << "Starting CloudDiscoveryService...";

    transport_ = std::make_unique<TransportClient>(
        config_.transport_endpoint, config_.content_id);

    if (!transport_->Connect()) {
        LOG(ERROR) << "Failed to connect to transport at " << config_.transport_endpoint;
        return false;
    }

    connected_.store(true);
    running_.store(true);

    // Start message subscription thread
    subscription_thread_ = std::thread([this]() {
        transport_->SubscribeToMessages(
            [this](const std::string& vehicle_id, const std::vector<uint8_t>& payload) {
                HandleV2cMessage(vehicle_id, payload);
            },
            running_);
    });

    LOG(INFO) << "CloudDiscoveryService started";
    return true;
}

void CloudDiscoveryService::Stop() {
    if (!running_.load()) {
        return;
    }

    LOG(INFO) << "Stopping CloudDiscoveryService...";
    running_.store(false);
    connected_.store(false);

    if (subscription_thread_.joinable()) {
        subscription_thread_.join();
    }

    transport_.reset();
    LOG(INFO) << "CloudDiscoveryService stopped";
}

int64_t CloudDiscoveryService::NowMs() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// =============================================================================
// Message Handling
// =============================================================================

void CloudDiscoveryService::HandleV2cMessage(
    const std::string& vehicle_id,
    const std::vector<uint8_t>& payload) {

    sync_pb::discovery_envelope_t envelope;
    if (!envelope.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
        LOG(WARNING) << "Failed to parse discovery envelope from " << vehicle_id;
        return;
    }

    // Use vehicle_id from envelope if set, otherwise use transport-provided one
    std::string vid = envelope.vehicle_id().empty() ? vehicle_id : envelope.vehicle_id();

    if (envelope.has_manifest()) {
        ProcessHashManifest(vid, envelope.manifest());
    }

    if (envelope.has_schemas()) {
        ProcessSchemas(vid, envelope.schemas());
    }
}

void CloudDiscoveryService::ProcessHashManifest(
    const std::string& vehicle_id,
    const sync_pb::hash_list_t& manifest) {

    LOG(INFO) << "Received hash manifest from " << vehicle_id
              << " with " << manifest.hashes_size() << " hashes";

    std::vector<std::string> unknown_hashes;
    std::unordered_set<std::string> vehicle_hashes;

    {
        std::shared_lock<std::shared_mutex> lock(schemas_mutex_);

        for (const auto& entry : manifest.hashes()) {
            vehicle_hashes.insert(entry.schema_hash());

            if (schemas_.find(entry.schema_hash()) == schemas_.end()) {
                unknown_hashes.push_back(entry.schema_hash());
            }
        }
    }

    // Update vehicle state
    {
        std::unique_lock<std::shared_mutex> lock(vehicles_mutex_);
        auto& state = vehicles_[vehicle_id];
        state.schema_hashes = std::move(vehicle_hashes);
        state.last_sync_ms = NowMs();
    }

    // Request unknown schemas
    if (!unknown_hashes.empty()) {
        LOG(INFO) << "Requesting " << unknown_hashes.size()
                  << " unknown schemas from " << vehicle_id;
        SendSchemaRequest(vehicle_id, unknown_hashes);
    }
}

void CloudDiscoveryService::ProcessSchemas(
    const std::string& vehicle_id,
    const sync_pb::schema_map_t& schemas) {

    LOG(INFO) << "Received " << schemas.schemas_size()
              << " schemas from " << vehicle_id;

    std::unique_lock<std::shared_mutex> lock(schemas_mutex_);

    for (const auto& schema : schemas.schemas()) {
        const auto& hash = schema.schema_hash();

        if (schemas_.find(hash) != schemas_.end()) {
            // Already have this schema, just add vehicle to the set
            schemas_[hash].vehicle_ids.insert(vehicle_id);
            continue;
        }

        // Parse IFEX YAML to extract service name and version
        std::string service_name;
        std::string version;

        try {
            YAML::Node yaml = YAML::Load(schema.ifex_schema());
            service_name = yaml["name"].as<std::string>("");
            int major = yaml["major_version"].as<int>(1);
            int minor = yaml["minor_version"].as<int>(0);
            version = std::to_string(major) + "." + std::to_string(minor);
        } catch (const std::exception& e) {
            LOG(WARNING) << "Failed to parse IFEX YAML for hash " << hash
                         << ": " << e.what();
            service_name = "unknown";
            version = "0.0";
        }

        // Store new schema
        SchemaData data;
        data.schema_hash = hash;
        data.service_name = service_name;
        data.version = version;
        data.ifex_yaml = schema.ifex_schema();
        data.first_seen_ms = NowMs();
        data.vehicle_ids.insert(vehicle_id);

        schemas_[hash] = std::move(data);

        LOG(INFO) << "Stored new schema: " << service_name << " v" << version
                  << " (hash=" << hash.substr(0, 8) << "...)";
    }
}

void CloudDiscoveryService::SendSchemaRequest(
    const std::string& vehicle_id,
    const std::vector<std::string>& hashes) {

    sync_pb::discovery_envelope_t envelope;
    envelope.set_vehicle_id(vehicle_id);

    auto* request = envelope.mutable_request();
    for (const auto& hash : hashes) {
        request->add_hashes(hash);
    }

    std::string serialized;
    if (!envelope.SerializeToString(&serialized)) {
        LOG(ERROR) << "Failed to serialize schema request";
        return;
    }

    std::vector<uint8_t> payload(serialized.begin(), serialized.end());

    if (transport_->SendToVehicle(vehicle_id, payload)) {
        LOG(INFO) << "Sent schema request to " << vehicle_id
                  << " for " << hashes.size() << " hashes";
    }
}

// =============================================================================
// gRPC Method Implementations
// =============================================================================

grpc::Status CloudDiscoveryService::get_vehicle_services(
    grpc::ServerContext* /*context*/,
    const swdv::cloud_discovery_service::get_vehicle_services_request* request,
    swdv::cloud_discovery_service::get_vehicle_services_response* response) {

    const auto& vehicle_id = request->vehicle_id();

    std::shared_lock<std::shared_mutex> vehicles_lock(vehicles_mutex_);
    auto vit = vehicles_.find(vehicle_id);

    if (vit == vehicles_.end()) {
        // Vehicle not found - return empty with UNKNOWN sync status
        auto* sync_info = response->mutable_sync_info();
        sync_info->set_vehicle_id(vehicle_id);
        sync_info->set_sync_status(swdv::cloud_discovery_service::UNKNOWN);
        return grpc::Status::OK;
    }

    const auto& vehicle_state = vit->second;

    // Fill sync info
    auto* sync_info = response->mutable_sync_info();
    sync_info->set_vehicle_id(vehicle_id);
    sync_info->set_sync_status(swdv::cloud_discovery_service::SYNCED);
    sync_info->set_last_sync_ms(vehicle_state.last_sync_ms);
    sync_info->set_service_count(static_cast<uint32_t>(vehicle_state.schema_hashes.size()));

    // Get schemas for this vehicle
    std::shared_lock<std::shared_mutex> schemas_lock(schemas_mutex_);

    for (const auto& hash : vehicle_state.schema_hashes) {
        auto sit = schemas_.find(hash);
        if (sit == schemas_.end()) continue;

        const auto& schema = sit->second;
        auto* svc = response->add_services();
        svc->set_vehicle_id(vehicle_id);
        svc->set_name(schema.service_name);
        svc->set_version(schema.version);
        svc->set_schema_hash(hash);
        svc->set_status(swdv::cloud_discovery_service::AVAILABLE);
    }

    return grpc::Status::OK;
}

grpc::Status CloudDiscoveryService::find_services(
    grpc::ServerContext* /*context*/,
    const swdv::cloud_discovery_service::find_services_request* request,
    swdv::cloud_discovery_service::find_services_response* response) {

    const auto& filter = request->filter();
    uint32_t count = 0;

    std::shared_lock<std::shared_mutex> vehicles_lock(vehicles_mutex_);
    std::shared_lock<std::shared_mutex> schemas_lock(schemas_mutex_);

    for (const auto& [vehicle_id, vehicle_state] : vehicles_) {
        // Apply vehicle filter
        if (!filter.vehicle_id().empty() && filter.vehicle_id() != vehicle_id) {
            continue;
        }

        for (const auto& hash : vehicle_state.schema_hashes) {
            auto sit = schemas_.find(hash);
            if (sit == schemas_.end()) continue;

            const auto& schema = sit->second;

            // Apply service name filter
            if (!filter.service_name().empty() &&
                filter.service_name() != schema.service_name) {
                continue;
            }

            auto* svc = response->add_services();
            svc->set_vehicle_id(vehicle_id);
            svc->set_name(schema.service_name);
            svc->set_version(schema.version);
            svc->set_schema_hash(hash);
            svc->set_status(swdv::cloud_discovery_service::AVAILABLE);
            count++;
        }
    }

    response->set_total_count(count);
    return grpc::Status::OK;
}

grpc::Status CloudDiscoveryService::get_fleet_capabilities(
    grpc::ServerContext* /*context*/,
    const swdv::cloud_discovery_service::get_fleet_capabilities_request* /*request*/,
    swdv::cloud_discovery_service::get_fleet_capabilities_response* response) {

    std::shared_lock<std::shared_mutex> lock(schemas_mutex_);

    for (const auto& [hash, schema] : schemas_) {
        auto* cap = response->add_capabilities();
        cap->set_service_name(schema.service_name);
        cap->set_version(schema.version);
        cap->set_vehicle_count(static_cast<uint32_t>(schema.vehicle_ids.size()));
        cap->set_available_count(static_cast<uint32_t>(schema.vehicle_ids.size()));

        // Parse methods from IFEX YAML
        try {
            YAML::Node yaml = YAML::Load(schema.ifex_yaml);
            if (yaml["namespaces"]) {
                for (const auto& ns : yaml["namespaces"]) {
                    if (ns["methods"]) {
                        for (const auto& method : ns["methods"]) {
                            cap->add_methods(method["name"].as<std::string>());
                        }
                    }
                }
            }
        } catch (...) {
            // Ignore parse errors
        }
    }

    return grpc::Status::OK;
}

grpc::Status CloudDiscoveryService::get_schema(
    grpc::ServerContext* /*context*/,
    const swdv::cloud_discovery_service::get_schema_request* request,
    swdv::cloud_discovery_service::get_schema_response* response) {

    const auto& hash = request->schema_hash();

    std::shared_lock<std::shared_mutex> lock(schemas_mutex_);
    auto it = schemas_.find(hash);

    if (it == schemas_.end()) {
        response->set_found(false);
        return grpc::Status::OK;
    }

    response->set_found(true);
    auto* schema = response->mutable_schema();
    schema->set_schema_hash(it->second.schema_hash);
    schema->set_service_name(it->second.service_name);
    schema->set_version(it->second.version);
    schema->set_ifex_yaml(it->second.ifex_yaml);
    schema->set_first_seen_ms(it->second.first_seen_ms);
    schema->set_vehicle_count(static_cast<uint32_t>(it->second.vehicle_ids.size()));

    return grpc::Status::OK;
}

grpc::Status CloudDiscoveryService::list_schemas(
    grpc::ServerContext* /*context*/,
    const swdv::cloud_discovery_service::list_schemas_request* request,
    swdv::cloud_discovery_service::list_schemas_response* response) {

    const auto& filter = request->service_name_filter();

    std::shared_lock<std::shared_mutex> lock(schemas_mutex_);

    for (const auto& [hash, data] : schemas_) {
        if (!filter.empty() && data.service_name.find(filter) == std::string::npos) {
            continue;
        }

        auto* schema = response->add_schemas();
        schema->set_schema_hash(data.schema_hash);
        schema->set_service_name(data.service_name);
        schema->set_version(data.version);
        schema->set_ifex_yaml(data.ifex_yaml);
        schema->set_first_seen_ms(data.first_seen_ms);
        schema->set_vehicle_count(static_cast<uint32_t>(data.vehicle_ids.size()));
    }

    return grpc::Status::OK;
}

grpc::Status CloudDiscoveryService::get_vehicle_sync_status(
    grpc::ServerContext* /*context*/,
    const swdv::cloud_discovery_service::get_vehicle_sync_status_request* request,
    swdv::cloud_discovery_service::get_vehicle_sync_status_response* response) {

    const auto& vehicle_id = request->vehicle_id();

    std::shared_lock<std::shared_mutex> lock(vehicles_mutex_);
    auto it = vehicles_.find(vehicle_id);

    auto* info = response->mutable_info();
    info->set_vehicle_id(vehicle_id);

    if (it == vehicles_.end()) {
        info->set_sync_status(swdv::cloud_discovery_service::UNKNOWN);
    } else {
        info->set_sync_status(swdv::cloud_discovery_service::SYNCED);
        info->set_last_sync_ms(it->second.last_sync_ms);
        info->set_service_count(static_cast<uint32_t>(it->second.schema_hashes.size()));
    }

    return grpc::Status::OK;
}

grpc::Status CloudDiscoveryService::list_vehicles(
    grpc::ServerContext* /*context*/,
    const swdv::cloud_discovery_service::list_vehicles_request* /*request*/,
    swdv::cloud_discovery_service::list_vehicles_response* response) {

    std::shared_lock<std::shared_mutex> lock(vehicles_mutex_);

    for (const auto& [vehicle_id, state] : vehicles_) {
        auto* info = response->add_vehicles();
        info->set_vehicle_id(vehicle_id);
        info->set_sync_status(swdv::cloud_discovery_service::SYNCED);
        info->set_last_sync_ms(state.last_sync_ms);
        info->set_service_count(static_cast<uint32_t>(state.schema_hashes.size()));
    }

    return grpc::Status::OK;
}

grpc::Status CloudDiscoveryService::healthy(
    grpc::ServerContext* /*context*/,
    const swdv::cloud_discovery_service::healthy_request* /*request*/,
    swdv::cloud_discovery_service::healthy_response* response) {

    response->set_is_healthy(connected_.load());
    return grpc::Status::OK;
}

}  // namespace ifex::cloud
