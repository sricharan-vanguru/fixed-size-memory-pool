#pragma once

#include <cstddef>

namespace memory_pool {

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
