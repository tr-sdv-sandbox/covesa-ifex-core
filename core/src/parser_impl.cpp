#include "ifex/parser.hpp"
#include <yaml-cpp/yaml.h>
#include <fstream>
#include <sstream>
#include <glog/logging.h>
#include <algorithm>
#include <nlohmann/json.hpp>
#include <unordered_map>

namespace ifex {

class ParserImpl : public Parser {
public:
    explicit ParserImpl(const std::string& ifex_yaml) 
        : ifex_yaml_(ifex_yaml) {
        try {
            root_ = YAML::Load(ifex_yaml);
            parse_service();
        } catch (const YAML::Exception& e) {
            LOG(ERROR) << "Failed to parse IFEX YAML: " << e.what();
            throw std::runtime_error("Invalid IFEX YAML: " + std::string(e.what()));
        }
    }
    
    std::string get_service_name() const override {
        return service_name_;
    }
    
    std::string get_service_version() const override {
        return std::to_string(major_version_) + "." + std::to_string(minor_version_);
    }
    
    std::string get_service_description() const override {
        return service_description_;
    }
    
    std::vector<std::string> get_method_names() const override {
        std::vector<std::string> names;
        for (const auto& method : methods_) {
            names.push_back(method.method_name);
        }
        return names;
    }
    
    bool has_method(const std::string& method_name) const override {
        for (const auto& method : methods_) {
            if (method.method_name == method_name) {
                return true;
            }
        }
        return false;
    }
    
    MethodSignature get_method_signature(const std::string& method_name) const override {
        for (const auto& method : methods_) {
            if (method.method_name == method_name) {
                return method;
            }
        }
        throw std::runtime_error("Method not found: " + method_name);
    }
    
    std::vector<MethodSignature> get_all_methods() const override {
        return methods_;
    }
    
    ValidationResult validate_parameters(
        const std::string& method_name,
        const std::string& parameters_json) const override {
        
        ValidationResult result;
        
        // Find the method
        MethodSignature method_sig;
        bool found = false;
        for (const auto& method : methods_) {
            if (method.method_name == method_name) {
                method_sig = method;
                found = true;
                break;
            }
        }
        
        if (!found) {
            result.valid = false;
            result.errors.push_back("Method not found: " + method_name);
            return result;
        }
        
        // Parse JSON parameters
        nlohmann::json params;
        try {
            params = nlohmann::json::parse(parameters_json);
        } catch (const nlohmann::json::parse_error& e) {
            result.valid = false;
            result.errors.push_back("Invalid JSON: " + std::string(e.what()));
            return result;
        }
        
        // Check if params is an object
        if (!params.is_object()) {
            result.valid = false;
            result.errors.push_back("Parameters must be a JSON object");
            return result;
        }
        
        // Validate each required parameter
        for (const auto& param : method_sig.input_parameters) {
            if (!param.is_optional && !params.contains(param.name)) {
                result.valid = false;
                result.errors.push_back("Missing required parameter: " + param.name);
                continue;
            }
            
            if (params.contains(param.name)) {
                // Validate type
                const auto& value = params[param.name];
                if (!validate_parameter_type(value, param)) {
                    result.valid = false;
                    std::string expected_type = type_to_string(param.type);
                    if (param.is_array) {
                        expected_type += " array";
                    }
                    result.errors.push_back("Invalid type for parameter '" + param.name + 
                                          "': expected " + expected_type);
                }
            }
        }
        
        // Check for unexpected parameters
        for (auto it = params.begin(); it != params.end(); ++it) {
            bool found_param = false;
            for (const auto& param : method_sig.input_parameters) {
                if (param.name == it.key()) {
                    found_param = true;
                    break;
                }
            }
            if (!found_param) {
                result.valid = false;
                result.errors.push_back("Unexpected parameter: " + it.key());
            }
        }
        
        return result;
    }
    
    std::vector<std::string> get_struct_names() const override {
        return struct_names_;
    }
    
    std::vector<std::string> get_enum_names() const override {
        return enum_names_;
    }
    
    std::optional<StructDefinition> get_struct_definition(const std::string& struct_name) const override {
        auto it = struct_definitions_.find(struct_name);
        if (it != struct_definitions_.end()) {
            return it->second;
        }
        return std::nullopt;
    }
    
    std::optional<EnumDefinition> get_enum_definition(const std::string& enum_name) const override {
        auto it = enum_definitions_.find(enum_name);
        if (it != enum_definitions_.end()) {
            return it->second;
        }
        return std::nullopt;
    }
    
    std::vector<StructDefinition> get_all_struct_definitions() const override {
        std::vector<StructDefinition> result;
        for (const auto& [name, def] : struct_definitions_) {
            result.push_back(def);
        }
        return result;
    }
    
    std::vector<EnumDefinition> get_all_enum_definitions() const override {
        std::vector<EnumDefinition> result;
        for (const auto& [name, def] : enum_definitions_) {
            result.push_back(def);
        }
        return result;
    }
    
    std::string get_ifex_yaml() const override {
        return ifex_yaml_;
    }
    
private:
    void parse_service() {
        service_name_ = root_["name"].as<std::string>();
        major_version_ = root_["major_version"].as<int>(1);
        minor_version_ = root_["minor_version"].as<int>(0);
        service_description_ = root_["description"].as<std::string>("");
        
        // Parse namespaces
        if (root_["namespaces"]) {
            for (const auto& ns : root_["namespaces"]) {
                parse_namespace(ns);
            }
        }
    }
    
    void parse_namespace(const YAML::Node& ns) {
        std::string namespace_name = ns["name"].as<std::string>();
        
        // Parse enumerations
        if (ns["enumerations"]) {
            for (const auto& enum_node : ns["enumerations"]) {
                EnumDefinition enum_def;
                enum_def.name = enum_node["name"].as<std::string>();
                enum_def.description = enum_node["description"].as<std::string>("");
                
                // Parse datatype
                std::string datatype_str = enum_node["datatype"].as<std::string>();
                enum_def.datatype = parse_type(datatype_str);
                
                // Parse options
                if (enum_node["options"]) {
                    for (const auto& option_node : enum_node["options"]) {
                        EnumOption option;
                        option.name = option_node["name"].as<std::string>();
                        option.description = option_node["description"].as<std::string>("");
                        option.value = option_node["value"].as<int64_t>();
                        enum_def.options.push_back(option);
                    }
                }
                
                enum_names_.push_back(enum_def.name);
                enum_definitions_[enum_def.name] = enum_def;
            }
        }
        
        // Parse structs
        if (ns["structs"]) {
            for (const auto& struct_node : ns["structs"]) {
                StructDefinition struct_def;
                struct_def.name = struct_node["name"].as<std::string>();
                struct_def.description = struct_node["description"].as<std::string>("");
                
                // Parse members
                if (struct_node["members"]) {
                    for (const auto& member_node : struct_node["members"]) {
                        Parameter member = parse_parameter(member_node);
                        struct_def.members.push_back(member);
                    }
                }
                
                struct_names_.push_back(struct_def.name);
                struct_definitions_[struct_def.name] = struct_def;
            }
        }
        
        // Parse methods
        if (ns["methods"]) {
            for (const auto& method_node : ns["methods"]) {
                MethodSignature method;
                method.service_name = service_name_;
                method.namespace_name = namespace_name;
                method.method_name = method_node["name"].as<std::string>();
                method.description = method_node["description"].as<std::string>("");
                
                // Parse input parameters
                if (method_node["input"]) {
                    for (const auto& param_node : method_node["input"]) {
                        Parameter param = parse_parameter(param_node);
                        method.input_parameters.push_back(param);
                    }
                }
                
                // Parse output parameters
                if (method_node["output"]) {
                    for (const auto& param_node : method_node["output"]) {
                        Parameter param = parse_parameter(param_node);
                        method.output_parameters.push_back(param);
                    }
                }
                
                // Parse extensions (metadata)
                for (auto it = method_node.begin(); it != method_node.end(); ++it) {
                    std::string key = it->first.as<std::string>();
                    if (key.rfind("x-", 0) == 0) {  // Check if starts with "x-"
                        // Store extension as metadata
                        // For now, just store as string representation
                        YAML::Emitter emitter;
                        emitter << it->second;
                        method.metadata[key] = emitter.c_str();
                    }
                }
                
                methods_.push_back(method);
            }
        }
    }
    
    Parameter parse_parameter(const YAML::Node& param_node) {
        Parameter param;
        param.name = param_node["name"].as<std::string>();
        param.description = param_node["description"].as<std::string>("");
        
        // Support both 'datatype' (standard) and 'type' (legacy/simple format)
        std::string datatype;
        if (param_node["datatype"]) {
            datatype = param_node["datatype"].as<std::string>();
        } else if (param_node["type"]) {
            datatype = param_node["type"].as<std::string>();
        } else {
            throw std::runtime_error("Parameter missing 'datatype' field: " + param.name);
        }
        param.type_name = datatype;
        
        // Check if it's an array type
        if (datatype.size() > 2 && datatype.substr(datatype.size() - 2) == "[]") {
            param.is_array = true;
            datatype = datatype.substr(0, datatype.length() - 2);
        }
        
        // Parse type
        param.type = parse_type(datatype);
        
        // Parse optional/mandatory
        if (param_node["mandatory"]) {
            param.is_optional = !param_node["mandatory"].as<bool>();
        }
        
        // Parse default value
        if (param_node["default"]) {
            YAML::Emitter emitter;
            emitter << param_node["default"];
            param.default_value = emitter.c_str();
        }
        
        // Parse constraints
        if (param_node["constraints"]) {
            auto constraints = param_node["constraints"];
            if (constraints["min"]) {
                param.constraints.min = constraints["min"].as<double>();
            }
            if (constraints["max"]) {
                param.constraints.max = constraints["max"].as<double>();
            }
            if (constraints["min_items"]) {
                param.constraints.min_items = constraints["min_items"].as<size_t>();
            }
            if (constraints["max_items"]) {
                param.constraints.max_items = constraints["max_items"].as<size_t>();
            }
        }
        
        return param;
    }
    
    Type parse_type(const std::string& type_str) {
        if (type_str == "uint8") return Type::UINT8;
        if (type_str == "uint16") return Type::UINT16;
        if (type_str == "uint32") return Type::UINT32;
        if (type_str == "uint64") return Type::UINT64;
        if (type_str == "int8") return Type::INT8;
        if (type_str == "int16") return Type::INT16;
        if (type_str == "int32") return Type::INT32;
        if (type_str == "int64") return Type::INT64;
        if (type_str == "float") return Type::FLOAT;
        if (type_str == "double") return Type::DOUBLE;
        if (type_str == "boolean") return Type::BOOLEAN;
        if (type_str == "string") return Type::STRING;
        if (type_str == "bytes") return Type::BYTES;
        
        // Check if it's a known enum or struct
        if (std::find(enum_names_.begin(), enum_names_.end(), type_str) != enum_names_.end()) {
            return Type::ENUM;
        }
        if (std::find(struct_names_.begin(), struct_names_.end(), type_str) != struct_names_.end()) {
            return Type::STRUCT;
        }
        
        // Default to string for unknown types
        return Type::STRING;
    }
    
    bool validate_parameter_type(const nlohmann::json& value, const Parameter& param) const {
        // Handle array types first
        if (param.is_array) {
            if (!value.is_array()) {
                return false;
            }
            // For arrays, we could validate each element type, but for now just check it's an array
            return true;
        }
        
        // Handle non-array types
        switch (param.type) {
            case Type::BOOLEAN:
                return value.is_boolean();
            case Type::UINT8:
            case Type::UINT16:
            case Type::UINT32:
            case Type::UINT64:
            case Type::INT8:
            case Type::INT16:
            case Type::INT32:
            case Type::INT64:
                return value.is_number_integer();
            case Type::FLOAT:
            case Type::DOUBLE:
                return value.is_number();
            case Type::STRING:
                return value.is_string();
            case Type::BYTES:
                return value.is_string(); // Base64 encoded
            case Type::STRUCT:
                return value.is_object();
            case Type::ARRAY:
                return value.is_array();
            case Type::ENUM:
                return value.is_number_integer(); // Enum values are integers in our test
            default:
                return false;
        }
    }
    
    std::string type_to_string(Type type) const {
        switch (type) {
            case Type::UINT8: return "uint8";
            case Type::UINT16: return "uint16";
            case Type::UINT32: return "uint32";
            case Type::UINT64: return "uint64";
            case Type::INT8: return "int8";
            case Type::INT16: return "int16";
            case Type::INT32: return "int32";
            case Type::INT64: return "int64";
            case Type::FLOAT: return "float";
            case Type::DOUBLE: return "double";
            case Type::BOOLEAN: return "boolean";
            case Type::STRING: return "string";
            case Type::BYTES: return "bytes";
            case Type::STRUCT: return "struct";
            case Type::ARRAY: return "array";
            case Type::ENUM: return "enum";
            default: return "unknown";
        }
    }
    
private:
    std::string ifex_yaml_;
    YAML::Node root_;
    
    // Parsed data
    std::string service_name_;
    int major_version_ = 1;
    int minor_version_ = 0;
    std::string service_description_;
    std::vector<MethodSignature> methods_;
    std::vector<std::string> struct_names_;
    std::vector<std::string> enum_names_;
    std::unordered_map<std::string, StructDefinition> struct_definitions_;
    std::unordered_map<std::string, EnumDefinition> enum_definitions_;
};

// Factory methods
std::unique_ptr<Parser> Parser::create(const std::string& ifex_yaml) {
    return std::make_unique<ParserImpl>(ifex_yaml);
}

std::unique_ptr<Parser> Parser::create_from_file(const std::string& file_path) {
    std::ifstream file(file_path);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open file: " + file_path);
    }
    
    std::stringstream buffer;
    buffer << file.rdbuf();
    return create(buffer.str());
}

} // namespace ifex