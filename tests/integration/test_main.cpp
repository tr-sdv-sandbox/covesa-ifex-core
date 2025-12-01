#include <gtest/gtest.h>
#include <glog/logging.h>
#include "test_fixture.hpp"

// Global test environment - services start once for all tests
class IntegrationTestEnvironment : public ::testing::Environment {
public:
    void SetUp() override {
        IntegrationTestFixture::GlobalSetUp();
    }

    void TearDown() override {
        IntegrationTestFixture::GlobalTearDown();
    }
};

int main(int argc, char** argv) {
    // Initialize Google Test
    ::testing::InitGoogleTest(&argc, argv);

    // Initialize Google Logging
    google::InitGoogleLogging(argv[0]);
    google::InstallFailureSignalHandler();

    // Register global environment (services start once)
    ::testing::AddGlobalTestEnvironment(new IntegrationTestEnvironment);

    // Run all tests
    return RUN_ALL_TESTS();
}
