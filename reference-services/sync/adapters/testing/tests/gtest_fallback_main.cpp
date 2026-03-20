#include "gtest_fallback.h"

int main(int argc, char** argv) {
    return ::ifex::sync::testing::fallback::run_all(argc, argv);
}
