#pragma once

#include "settings-service.grpc.pb.h"
#include <grpcpp/grpcpp.h>
#include <nlohmann/json.hpp>
#include <map>
#include <mutex>
#include <string>

namespace ifex::settings {

using json = nlohmann::json;

// Preset storage
struct PresetStorage {
    struct Preset {
        json metadata;
        json values;
        uint32_t version = 1;
    };
    
    std::map<std::string, std::map<std::string, Preset>> presets; // service_method -> preset_name -> preset
    std::mutex mutex;
};

// Settings server implementation
class SettingsServer final : 
    public swdv::settings_service::get_presets_service::Service,
    public swdv::settings_service::get_preset_service::Service,
    public swdv::settings_service::create_preset_service::Service,
    public swdv::settings_service::update_preset_service::Service,
    public swdv::settings_service::delete_preset_service::Service,
    public swdv::settings_service::apply_preset_service::Service {
    
public:
    SettingsServer();
    
    // Initialize with presets from JSON file
    void LoadPresetsFromFile(const std::string& filename);
    
    // gRPC service implementations
    grpc::Status get_presets(grpc::ServerContext* context,
                            const swdv::settings_service::get_presets_request* request,
                            swdv::settings_service::get_presets_response* response) override;
                            
    grpc::Status get_preset(grpc::ServerContext* context,
                           const swdv::settings_service::get_preset_request* request,
                           swdv::settings_service::get_preset_response* response) override;
                           
    grpc::Status create_preset(grpc::ServerContext* context,
                              const swdv::settings_service::create_preset_request* request,
                              swdv::settings_service::create_preset_response* response) override;
                              
    grpc::Status update_preset(grpc::ServerContext* context,
                              const swdv::settings_service::update_preset_request* request,
                              swdv::settings_service::update_preset_response* response) override;
                              
    grpc::Status delete_preset(grpc::ServerContext* context,
                              const swdv::settings_service::delete_preset_request* request,
                              swdv::settings_service::delete_preset_response* response) override;
                              
    grpc::Status apply_preset(grpc::ServerContext* context,
                             const swdv::settings_service::apply_preset_request* request,
                             swdv::settings_service::apply_preset_response* response) override;

    // Start the gRPC server
    void Start(const std::string& listen_address);
    void Shutdown();
    
private:
    PresetStorage storage_;
    std::unique_ptr<grpc::Server> server_;
    
    // Helper to make service method key
    std::string MakeKey(const swdv::settings_service::service_method_t& service_method);
    
    // Helper to convert between proto and JSON
    void PresetToProto(const PresetStorage::Preset& preset, swdv::settings_service::preset_t* proto);
    PresetStorage::Preset PresetFromProto(const swdv::settings_service::preset_t& proto);
    
    // Helper to check if preset matches filter
    bool MatchesFilter(const PresetStorage::Preset& preset, 
                      const swdv::settings_service::preset_filter_t& filter);
};

} // namespace ifex::settings