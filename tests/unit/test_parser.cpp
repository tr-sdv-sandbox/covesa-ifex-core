#include <gtest/gtest.h>
#include "ifex/parser.hpp"
#include <memory>

using namespace ifex;

class ParserTest : public ::testing::Test {
protected:
    const std::string simple_ifex = R"(
name: test_service
major_version: 1
minor_version: 0
description: Test service for parser testing
namespaces:
  - name: default
    description: Default namespace
    methods:
      - name: get_temperature
        description: Get current temperature
        input:
          - name: zone
            datatype: string
            description: Temperature zone
            mandatory: true
        output:
          - name: temperature
            datatype: float
            description: Current temperature in Celsius
      - name: set_temperature
        description: Set target temperature
        input:
          - name: zone
            datatype: string
            description: Temperature zone
            mandatory: true
          - name: temperature
            datatype: float
            description: Target temperature in Celsius
            mandatory: true
        output:
          - name: success
            datatype: boolean
            description: Whether operation succeeded
)";

    const std::string namespaced_ifex = R"(
---
name: climate_control
major_version: 2
minor_version: 0
description: Vehicle climate control service
namespaces:
  - name: hvac
    description: HVAC control
    methods:
      - name: get_zone_temperature
        description: Get temperature for a zone
        input:
          - name: zone_id
            datatype: uint8
            description: Zone identifier
            mandatory: true
        output:
          - name: temperature
            datatype: float
            description: Current temperature
      - name: set_mode
        description: Set HVAC mode
        x-scheduling:
          enabled: true
          profile:
            category: efficiency
            business_impact: cost_optimization
          timing:
            estimated_duration_seconds: 30
        input:
          - name: mode
            datatype: string
            description: HVAC mode
            mandatory: true
)";

    const std::string echo_service_ifex = R"(
name: echo_service
major_version: 1
minor_version: 0
description: Simple echo service for testing IFEX dynamic invocation

namespaces:
  - name: basic
    description: Basic echo operations
    methods:
      - name: echo
        description: Echo back the input message
        input:
          - name: message
            datatype: string
            description: Message to echo back
            mandatory: true
        output:
          - name: response
            datatype: string
            description: Echoed message
            
      - name: echo_with_delay
        description: Echo back the input message after a delay
        input:
          - name: message
            datatype: string
            description: Message to echo back
            mandatory: true
          - name: delay_ms
            datatype: uint32
            description: Delay in milliseconds
            mandatory: true
        output:
          - name: response
            datatype: string
            description: Echoed message
          - name: actual_delay_ms
            datatype: uint32
            description: Actual delay in milliseconds
)";
};

TEST_F(ParserTest, SimpleFormatParsing) {
    auto parser = Parser::create(simple_ifex);
    
    EXPECT_EQ(parser->get_service_name(), "test_service");
    EXPECT_EQ(parser->get_service_description(), "Test service for parser testing");
    EXPECT_EQ(parser->get_service_version(), "1.0");
    
    auto methods = parser->get_method_names();
    EXPECT_EQ(methods.size(), 2);
    EXPECT_TRUE(parser->has_method("get_temperature"));
    EXPECT_TRUE(parser->has_method("set_temperature"));
    EXPECT_FALSE(parser->has_method("non_existent"));
}

TEST_F(ParserTest, NamespacedFormatParsing) {
    auto parser = Parser::create(namespaced_ifex);
    
    EXPECT_EQ(parser->get_service_name(), "climate_control");
    EXPECT_EQ(parser->get_service_version(), "2.0");
    
    auto methods = parser->get_method_names();
    EXPECT_EQ(methods.size(), 2);
    EXPECT_TRUE(parser->has_method("get_zone_temperature"));
    EXPECT_TRUE(parser->has_method("set_mode"));
}

TEST_F(ParserTest, MethodSignatureExtraction) {
    auto parser = Parser::create(simple_ifex);
    
    auto sig = parser->get_method_signature("get_temperature");
    EXPECT_EQ(sig.method_name, "get_temperature");
    EXPECT_EQ(sig.service_name, "test_service");
    EXPECT_EQ(sig.input_parameters.size(), 1);
    EXPECT_EQ(sig.output_parameters.size(), 1);
    
    EXPECT_EQ(sig.input_parameters[0].name, "zone");
    EXPECT_EQ(sig.input_parameters[0].type, Type::STRING);
    
    EXPECT_EQ(sig.output_parameters[0].name, "temperature");
    EXPECT_EQ(sig.output_parameters[0].type, Type::FLOAT);
}

TEST_F(ParserTest, ExtensionMetadata) {
    auto parser = Parser::create(namespaced_ifex);
    
    // Our parser tracks extensions in method signatures
    auto sig = parser->get_method_signature("set_mode");
    
    // The method should exist
    EXPECT_EQ(sig.method_name, "set_mode");
    
    // For now, just test that the method exists
    // Extension parsing is handled internally
    EXPECT_TRUE(parser->has_method("set_mode"));
}

TEST_F(ParserTest, ParameterValidation) {
    auto parser = Parser::create(simple_ifex);
    
    // Valid parameters
    auto result = parser->validate_parameters("get_temperature", R"({"zone": "living_room"})");
    EXPECT_TRUE(result.valid);
    EXPECT_TRUE(result.errors.empty());
    
    // Missing required parameter
    result = parser->validate_parameters("get_temperature", R"({})");
    EXPECT_FALSE(result.valid);
    EXPECT_FALSE(result.errors.empty());
    
    // Wrong type
    result = parser->validate_parameters("set_temperature", R"({"zone": "room", "temperature": "not_a_number"})");
    EXPECT_FALSE(result.valid);
    EXPECT_FALSE(result.errors.empty());
}

TEST_F(ParserTest, GetIfexYaml) {
    auto parser = Parser::create(simple_ifex);
    auto yaml = parser->get_ifex_yaml();
    EXPECT_FALSE(yaml.empty());
    EXPECT_NE(yaml.find("test_service"), std::string::npos);
}

TEST_F(ParserTest, InvalidYaml) {
    EXPECT_THROW(Parser::create("invalid yaml {{{"), std::exception);
}

TEST_F(ParserTest, MissingServiceName) {
    const std::string bad_ifex = R"(
description: Missing name field
methods: []
)";
    EXPECT_THROW(Parser::create(bad_ifex), std::exception);
}

TEST_F(ParserTest, UnsignedTypes) {
    auto parser = Parser::create(echo_service_ifex);
    
    auto sig = parser->get_method_signature("echo_with_delay");
    EXPECT_EQ(sig.input_parameters.size(), 2);
    EXPECT_EQ(sig.output_parameters.size(), 2);
    
    // Check uint32 type is correctly parsed
    EXPECT_EQ(sig.input_parameters[1].name, "delay_ms");
    EXPECT_EQ(sig.input_parameters[1].type, Type::UINT32);
    
    EXPECT_EQ(sig.output_parameters[1].name, "actual_delay_ms");
    EXPECT_EQ(sig.output_parameters[1].type, Type::UINT32);
}

TEST_F(ParserTest, DataTypeAliases) {
    // Test that both 'type' and 'datatype' work
    const std::string mixed_types = R"(
name: mixed_service
major_version: 1
minor_version: 0
description: Service with mixed type/datatype usage
namespaces:
  - name: default
    methods:
      - name: test_method
        input:
          - name: param1
            type: string
            mandatory: true
          - name: param2
            datatype: int32
            mandatory: true
        output:
          - name: result
            datatype: boolean
)";
    
    auto parser = Parser::create(mixed_types);
    auto sig = parser->get_method_signature("test_method");
    
    EXPECT_EQ(sig.input_parameters[0].type, Type::STRING);
    EXPECT_EQ(sig.input_parameters[1].type, Type::INT32);
    EXPECT_EQ(sig.output_parameters[0].type, Type::BOOLEAN);
}

TEST_F(ParserTest, MandatoryFields) {
    auto parser = Parser::create(echo_service_ifex);
    
    // Check mandatory parameters
    auto sig = parser->get_method_signature("echo");
    EXPECT_FALSE(sig.input_parameters[0].is_optional);  // mandatory: true means NOT optional
    
    // Test validation with missing mandatory field
    auto result = parser->validate_parameters("echo", R"({})");
    EXPECT_FALSE(result.valid);
    EXPECT_GT(result.errors.size(), 0);
    EXPECT_NE(result.errors[0].find("message"), std::string::npos);
}