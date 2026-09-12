#pragma once

#include <cstddef>

namespace memory_pool {

enum class DiagnosticMode {
    disabled,
    enabled,
};

struct PoolOptions {
    DiagnosticMode diagnostics{DiagnosticMode::disabled};
    bool poison_memory{false};
    bool guard_bytes{false};
    bool collect_statistics{false};
    void (*leak_handler)(std::size_t outstanding_blocks) noexcept{nullptr};
};

}  // namespace memory_pool
