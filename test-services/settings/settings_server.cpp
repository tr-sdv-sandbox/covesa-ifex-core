#include "settings_server.hpp"
#include <glog/logging.h>
#include <fstream>
#include <chrono>

namespace ifex::settings {

SettingsServer::SettingsServer() {
    LOG(INFO) << "Initializing Settings Server";
}

void SettingsServer::LoadPresetsFromFile(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        LOG(WARNING) << "Could not open presets file: " << filename;
        return;
    }
    
    try {
        json presets_data = json::parse(file);
        
        for (const auto& namespace_data : presets_data["presets"]) {
            std::string namespace_name = namespace_data["namespace"];
            std::string method_name = namespace_data["method"];
            std::string service_name = presets_data["service"].get<std::string>();
            std::string service_method = service_name + "." + namespace_name + "." + method_name;
            
            for (const auto& preset : namespace_data["presets"]) {
                PresetStorage::Preset p;
                p.metadata = {
                    {"name", preset["name"]},
                    {"display_name", preset["display_name"]},
                    {"description", preset["description"]},
                    {"type", preset["type"]},
                    {"scope", preset["scope"]},
                    {"created_timestamp", std::chrono::system_clock::now().time_since_epoch().count()},
                    {"modified_timestamp", std::chrono::system_clock::now().time_since_epoch().count()},
                    {"created_by", "system"},
                    {"tags", preset["tags"]}
                };
                p.values = preset["values"];
                
                std::lock_guard<std::mutex> lock(storage_.mutex);
                storage_.presets[service_method][preset["name"]] = p;
            }
        }
        
        LOG(INFO) << "Loaded presets from " << filename;
    } catch (const std::exception& e) {
        LOG(ERROR) << "Failed to load presets: " << e.what();
    }
    
    file.close();
}

std::string SettingsServer::MakeKey(const swdv::settings_service::service_method_t& service_method) {
    return service_method.service_name() + "." + 
           service_method.namespace_name() + "." + 
           service_method.method_name();
}

void SettingsServer::PresetToProto(const PresetStorage::Preset& preset, 
                                   swdv::settings_service::preset_t* proto) {
    // Set metadata
    auto* metadata = proto->mutable_metadata();
    metadata->set_name(preset.metadata["name"]);
    metadata->set_display_name(preset.metadata["display_name"]);
    metadata->set_description(preset.metadata["description"]);
    
    // Convert type string to enum
    std::string type_str = preset.metadata["type"];
    if (type_str == "TYPE_SYSTEM") {
        metadata->set_type(swdv::settings_service::TYPE_SYSTEM);
    } else if (type_str == "TYPE_USER") {
        metadata->set_type(swdv::settings_service::TYPE_USER);
    } else if (type_str == "TYPE_FLEET") {
        metadata->set_type(swdv::settings_service::TYPE_FLEET);
    } else if (type_str == "TYPE_DEVICE") {
        metadata->set_type(swdv::settings_service::TYPE_DEVICE);
    }
    
    // Convert scope string to enum
    std::string scope_str = preset.metadata["scope"];
    if (scope_str == "SCOPE_GLOBAL") {
        metadata->set_scope(swdv::settings_service::SCOPE_GLOBAL);
    } else if (scope_str == "SCOPE_USER") {
        metadata->set_scope(swdv::settings_service::SCOPE_USER);
    } else if (scope_str == "SCOPE_DEVICE") {
        metadata->set_scope(swdv::settings_service::SCOPE_DEVICE);
    } else if (scope_str == "SCOPE_GROUP") {
        metadata->set_scope(swdv::settings_service::SCOPE_GROUP);
    }
    
    metadata->set_created_timestamp(preset.metadata["created_timestamp"]);
    metadata->set_modified_timestamp(preset.metadata["modified_timestamp"]);
    metadata->set_created_by(preset.metadata["created_by"]);
    
    for (const auto& tag : preset.metadata["tags"]) {
        metadata->add_tags(tag);
    }
    
    // Set values and version
    proto->set_values(preset.values.dump());
    proto->set_version(preset.version);
}

PresetStorage::Preset SettingsServer::PresetFromProto(const swdv::settings_service::preset_t& proto) {
    PresetStorage::Preset preset;
    
    const auto& metadata = proto.metadata();
    preset.metadata = {
        {"name", metadata.name()},
        {"display_name", metadata.display_name()},
        {"description", metadata.description()},
        {"created_by", metadata.created_by()},
        {"created_timestamp", metadata.created_timestamp()},
        {"modified_timestamp", metadata.modified_timestamp()}
    };
    
    // Convert enum to string
    switch (metadata.type()) {
        case swdv::settings_service::TYPE_SYSTEM:
            preset.metadata["type"] = "TYPE_SYSTEM";
            break;
        case swdv::settings_service::TYPE_USER:
            preset.metadata["type"] = "TYPE_USER";
            break;
        case swdv::settings_service::TYPE_FLEET:
            preset.metadata["type"] = "TYPE_FLEET";
            break;
        case swdv::settings_service::TYPE_DEVICE:
            preset.metadata["type"] = "TYPE_DEVICE";
            break;
    }
    
    switch (metadata.scope()) {
        case swdv::settings_service::SCOPE_GLOBAL:
            preset.metadata["scope"] = "SCOPE_GLOBAL";
            break;
        case swdv::settings_service::SCOPE_USER:
            preset.metadata["scope"] = "SCOPE_USER";
            break;
        case swdv::settings_service::SCOPE_DEVICE:
            preset.metadata["scope"] = "SCOPE_DEVICE";
            break;
        case swdv::settings_service::SCOPE_GROUP:
            preset.metadata["scope"] = "SCOPE_GROUP";
            break;
    }
    
    json tags = json::array();
    for (const auto& tag : metadata.tags()) {
        tags.push_back(tag);
    }
    preset.metadata["tags"] = tags;
    
    preset.values = json::parse(proto.values());
    preset.version = proto.version();
    
    return preset;
}

bool SettingsServer::MatchesFilter(const PresetStorage::Preset& preset,
                                  const swdv::settings_service::preset_filter_t& filter) {
    // For simplicity, if filter has no constraints, match all
    bool has_filter = filter.tags_size() > 0 || 
                     filter.created_after() > 0 || 
                     !filter.created_by().empty();
    
    // If completely empty filter, match all
    if (!has_filter) {
        return true;
    }
    
    // Check tags if specified
    if (filter.tags_size() > 0) {
        auto preset_tags = preset.metadata["tags"];
        for (const auto& required_tag : filter.tags()) {
            bool found = false;
            for (const auto& tag : preset_tags) {
                if (tag == required_tag) {
                    found = true;
                    break;
                }
            }
            if (!found) return false;
        }
    }
    
    // Check created_after if specified
    if (filter.created_after() > 0) {
        uint64_t created_timestamp = preset.metadata["created_timestamp"];
        if (created_timestamp < filter.created_after()) return false;
    }
    
    // Check created_by if specified
    if (!filter.created_by().empty()) {
        std::string created_by = preset.metadata["created_by"];
        if (created_by != filter.created_by()) return false;
    }
    
    return true;
}

grpc::Status SettingsServer::get_presets(grpc::ServerContext* context,
                                        const swdv::settings_service::get_presets_request* request,
                                        swdv::settings_service::get_presets_response* response) {
    LOG(INFO) << "get_presets called";
    LOG(INFO) << "Context peer: " << context->peer();
    
    std::string key = MakeKey(request->service_method());
    
    std::lock_guard<std::mutex> lock(storage_.mutex);
    auto it = storage_.presets.find(key);
    
    if (it != storage_.presets.end()) {
        for (const auto& [name, preset] : it->second) {
            // Apply filter if provided
            if (request->has_filter() && !MatchesFilter(preset, request->filter())) {
                continue;
            }
            
            auto* proto_preset = response->add_presets();
            PresetToProto(preset, proto_preset);
        }
    }
    
    LOG(INFO) << "Returning " << response->presets_size() << " presets for " << key;
    return grpc::Status::OK;
}

grpc::Status SettingsServer::get_preset(grpc::ServerContext* context,
                                       const swdv::settings_service::get_preset_request* request,
                                       swdv::settings_service::get_preset_response* response) {
    LOG(INFO) << "get_preset called for: " << request->preset_name();
    
    std::string key = MakeKey(request->service_method());
    
    std::lock_guard<std::mutex> lock(storage_.mutex);
    auto it = storage_.presets.find(key);
    
    if (it != storage_.presets.end()) {
        auto preset_it = it->second.find(request->preset_name());
        if (preset_it != it->second.end()) {
            PresetToProto(preset_it->second, response->mutable_preset());
            response->set_found(true);
            return grpc::Status::OK;
        }
    }
    
    response->set_found(false);
    return grpc::Status::OK;
}

grpc::Status SettingsServer::create_preset(grpc::ServerContext* context,
                                          const swdv::settings_service::create_preset_request* request,
                                          swdv::settings_service::create_preset_response* response) {
    LOG(INFO) << "create_preset called";
    
    std::string key = MakeKey(request->service_method());
    std::string preset_name = request->preset().metadata().name();
    
    std::lock_guard<std::mutex> lock(storage_.mutex);
    
    // Check if already exists
    if (storage_.presets[key].count(preset_name) > 0) {
        response->set_success(false);
        response->set_error_message("Preset already exists: " + preset_name);
        return grpc::Status::OK;
    }
    
    // Create new preset
    PresetStorage::Preset preset = PresetFromProto(request->preset());
    
    // Update timestamps
    auto now = std::chrono::system_clock::now().time_since_epoch();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    preset.metadata["created_timestamp"] = ms;
    preset.metadata["modified_timestamp"] = ms;
    
    storage_.presets[key][preset_name] = preset;
    
    response->set_success(true);
    LOG(INFO) << "Created preset: " << preset_name << " for " << key;
    
    return grpc::Status::OK;
}

grpc::Status SettingsServer::update_preset(grpc::ServerContext* context,
                                          const swdv::settings_service::update_preset_request* request,
                                          swdv::settings_service::update_preset_response* response) {
    LOG(INFO) << "update_preset called for: " << request->preset_name();
    
    std::string key = MakeKey(request->service_method());
    
    std::lock_guard<std::mutex> lock(storage_.mutex);
    auto it = storage_.presets.find(key);
    
    if (it == storage_.presets.end()) {
        response->set_success(false);
        response->set_error_message("Service method not found");
        return grpc::Status::OK;
    }
    
    auto preset_it = it->second.find(request->preset_name());
    if (preset_it == it->second.end()) {
        response->set_success(false);
        response->set_error_message("Preset not found");
        return grpc::Status::OK;
    }
    
    // Don't allow updating system presets
    std::string type_str = preset_it->second.metadata["type"];
    if (type_str == "TYPE_SYSTEM") {
        response->set_success(false);
        response->set_error_message("Cannot update system preset");
        return grpc::Status::OK;
    }
    
    // Update preset
    preset_it->second = PresetFromProto(request->preset());
    preset_it->second.version++;
    
    // Update timestamp
    auto now = std::chrono::system_clock::now().time_since_epoch();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    preset_it->second.metadata["modified_timestamp"] = ms;
    
    response->set_success(true);
    response->set_new_version(preset_it->second.version);
    
    LOG(INFO) << "Updated preset: " << request->preset_name() << " to version " << preset_it->second.version;
    
    return grpc::Status::OK;
}

grpc::Status SettingsServer::delete_preset(grpc::ServerContext* context,
                                          const swdv::settings_service::delete_preset_request* request,
                                          swdv::settings_service::delete_preset_response* response) {
    LOG(INFO) << "delete_preset called for: " << request->preset_name();
    
    std::string key = MakeKey(request->service_method());
    
    std::lock_guard<std::mutex> lock(storage_.mutex);
    auto it = storage_.presets.find(key);
    
    if (it == storage_.presets.end()) {
        response->set_success(false);
        response->set_error_message("Service method not found");
        return grpc::Status::OK;
    }
    
    auto preset_it = it->second.find(request->preset_name());
    if (preset_it == it->second.end()) {
        response->set_success(false);
        response->set_error_message("Preset not found");
        return grpc::Status::OK;
    }
    
    // Don't allow deleting system presets
    std::string type_str = preset_it->second.metadata["type"];
    if (type_str == "TYPE_SYSTEM") {
        response->set_success(false);
        response->set_error_message("Cannot delete system preset");
        return grpc::Status::OK;
    }
    
    it->second.erase(preset_it);
    response->set_success(true);
    
    LOG(INFO) << "Deleted preset: " << request->preset_name();
    
    return grpc::Status::OK;
}

grpc::Status SettingsServer::apply_preset(grpc::ServerContext* context,
                                         const swdv::settings_service::apply_preset_request* request,
                                         swdv::settings_service::apply_preset_response* response) {
    LOG(INFO) << "apply_preset called for: " << request->preset_name();
    
    std::string key = MakeKey(request->service_method());
    
    std::lock_guard<std::mutex> lock(storage_.mutex);
    auto it = storage_.presets.find(key);
    
    if (it == storage_.presets.end()) {
        response->set_success(false);
        response->set_values("{}");
        return grpc::Status::OK;
    }
    
    auto preset_it = it->second.find(request->preset_name());
    if (preset_it == it->second.end()) {
        response->set_success(false);
        response->set_values("{}");
        return grpc::Status::OK;
    }
    
    json values = preset_it->second.values;
    
    // Merge with provided values if any
    if (!request->merge_with().empty()) {
        try {
            json merge_values = json::parse(request->merge_with());
            values.merge_patch(merge_values);
        } catch (const std::exception& e) {
            LOG(WARNING) << "Failed to parse merge values: " << e.what();
        }
    }
    
    response->set_values(values.dump());
    response->set_success(true);
    
    LOG(INFO) << "Applied preset: " << request->preset_name() << " with values: " << values.dump();
    
    return grpc::Status::OK;
}

void SettingsServer::Start(const std::string& listen_address) {
    grpc::ServerBuilder builder;
    
    builder.AddListeningPort(listen_address, grpc::InsecureServerCredentials());
    
    // Register all services
    builder.RegisterService(static_cast<swdv::settings_service::get_presets_service::Service*>(this));
    builder.RegisterService(static_cast<swdv::settings_service::get_preset_service::Service*>(this));
    builder.RegisterService(static_cast<swdv::settings_service::create_preset_service::Service*>(this));
    builder.RegisterService(static_cast<swdv::settings_service::update_preset_service::Service*>(this));
    builder.RegisterService(static_cast<swdv::settings_service::delete_preset_service::Service*>(this));
    builder.RegisterService(static_cast<swdv::settings_service::apply_preset_service::Service*>(this));
    
    server_ = builder.BuildAndStart();
    LOG(INFO) << "Settings server listening on " << listen_address;
}

void SettingsServer::Shutdown() {
    if (server_) {
        server_->Shutdown();
        LOG(INFO) << "Settings server shut down";
    }
}

} // namespace ifex::settings