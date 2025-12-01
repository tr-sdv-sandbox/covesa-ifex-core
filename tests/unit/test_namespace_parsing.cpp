#include <gtest/gtest.h>
#include <ifex/parser.hpp>
#include <unordered_map>

using namespace ifex;

class NamespaceParsingTest : public ::testing::Test {
protected:
    std::string multi_namespace_ifex = R"(
name: MultiNamespaceService
version: 1.0.0
description: Service with multiple namespaces

namespaces:
  - name: admin
    description: Administrative operations
    methods:
      - name: shutdown
        description: Shutdown the service
        input: []
        output: []
      
      - name: get_status
        description: Get service status
        input: []
        output:
          - name: status
            type: string
  
  - name: user
    description: User operations
    methods:
      - name: create_user
        description: Create a new user
        input:
          - name: username
            type: string
          - name: email
            type: string
        output:
          - name: user_id
            type: int32
      
      - name: delete_user
        description: Delete a user
        input:
          - name: user_id
            type: int32
        output:
          - name: success
            type: boolean
)";
};

TEST_F(NamespaceParsingTest, ParsesMultipleNamespaces) {
    auto parser = Parser::create(multi_namespace_ifex);
    ASSERT_NE(parser, nullptr);
    
    auto methods = parser->get_all_methods();
    ASSERT_EQ(methods.size(), 4);
    
    // Count methods per namespace
    int admin_count = 0;
    int user_count = 0;
    
    for (const auto& method : methods) {
        if (method.namespace_name == "admin") {
            admin_count++;
            EXPECT_TRUE(method.method_name == "shutdown" || method.method_name == "get_status");
        } else if (method.namespace_name == "user") {
            user_count++;
            EXPECT_TRUE(method.method_name == "create_user" || method.method_name == "delete_user");
        } else {
            FAIL() << "Unexpected namespace: " << method.namespace_name;
        }
    }
    
    EXPECT_EQ(admin_count, 2);
    EXPECT_EQ(user_count, 2);
}

TEST_F(NamespaceParsingTest, RegistersNamespacesProperly) {
    // Test that when we register a service with namespaces,
    // the discovery client groups methods by namespace
    
    auto parser = Parser::create(multi_namespace_ifex);
    ASSERT_NE(parser, nullptr);
    
    // Verify that methods have correct namespace assignments
    auto methods = parser->get_all_methods();
    
    std::unordered_map<std::string, int> namespace_counts;
    for (const auto& method : methods) {
        namespace_counts[method.namespace_name]++;
    }
    
    EXPECT_EQ(namespace_counts["admin"], 2);
    EXPECT_EQ(namespace_counts["user"], 2);
    EXPECT_EQ(namespace_counts.size(), 2);
}