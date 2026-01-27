#include "dynamic_caller.hpp"
#include <ifex/discovery.hpp>
#include <ifex/parser.hpp>
#include <grpcpp/grpcpp.h>
#include <grpcpp/generic/generic_stub.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/util/json_util.h>
#include <nlohmann/json.hpp>
#include <glog/logging.h>
#include <memory>
#include <map>
#include <mutex>
#include <chrono>
#include <thread>

namespace ifex {

using json = nlohmann::json;

class DynamicCallerImpl : public DynamicCaller {
private:
    // Service endpoint cache
    struct CachedEndpoint {
        ServiceEndpoint endpoint;
        std::chrono::steady_clock::time_point timestamp;
    };
    mutable std::mutex cache_mutex_;
    mutable std::map<std::string, CachedEndpoint> endpoint_cache_;
    static constexpr std::chrono::seconds CACHE_TTL{10};
    
    // Channel cache to reuse gRPC connections
    mutable std::mutex channel_mutex_;
    mutable std::map<std::string, std::shared_ptr<grpc::Channel>> channel_cache_;
    
    // Per-service descriptor pools to avoid conflicts
    mutable std::mutex descriptor_mutex_;
    std::map<std::string, std::shared_ptr<google::protobuf::DescriptorPool>> service_pools_;
    std::map<std::string, std::shared_ptr<google::protobuf::DynamicMessageFactory>> service_factories_;
    
    // Discovery client (cached)
    std::unique_ptr<DiscoveryClient> discovery_client_;
    
public:
    explicit DynamicCallerImpl(const std::string& discovery_endpoint) {
        LOG(INFO) << "Initializing Dynamic Caller with discovery endpoint: " << discovery_endpoint;
        discovery_client_ = DiscoveryClient::create(discovery_endpoint);
    }
    
    CallResponse call_method(
        const CallRequest& request,
        const std::string& ifex_schema) override {
        
        // Check cache first
        ServiceEndpoint endpoint;
        bool found_in_cache = false;
        
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            auto it = endpoint_cache_.find(request.service_name);
            if (it != endpoint_cache_.end()) {
                auto now = std::chrono::steady_clock::now();
                if ((now - it->second.timestamp) < CACHE_TTL) {
                    endpoint = it->second.endpoint;
                    found_in_cache = true;
                    LOG(INFO) << "Cache hit for service: " << request.service_name;
                }
            }
        }
        
        if (!found_in_cache) {
            // Use discovery to find the service endpoint
            auto service_info = discovery_client_->get_service(request.service_name);
            
            if (!service_info.has_value()) {
                CallResponse response;
                response.success = false;
                response.status = CallResponse::Status::SERVICE_UNAVAILABLE;
                response.error_message = "Service not found: " + request.service_name;
                return response;
            }
            
            endpoint = service_info->endpoint;
            
            // Cache the endpoint
            {
                std::lock_guard<std::mutex> lock(cache_mutex_);
                endpoint_cache_[request.service_name] = {endpoint, std::chrono::steady_clock::now()};
            }
        }
        
        return call_method(request, endpoint, ifex_schema);
    }
    
    CallResponse call_method(
        const CallRequest& request,
        const ServiceEndpoint& endpoint,
        const std::string& ifex_schema) override {
        
        auto start_time = std::chrono::high_resolution_clock::now();
        CallResponse response;
        
        try {
            LOG(INFO) << "Dynamic call to " << request.service_name << "." << request.method_name
                      << " at " << endpoint.address;
            
            // Parse the IFEX schema to understand the method
            auto parser = Parser::create(ifex_schema);
            
            // Verify the method exists
            if (!parser->has_method(request.method_name)) {
                LOG(ERROR) << "Method not found in IFEX schema: " << request.method_name;
                response.success = false;
                response.status = CallResponse::Status::METHOD_NOT_FOUND;
                response.error_message = "Method not found: " + request.method_name;
                auto end_time = std::chrono::high_resolution_clock::now();
                response.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
                return response;
            }
            LOG(INFO) << "Method found in IFEX schema: " << request.method_name;
            
            // Get method signature
            auto method_sig = parser->get_method_signature(request.method_name);
            
            // Parse parameters
            json params;
            try {
                params = json::parse(request.parameters_json);
                LOG(INFO) << "Parsed JSON parameters: " << params.dump(2);
            } catch (const json::parse_error& e) {
                response.success = false;
                response.status = CallResponse::Status::INVALID_PARAMETERS;
                response.error_message = "Invalid JSON parameters: " + std::string(e.what());
                auto end_time = std::chrono::high_resolution_clock::now();
                response.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
                return response;
            }
            
            // Build protobuf descriptors - this will create/get pools as needed
            const google::protobuf::Descriptor* request_desc = nullptr;
            const google::protobuf::Descriptor* response_desc = nullptr;
            auto pool = create_protobuf_descriptors(request.service_name, request.method_name, 
                                                  method_sig, parser.get(), request_desc, response_desc);
            if (!pool) {
                response.success = false;
                response.status = CallResponse::Status::INTERNAL_ERROR;
                response.error_message = "Failed to create protobuf descriptors";
                auto end_time = std::chrono::high_resolution_clock::now();
                response.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
                return response;
            }
            
            // Get the factory that matches this pool
            std::shared_ptr<google::protobuf::DynamicMessageFactory> factory;
            {
                std::lock_guard<std::mutex> lock(descriptor_mutex_);
                factory = service_factories_[request.service_name];
            }
            
            // Create request message
            std::unique_ptr<google::protobuf::Message> request_msg(factory->GetPrototype(request_desc)->New());
            LOG(INFO) << "Populating request message with params: " << params.dump(2);
            if (!populate_message(request_msg.get(), params)) {
                response.success = false;
                response.status = CallResponse::Status::INVALID_PARAMETERS;
                response.error_message = "Failed to populate request message";
                auto end_time = std::chrono::high_resolution_clock::now();
                response.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
                return response;
            }
            LOG(INFO) << "Request message populated successfully";
            
            // Log the actual protobuf message content
            LOG(INFO) << "Protobuf request message content: " << request_msg->DebugString();
            
            // Make the gRPC call
            auto grpc_response = make_grpc_call(request.service_name, request.method_name, 
                                              endpoint.address, request_msg.get(), response_desc,
                                              request.timeout);
            
            response.success = grpc_response.success;
            response.status = grpc_response.status;
            response.error_message = grpc_response.error_message;
            response.result_json = grpc_response.result_json;
            
        } catch (const std::exception& e) {
            LOG(ERROR) << "Exception during dynamic call: " << e.what();
            response.success = false;
            response.status = CallResponse::Status::INTERNAL_ERROR;
            response.error_message = "Exception: " + std::string(e.what());
        }
        
        auto end_time = std::chrono::high_resolution_clock::now();
        response.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
        
        return response;
    }
    
    std::string generate_example_parameters(
        const std::string& service_name,
        const std::string& method_name,
        const std::string& ifex_schema) override {
        
        try {
            auto parser = Parser::create(ifex_schema);
            
            if (!parser->has_method(method_name)) {
                return "{}";
            }
            
            auto method_sig = parser->get_method_signature(method_name);
            
            json params = json::object();
            for (const auto& param : method_sig.input_parameters) {
                // Generate example values based on type
                switch (param.type) {
                    case Type::BOOLEAN:
                        params[param.name] = true;
                        break;
                    case Type::INT32:
                        params[param.name] = 42;
                        break;
                    case Type::DOUBLE:
                        params[param.name] = 3.14;
                        break;
                    case Type::STRING:
                        params[param.name] = "example_" + param.name;
                        break;
                    default:
                        params[param.name] = nullptr;
                }
            }
            
            return params.dump(2);
            
        } catch (const std::exception& e) {
            LOG(ERROR) << "Failed to generate example parameters: " << e.what();
            return "{}";
        }
        return "{}";
    }
    
private:
    // Helper method to set field type based on IFEX parameter
    void set_field_type(google::protobuf::FieldDescriptorProto* field, const Parameter& param, const std::string& package_name) {
        switch (param.type) {
            case Type::BOOLEAN:
                field->set_type(google::protobuf::FieldDescriptorProto::TYPE_BOOL);
                break;
            case Type::INT8:
                field->set_type(google::protobuf::FieldDescriptorProto::TYPE_INT32);  // Proto3 doesn't have int8
                break;
            case Type::INT16:
                field->set_type(google::protobuf::FieldDescriptorProto::TYPE_INT32);  // Proto3 doesn't have int16
                break;
            case Type::INT32:
                field->set_type(google::protobuf::FieldDescriptorProto::TYPE_INT32);
                break;
            case Type::INT64:
                field->set_type(google::protobuf::FieldDescriptorProto::TYPE_INT64);
                break;
            case Type::UINT8:
                field->set_type(google::protobuf::FieldDescriptorProto::TYPE_UINT32);  // Proto3 doesn't have uint8
                break;
            case Type::UINT16:
                field->set_type(google::protobuf::FieldDescriptorProto::TYPE_UINT32);  // Proto3 doesn't have uint16
                break;
            case Type::UINT32:
                field->set_type(google::protobuf::FieldDescriptorProto::TYPE_UINT32);
                break;
            case Type::UINT64:
                field->set_type(google::protobuf::FieldDescriptorProto::TYPE_UINT64);
                break;
            case Type::FLOAT:
                field->set_type(google::protobuf::FieldDescriptorProto::TYPE_FLOAT);
                break;
            case Type::DOUBLE:
                field->set_type(google::protobuf::FieldDescriptorProto::TYPE_DOUBLE);
                break;
            case Type::STRING:
                field->set_type(google::protobuf::FieldDescriptorProto::TYPE_STRING);
                break;
            case Type::BYTES:
                field->set_type(google::protobuf::FieldDescriptorProto::TYPE_BYTES);
                break;
            case Type::STRUCT: {
                // For struct types, we need to reference the message type
                field->set_type(google::protobuf::FieldDescriptorProto::TYPE_MESSAGE);
                // Strip array suffix if present
                std::string type_name = param.type_name;
                if (type_name.size() > 2 && type_name.substr(type_name.size() - 2) == "[]") {
                    type_name = type_name.substr(0, type_name.size() - 2);
                }
                // For message types in the same file, we need to use the fully qualified name
                field->set_type_name("." + package_name + "." + type_name);
                LOG(INFO) << "Created MESSAGE field '" << param.name << "' with type_name: " << "." + package_name + "." + type_name;
                break;
            }
            case Type::ARRAY:
                // ARRAY type shouldn't appear here - arrays are handled via is_array flag
                LOG(ERROR) << "Type::ARRAY for '" << param.name << "' - this should be handled via is_array flag";
                field->set_type(google::protobuf::FieldDescriptorProto::TYPE_STRING);
                break;
            case Type::ENUM: {
                // For enum types, reference the enum type
                field->set_type(google::protobuf::FieldDescriptorProto::TYPE_ENUM);
                // Strip array suffix if present
                std::string enum_type_name = param.type_name;
                if (enum_type_name.size() > 2 && enum_type_name.substr(enum_type_name.size() - 2) == "[]") {
                    enum_type_name = enum_type_name.substr(0, enum_type_name.size() - 2);
                }
                // For enum types in the same file, we need to use the fully qualified name
                field->set_type_name("." + package_name + "." + enum_type_name);
                LOG(INFO) << "Created ENUM field '" << param.name << "' with type: " << "." + package_name + "." + enum_type_name;
                break;
            }
            default:
                LOG(WARNING) << "Unknown parameter type for '" << param.name << "', defaulting to STRING";
                field->set_type(google::protobuf::FieldDescriptorProto::TYPE_STRING);
        }
    }

    // Create protobuf descriptors from IFEX method signature
    // Returns the pool to use (might be different from input if rebuilt)
    std::shared_ptr<google::protobuf::DescriptorPool> create_protobuf_descriptors(
                                   const std::string& service_name,
                                   const std::string& method_name,
                                   const MethodSignature& method_sig,
                                   const Parser* parser,
                                   const google::protobuf::Descriptor*& request_desc,
                                   const google::protobuf::Descriptor*& response_desc) {
        
        LOG(INFO) << "create_protobuf_descriptors: Starting for " << service_name << "." << method_name;
        std::string proto_filename = service_name + "_service.proto";
        
        // Entire descriptor creation process must be atomic
        std::lock_guard<std::mutex> lock(descriptor_mutex_);
        LOG(INFO) << "create_protobuf_descriptors: Acquired descriptor mutex";
        
        // Get or create the pool for this service
        std::shared_ptr<google::protobuf::DescriptorPool> pool;
        if (service_pools_.find(service_name) == service_pools_.end()) {
            LOG(INFO) << "create_protobuf_descriptors: Creating new pool for service " << service_name;
            service_pools_[service_name] = std::make_shared<google::protobuf::DescriptorPool>();
            service_factories_[service_name] = std::make_shared<google::protobuf::DynamicMessageFactory>(
                service_pools_[service_name].get());
        }
        pool = service_pools_[service_name];
        
        // Check if this proto is already in the pool
        const google::protobuf::FileDescriptor* existing_file = pool->FindFileByName(proto_filename);
        if (existing_file) {
            // Check if this method's messages already exist
            request_desc = existing_file->FindMessageTypeByName(method_name + "_request");
            response_desc = existing_file->FindMessageTypeByName(method_name + "_response");
            
            if (request_desc && response_desc) {
                LOG(INFO) << "Reusing existing protobuf descriptors for " << proto_filename;
                return pool;
            }
            
            // Method doesn't exist in the file - this suggests the service has been updated
            // Clear the pool and rebuild to avoid conflicts
            LOG(INFO) << "New method detected, rebuilding service descriptor pool";
            service_pools_[service_name] = std::make_shared<google::protobuf::DescriptorPool>();
            service_factories_[service_name] = std::make_shared<google::protobuf::DynamicMessageFactory>(
                service_pools_[service_name].get());
            pool = service_pools_[service_name];
        }
        
        // Create new proto file descriptor
        google::protobuf::FileDescriptorProto file_proto;
        file_proto.set_name(proto_filename);
        file_proto.set_package("swdv." + service_name);
        file_proto.set_syntax("proto3");
        
        // First, create all struct definitions that might be needed
        if (parser) {
            LOG(INFO) << "create_protobuf_descriptors: Getting all struct definitions";
            auto all_structs = parser->get_all_struct_definitions();
            LOG(INFO) << "create_protobuf_descriptors: Found " << all_structs.size() << " struct definitions";
            for (const auto& struct_def : all_structs) {
                LOG(INFO) << "create_protobuf_descriptors: Creating struct " << struct_def.name;
                auto* struct_msg = file_proto.add_message_type();
                struct_msg->set_name(struct_def.name);
                
                int field_num = 1;
                for (const auto& member : struct_def.members) {
                    auto* field = struct_msg->add_field();
                    field->set_name(member.name);
                    field->set_number(field_num++);
                    
                    // Set label based on whether it's an array
                    if (member.is_array) {
                        field->set_label(google::protobuf::FieldDescriptorProto::LABEL_REPEATED);
                    } else {
                        field->set_label(google::protobuf::FieldDescriptorProto::LABEL_OPTIONAL);
                    }
                    
                    // Map IFEX types to protobuf types
                    set_field_type(field, member, file_proto.package());
                }
            }
            
            // Also create enum definitions
            LOG(INFO) << "create_protobuf_descriptors: Getting all enum definitions";
            auto all_enums = parser->get_all_enum_definitions();
            LOG(INFO) << "create_protobuf_descriptors: Found " << all_enums.size() << " enum definitions";
            for (const auto& enum_def : all_enums) {
                LOG(INFO) << "create_protobuf_descriptors: Creating enum " << enum_def.name;
                auto* enum_proto = file_proto.add_enum_type();
                enum_proto->set_name(enum_def.name);
                
                for (const auto& option : enum_def.options) {
                    auto* value = enum_proto->add_value();
                    value->set_name(option.name);
                    value->set_number(option.value);
                }
            }
        }
        
        // Create request message
        auto* request_msg = file_proto.add_message_type();
        request_msg->set_name(method_name + "_request");
        
        // Add fields for each input parameter
        int field_num = 1;
        for (const auto& param : method_sig.input_parameters) {
            LOG(INFO) << "create_protobuf_descriptors: Creating input field '" << param.name 
                      << "' type: " << static_cast<int>(param.type) 
                      << " is_array: " << param.is_array
                      << " type_name: " << param.type_name;
            auto* field = request_msg->add_field();
            field->set_name(param.name);
            field->set_number(field_num++);
            
            // Set label based on whether it's an array
            if (param.is_array) {
                field->set_label(google::protobuf::FieldDescriptorProto::LABEL_REPEATED);
            } else {
                field->set_label(google::protobuf::FieldDescriptorProto::LABEL_OPTIONAL);
            }
            
            // Map IFEX types to protobuf types
            set_field_type(field, param, file_proto.package());
        }
        
        // Create response message (for now, just a simple structure)
        auto* response_msg = file_proto.add_message_type();
        response_msg->set_name(method_name + "_response");
        
        // Add a result field based on output parameters
        if (!method_sig.output_parameters.empty()) {
            field_num = 1;
            for (const auto& param : method_sig.output_parameters) {
                auto* field = response_msg->add_field();
                field->set_name(param.name);
                field->set_number(field_num++);
                
                // Set label based on whether it's an array
                if (param.is_array) {
                    field->set_label(google::protobuf::FieldDescriptorProto::LABEL_REPEATED);
                } else {
                    field->set_label(google::protobuf::FieldDescriptorProto::LABEL_OPTIONAL);
                }
                
                // Map IFEX types to protobuf types
                set_field_type(field, param, file_proto.package());
            }
        }
        
        // Build the file descriptor
        LOG(INFO) << "create_protobuf_descriptors: Building proto file descriptor";
        const google::protobuf::FileDescriptor* file_desc = pool->BuildFile(file_proto);
        if (!file_desc) {
            LOG(ERROR) << "Failed to build proto file descriptor";
            return nullptr;
        }
        LOG(INFO) << "create_protobuf_descriptors: Successfully built proto file";
        
        request_desc = file_desc->FindMessageTypeByName(method_name + "_request");
        response_desc = file_desc->FindMessageTypeByName(method_name + "_response");
        
        if (request_desc && response_desc) {
            LOG(INFO) << "create_protobuf_descriptors: Found request and response descriptors";
            return pool;
        } else {
            LOG(ERROR) << "create_protobuf_descriptors: Failed to find request/response descriptors";
            return nullptr;
        }
    }
    
    // Populate protobuf message from JSON
    bool populate_message(google::protobuf::Message* message, const json& params) {
        const google::protobuf::Descriptor* desc = message->GetDescriptor();
        const google::protobuf::Reflection* reflection = message->GetReflection();
        
        for (auto it = params.begin(); it != params.end(); ++it) {
            const google::protobuf::FieldDescriptor* field = desc->FindFieldByName(it.key());
            if (!field) {
                LOG(WARNING) << "Field '" << it.key() << "' not found in protobuf descriptor";
                continue;
            }
            
            LOG(INFO) << "Processing field '" << it.key() << "' with cpp_type: " << field->cpp_type() 
                      << " and type: " << field->type() 
                      << " is_repeated: " << field->is_repeated();
            
            try {
                // Handle repeated fields (arrays)
                if (field->is_repeated() && it.value().is_array()) {
                    const auto& array = it.value();
                    for (size_t i = 0; i < array.size(); ++i) {
                        const auto& elem = array[i];
                        switch (field->cpp_type()) {
                            case google::protobuf::FieldDescriptor::CPPTYPE_BOOL:
                                if (elem.is_boolean()) {
                                    reflection->AddBool(message, field, elem.get<bool>());
                                }
                                break;
                            case google::protobuf::FieldDescriptor::CPPTYPE_INT32:
                                if (elem.is_number()) {
                                    reflection->AddInt32(message, field, elem.get<int32_t>());
                                }
                                break;
                            case google::protobuf::FieldDescriptor::CPPTYPE_INT64:
                                if (elem.is_number()) {
                                    reflection->AddInt64(message, field, elem.get<int64_t>());
                                }
                                break;
                            case google::protobuf::FieldDescriptor::CPPTYPE_UINT32:
                                if (elem.is_number()) {
                                    reflection->AddUInt32(message, field, elem.get<uint32_t>());
                                }
                                break;
                            case google::protobuf::FieldDescriptor::CPPTYPE_UINT64:
                                if (elem.is_number()) {
                                    reflection->AddUInt64(message, field, elem.get<uint64_t>());
                                }
                                break;
                            case google::protobuf::FieldDescriptor::CPPTYPE_FLOAT:
                                if (elem.is_number()) {
                                    reflection->AddFloat(message, field, elem.get<float>());
                                }
                                break;
                            case google::protobuf::FieldDescriptor::CPPTYPE_DOUBLE:
                                if (elem.is_number()) {
                                    reflection->AddDouble(message, field, elem.get<double>());
                                }
                                break;
                            case google::protobuf::FieldDescriptor::CPPTYPE_STRING:
                                if (elem.is_string()) {
                                    reflection->AddString(message, field, elem.get<std::string>());
                                } else {
                                    reflection->AddString(message, field, elem.dump());
                                }
                                break;
                            case google::protobuf::FieldDescriptor::CPPTYPE_ENUM:
                                if (elem.is_number()) {
                                    const google::protobuf::EnumDescriptor* enum_desc = field->enum_type();
                                    const google::protobuf::EnumValueDescriptor* enum_value = 
                                        enum_desc->FindValueByNumber(elem.get<int>());
                                    if (enum_value) {
                                        reflection->AddEnum(message, field, enum_value);
                                    }
                                }
                                break;
                            case google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE:
                                if (elem.is_object()) {
                                    google::protobuf::Message* sub_message = 
                                        reflection->AddMessage(message, field);
                                    if (!populate_message(sub_message, elem)) {
                                        LOG(ERROR) << "Failed to populate array element for field: " << it.key();
                                    }
                                }
                                break;
                            default:
                                LOG(WARNING) << "Unsupported array element type for field: " << it.key();
                        }
                    }
                    continue;  // Skip to next field
                }
                
                // Handle singular fields
                switch (field->cpp_type()) {
                    case google::protobuf::FieldDescriptor::CPPTYPE_BOOL:
                        if (it.value().is_boolean()) {
                            reflection->SetBool(message, field, it.value().get<bool>());
                        }
                        break;
                        
                    case google::protobuf::FieldDescriptor::CPPTYPE_INT32:
                        if (it.value().is_number()) {
                            reflection->SetInt32(message, field, it.value().get<int32_t>());
                        }
                        break;
                        
                    case google::protobuf::FieldDescriptor::CPPTYPE_INT64:
                        if (it.value().is_number()) {
                            reflection->SetInt64(message, field, it.value().get<int64_t>());
                        }
                        break;
                        
                    case google::protobuf::FieldDescriptor::CPPTYPE_UINT32:
                        if (it.value().is_number()) {
                            reflection->SetUInt32(message, field, it.value().get<uint32_t>());
                        }
                        break;
                        
                    case google::protobuf::FieldDescriptor::CPPTYPE_UINT64:
                        if (it.value().is_number()) {
                            reflection->SetUInt64(message, field, it.value().get<uint64_t>());
                        }
                        break;
                        
                    case google::protobuf::FieldDescriptor::CPPTYPE_FLOAT:
                        if (it.value().is_number()) {
                            reflection->SetFloat(message, field, it.value().get<float>());
                        }
                        break;
                        
                    case google::protobuf::FieldDescriptor::CPPTYPE_DOUBLE:
                        if (it.value().is_number()) {
                            reflection->SetDouble(message, field, it.value().get<double>());
                        }
                        break;
                        
                    case google::protobuf::FieldDescriptor::CPPTYPE_STRING:
                        if (field->type() == google::protobuf::FieldDescriptor::TYPE_BYTES) {
                            // Handle bytes type
                            if (it.value().is_string()) {
                                reflection->SetString(message, field, it.value().get<std::string>());
                            }
                        } else {
                            // Handle string type
                            if (it.value().is_string()) {
                                reflection->SetString(message, field, it.value().get<std::string>());
                            } else {
                                // Convert to string
                                reflection->SetString(message, field, it.value().dump());
                            }
                        }
                        break;
                        
                    case google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE:
                        if (it.value().is_object()) {
                            // Handle nested message
                            LOG(INFO) << "Populating nested message field: " << it.key();
                            google::protobuf::Message* sub_message = 
                                reflection->MutableMessage(message, field);
                            if (!populate_message(sub_message, it.value())) {
                                LOG(ERROR) << "Failed to populate nested message: " << it.key();
                                return false;
                            }
                            LOG(INFO) << "Successfully populated nested message: " << it.key();
                        }
                        break;
                        
                    case google::protobuf::FieldDescriptor::CPPTYPE_ENUM:
                        if (it.value().is_number()) {
                            const google::protobuf::EnumDescriptor* enum_desc = field->enum_type();
                            const google::protobuf::EnumValueDescriptor* enum_value = 
                                enum_desc->FindValueByNumber(it.value().get<int>());
                            if (enum_value) {
                                reflection->SetEnum(message, field, enum_value);
                            } else {
                                LOG(WARNING) << "Invalid enum value " << it.value().get<int>() 
                                           << " for field: " << it.key();
                            }
                        }
                        break;
                        
                    default:
                        LOG(WARNING) << "Unsupported field type for field: " << it.key();
                }
            } catch (const std::exception& e) {
                LOG(ERROR) << "Failed to set field " << it.key() << ": " << e.what();
                return false;
            }
        }
        
        return true;
    }
    
    // Make the actual gRPC call
    CallResponse make_grpc_call(const std::string& service_name,
                               const std::string& method_name,
                               const std::string& endpoint,
                               const google::protobuf::Message* request,
                               const google::protobuf::Descriptor* response_desc,
                               std::chrono::milliseconds timeout) {
        CallResponse response;
        
        try {
            // Get or create channel
            std::shared_ptr<grpc::Channel> channel;
            {
                std::lock_guard<std::mutex> lock(channel_mutex_);
                auto it = channel_cache_.find(endpoint);
                if (it != channel_cache_.end()) {
                    channel = it->second;
                } else {
                    channel = grpc::CreateChannel(endpoint, grpc::InsecureChannelCredentials());
                    channel_cache_[endpoint] = channel;
                }
            }
            
            // Serialize request
            std::string serialized_request;
            if (!request->SerializeToString(&serialized_request)) {
                response.success = false;
                response.status = CallResponse::Status::INTERNAL_ERROR;
                response.error_message = "Failed to serialize request";
                return response;
            }
            LOG(INFO) << "Serialized request size: " << serialized_request.size() << " bytes";
            
            // Create generic stub and make the call
            grpc::GenericStub stub(channel);
            grpc::ClientContext context;
            context.set_deadline(std::chrono::system_clock::now() + timeout);
            
            // Construct method name following the pattern
            std::string full_method_name = "/swdv." + service_name + "." + method_name + "_service/" + method_name;
            LOG(INFO) << "Calling gRPC method: " << full_method_name;
            
            // Convert to ByteBuffer
            grpc::Slice request_slice(serialized_request);
            grpc::ByteBuffer request_buf(&request_slice, 1);
            
            // Make the call
            grpc::CompletionQueue cq;
            std::unique_ptr<grpc::ClientAsyncResponseReader<grpc::ByteBuffer>> response_reader(
                stub.PrepareUnaryCall(&context, full_method_name, request_buf, &cq)
            );
            response_reader->StartCall();
            
            grpc::ByteBuffer response_buffer;
            grpc::Status status;
            response_reader->Finish(&response_buffer, &status, (void*)1);
            
            void* tag;
            bool ok;
            cq.Next(&tag, &ok);
            
            if (status.ok()) {
                LOG(INFO) << "gRPC call succeeded";
                // Convert response back to string
                std::vector<grpc::Slice> slices;
                response_buffer.Dump(&slices);
                std::string serialized_response;
                for (const auto& slice : slices) {
                    serialized_response.append(reinterpret_cast<const char*>(slice.begin()), slice.size());
                }
                
                // Parse response protobuf and convert to JSON
                try {
                    // Get the factory for this service
                    google::protobuf::DynamicMessageFactory* factory = nullptr;
                    {
                        std::lock_guard<std::mutex> lock(descriptor_mutex_);
                        factory = service_factories_[service_name].get();
                    }
                    
                    if (!factory || !response_desc) {
                        response.success = false;
                        response.status = CallResponse::Status::INTERNAL_ERROR;
                        response.error_message = "Missing factory or response descriptor";
                        return response;
                    }
                    
                    // Create response message
                    std::unique_ptr<google::protobuf::Message> response_msg(
                        factory->GetPrototype(response_desc)->New());
                    
                    // Parse the serialized response
                    if (!response_msg->ParseFromString(serialized_response)) {
                        response.success = false;
                        response.status = CallResponse::Status::INTERNAL_ERROR;
                        response.error_message = "Failed to parse response protobuf";
                        return response;
                    }
                    
                    // Convert to JSON
                    google::protobuf::util::JsonPrintOptions options;
                    options.add_whitespace = false;
                    options.always_print_primitive_fields = true;
                    options.preserve_proto_field_names = true;
                    options.always_print_enums_as_ints = true;
                    
                    std::string json_output;
                    auto json_status = google::protobuf::util::MessageToJsonString(
                        *response_msg, &json_output, options);
                    
                    if (json_status.ok()) {
                        response.success = true;
                        response.status = CallResponse::Status::SUCCESS;
                        response.result_json = json_output;
                        LOG(INFO) << "Converted response to JSON: " << json_output;
                    } else {
                        response.success = false;
                        response.status = CallResponse::Status::INTERNAL_ERROR;
                        response.error_message = "Failed to convert response to JSON: " + 
                                               std::string(json_status.message());
                    }
                } catch (const std::exception& e) {
                    response.success = false;
                    response.status = CallResponse::Status::INTERNAL_ERROR;
                    response.error_message = "Exception parsing response: " + std::string(e.what());
                }
                
            } else {
                LOG(ERROR) << "gRPC call failed with status: " << status.error_code() 
                          << " - " << status.error_message()
                          << " for method: " << full_method_name
                          << " to endpoint: " << endpoint
                          << ", details: " << status.error_details();
                response.success = false;
                // Map gRPC status codes to our status enum
                switch (status.error_code()) {
                    case grpc::StatusCode::DEADLINE_EXCEEDED:
                        response.status = CallResponse::Status::TIMEOUT;
                        break;
                    case grpc::StatusCode::UNAVAILABLE:
                        response.status = CallResponse::Status::SERVICE_UNAVAILABLE;
                        break;
                    case grpc::StatusCode::NOT_FOUND:
                        response.status = CallResponse::Status::METHOD_NOT_FOUND;
                        break;
                    case grpc::StatusCode::INVALID_ARGUMENT:
                        response.status = CallResponse::Status::INVALID_PARAMETERS;
                        break;
                    default:
                        response.status = CallResponse::Status::INTERNAL_ERROR;
                        break;
                }
                response.error_message = "gRPC error: " + std::to_string(status.error_code()) + 
                                       " - " + status.error_message();
            }
            
        } catch (const std::exception& e) {
            response.success = false;
            response.status = CallResponse::Status::INTERNAL_ERROR;
            response.error_message = "Exception: " + std::string(e.what());
        }
        
        return response;
    }
};

// Factory method
std::unique_ptr<DynamicCaller> DynamicCaller::create(const std::string& discovery_endpoint) {
    return std::make_unique<DynamicCallerImpl>(discovery_endpoint);
}

} // namespace ifex