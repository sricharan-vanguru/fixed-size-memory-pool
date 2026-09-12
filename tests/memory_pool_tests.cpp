#include "test_support.hpp"

#include <iostream>

namespace core_tests {
void run(TestContext& test);
}

namespace diagnostics_tests {
void run(TestContext& test);
}

namespace statistics_tests {
void run(TestContext& test);
}

int main() {
    TestContext test;
    core_tests::run(test);
    diagnostics_tests::run(test);
    statistics_tests::run(test);

    if (test.failures() == 0) {
        std::cout << "All memory pool tests passed\n";
    }
    return test.failures() == 0 ? 0 : 1;
}
