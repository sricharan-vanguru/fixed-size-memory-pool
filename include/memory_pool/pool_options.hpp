#pragma once

#include <cstddef>

namespace memory_pool {

enum class DiagnosticMode {
    disabled,
    enabled,
};

struct PoolOptions {
    // Features below are independent except that allocation-state queries and
    // deterministic double-free detection require diagnostics to be enabled.
    DiagnosticMode diagnostics{DiagnosticMode::disabled};
    bool poison_memory{false};
    bool guard_bytes{false};
    bool collect_statistics{false};
    /// Called during pool destruction; therefore it must not throw or use the
    /// pool that is already being torn down.
    void (*leak_handler)(std::size_t outstanding_blocks) noexcept{nullptr};
};

}  // namespace memory_pool
