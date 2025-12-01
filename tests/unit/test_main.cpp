#include <gtest/gtest.h>
#include <glog/logging.h>

int main(int argc, char** argv) {
    // Initialize Google Test
    ::testing::InitGoogleTest(&argc, argv);
    
    // Initialize Google Logging
    google::InitGoogleLogging(argv[0]);
    google::InstallFailureSignalHandler();
    
    // Run all tests
    return RUN_ALL_TESTS();
}
