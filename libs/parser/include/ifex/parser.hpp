#pragma once

#include "ifex/types.hpp"
#include <memory>
#include <string>
#include <vector>

namespace ifex {

class Parser {
public:
    virtual ~Parser() = default;
    
    // Create parser from IFEX YAML content
    static std::unique_ptr<Parser> create(const std::string& ifex_yaml);
    
    // Create parser from file
    static std::unique_ptr<Parser> create_from_file(const std::string& file_path);
    
    // Service metadata
    virtual std::string get_service_name() const = 0;
    virtual std::string get_service_version() const = 0;
    virtual std::string get_service_description() const = 0;
    
    // Method discovery
    virtual std::vector<std::string> get_method_names() const = 0;
    virtual bool has_method(const std::string& method_name) const = 0;
    virtual MethodSignature get_method_signature(const std::string& method_name) const = 0;
    virtual std::vector<MethodSignature> get_all_methods() const = 0;
    
    // Parameter validation
    virtual ValidationResult validate_parameters(
        const std::string& method_name,
        const std::string& parameters_json) const = 0;
    
    // Type information
    virtual std::vector<std::string> get_struct_names() const = 0;
    virtual std::vector<std::string> get_enum_names() const = 0;
    
    // Get struct and enum definitions
    virtual std::optional<StructDefinition> get_struct_definition(const std::string& struct_name) const = 0;
    virtual std::optional<EnumDefinition> get_enum_definition(const std::string& enum_name) const = 0;
    virtual std::vector<StructDefinition> get_all_struct_definitions() const = 0;
    virtual std::vector<EnumDefinition> get_all_enum_definitions() const = 0;
    
    // Raw IFEX access
    virtual std::string get_ifex_yaml() const = 0;
};

} // namespace ifex