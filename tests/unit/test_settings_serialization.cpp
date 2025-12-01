#include <gtest/gtest.h>
#include "settings-service.pb.h"
#include <google/protobuf/util/json_util.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

TEST(SettingsSerializationTest, BasicGetPresetsRequest) {
    // Create a get_presets_request
    swdv::settings_service::get_presets_request request;
    
    // Set service_method fields
    auto* service_method = request.mutable_service_method();
    service_method->set_service_name("climate_control_service");
    service_method->set_namespace_name("climate");
    service_method->set_method_name("set_temperature");
    
    // Serialize to bytes
    std::string serialized = request.SerializeAsString();
    EXPECT_GT(serialized.size(), 0);
    
    // Deserialize back
    swdv::settings_service::get_presets_request deserialized;
    EXPECT_TRUE(deserialized.ParseFromString(serialized));
    
    // Verify fields
    EXPECT_EQ(deserialized.service_method().service_name(), "climate_control_service");
    EXPECT_EQ(deserialized.service_method().namespace_name(), "climate");
    EXPECT_EQ(deserialized.service_method().method_name(), "set_temperature");
}

TEST(SettingsSerializationTest, JsonToProtobuf) {
    // Create JSON representation
    json params;
    json service_method;
    service_method["service_name"] = "climate_control_service";
    service_method["namespace_name"] = "climate"; 
    service_method["method_name"] = "set_temperature";
    params["service_method"] = service_method;
    
    std::string json_str = params.dump();
    
    // Convert JSON to protobuf
    swdv::settings_service::get_presets_request request;
    google::protobuf::util::JsonParseOptions options;
    options.ignore_unknown_fields = false;
    
    auto status = google::protobuf::util::JsonStringToMessage(json_str, &request, options);
    ASSERT_TRUE(status.ok()) << "JSON parse failed: " << status.ToString();
    
    // Verify the conversion
    EXPECT_EQ(request.service_method().service_name(), "climate_control_service");
    EXPECT_EQ(request.service_method().namespace_name(), "climate");
    EXPECT_EQ(request.service_method().method_name(), "set_temperature");
    
    // Serialize and check size
    std::string serialized = request.SerializeAsString();
    EXPECT_GT(serialized.size(), 0);
    std::cout << "Serialized size: " << serialized.size() << " bytes" << std::endl;
}

TEST(SettingsSerializationTest, OptionalFields) {
    // Test with optional fields not set
    swdv::settings_service::get_presets_request request;
    
    auto* service_method = request.mutable_service_method();
    service_method->set_service_name("test_service");
    service_method->set_method_name("test_method");
    // namespace_name is optional, don't set it
    
    // Should still serialize successfully
    std::string serialized = request.SerializeAsString();
    EXPECT_GT(serialized.size(), 0);
    
    // And deserialize
    swdv::settings_service::get_presets_request deserialized;
    EXPECT_TRUE(deserialized.ParseFromString(serialized));
    
    EXPECT_EQ(deserialized.service_method().service_name(), "test_service");
    EXPECT_EQ(deserialized.service_method().method_name(), "test_method");
    EXPECT_EQ(deserialized.service_method().namespace_name(), ""); // Should be empty
}

TEST(SettingsSerializationTest, FilterSerialization) {
    // Test with filter field
    swdv::settings_service::get_presets_request request;
    
    auto* service_method = request.mutable_service_method();
    service_method->set_service_name("test_service");
    service_method->set_method_name("test_method");
    
    // Add a filter
    auto* filter = request.mutable_filter();
    filter->set_type(swdv::settings_service::TYPE_USER);
    filter->add_tags("comfort");
    filter->add_tags("summer");
    
    std::string serialized = request.SerializeAsString();
    EXPECT_GT(serialized.size(), 0);
    
    // Deserialize and verify
    swdv::settings_service::get_presets_request deserialized;
    EXPECT_TRUE(deserialized.ParseFromString(serialized));
    
    EXPECT_TRUE(deserialized.has_filter());
    EXPECT_EQ(deserialized.filter().type(), swdv::settings_service::TYPE_USER);
    EXPECT_EQ(deserialized.filter().tags_size(), 2);
    EXPECT_EQ(deserialized.filter().tags(0), "comfort");
    EXPECT_EQ(deserialized.filter().tags(1), "summer");
}

TEST(SettingsSerializationTest, ExactDispatcherScenario) {
    // Replicate exactly what the dispatcher does
    json params;
    json service_method;
    service_method["service_name"] = "climate_control_service";
    service_method["namespace_name"] = "climate";
    service_method["method_name"] = "set_temperature";
    params["service_method"] = service_method;
    
    std::string json_str = params.dump();
    std::cout << "JSON input: " << json_str << std::endl;
    
    // Convert JSON to protobuf (like the dynamic caller does)
    swdv::settings_service::get_presets_request request;
    google::protobuf::util::JsonParseOptions options;
    options.ignore_unknown_fields = false;
    
    auto status = google::protobuf::util::JsonStringToMessage(json_str, &request, options);
    ASSERT_TRUE(status.ok()) << "JSON parse failed: " << status.ToString();
    
    // Serialize 
    std::string serialized = request.SerializeAsString();
    std::cout << "Serialized size: " << serialized.size() << " bytes" << std::endl;
    
    // Hex dump the serialized data
    std::cout << "Hex dump: ";
    for (unsigned char c : serialized) {
        printf("%02x ", c);
    }
    std::cout << std::endl;
    
    // The dispatcher reported 103 bytes, we get 53 bytes
    // This suggests the dispatcher might be double-encoding
    EXPECT_LT(serialized.size(), 100);
}