#pragma once

#include <cstddef>

namespace memory_pool {

/// Cumulative thread-cache activity plus current and peak gauge values.
/// Snapshots are race-free but are not one atomic view of every field.
struct ThreadCacheStatistics {
    std::size_t allocation_requests{};
    std::size_t deallocation_requests{};
    std::size_t cache_hits{};
    std::size_t central_allocations{};
    std::size_t central_deallocations{};
    std::size_t local_deallocations{};
    std::size_t remote_deallocations{};
    std::size_t batch_refills{};
    std::size_t batch_flushes{};
    std::size_t cached_blocks{};
    std::size_t peak_cached_blocks{};
    std::size_t live_allocations{};
    std::size_t peak_live_allocations{};
};

}  // namespace memory_pool
