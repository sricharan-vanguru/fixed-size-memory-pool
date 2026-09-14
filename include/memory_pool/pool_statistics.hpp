#pragma once

#include <cstddef>

namespace memory_pool {

/// Optional cumulative counters for FixedSizeMemoryPool. Current and peak
/// allocation counts are expressed in blocks; byte fields include padding.
struct PoolStatistics {
    std::size_t allocation_requests{};
    std::size_t successful_allocations{};
    std::size_t deallocation_requests{};
    std::size_t failed_allocations{};
    std::size_t currently_allocated{};
    std::size_t peak_allocated{};
    std::size_t requested_bytes{};
    std::size_t reserved_bytes{};
};

}  // namespace memory_pool
