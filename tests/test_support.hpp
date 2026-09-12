#pragma once

#include "memory_pool/fixed_size_memory_pool.hpp"

#include <iostream>
#include <utility>

class TestContext {
public:
    void expect(bool condition, const char* message) {
        if (!condition) {
            std::cerr << "FAIL: " << message << '\n';
            ++failures_;
        }
    }

    template <typename Exception, typename Operation>
    void expect_throws(Operation&& operation, const char* message) {
        try {
            std::forward<Operation>(operation)();
        } catch (const Exception&) {
            return;
        } catch (...) {
            std::cerr << "FAIL: " << message << " (wrong exception type)\n";
            ++failures_;
            return;
        }

        std::cerr << "FAIL: " << message << " (no exception)\n";
        ++failures_;
    }

    [[nodiscard]] int failures() const noexcept { return failures_; }

private:
    int failures_{};
};

inline memory_pool::PoolOptions diagnostic_options(bool statistics = false,
                                                   bool poisoning = false,
                                                   bool guards = false) {
    return memory_pool::PoolOptions{
        .diagnostics = memory_pool::DiagnosticMode::enabled,
        .poison_memory = poisoning,
        .guard_bytes = guards,
        .collect_statistics = statistics,
    };
}
