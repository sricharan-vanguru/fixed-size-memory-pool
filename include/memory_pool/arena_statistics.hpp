#pragma once

#include <cstddef>

namespace memory_pool {

/// Cumulative arena activity plus current and peak storage gauges. Used bytes
/// include alignment padding; requested_bytes does not.
struct ArenaStatistics {
    std::size_t allocation_requests{};
    std::size_t successful_allocations{};
    std::size_t failed_allocations{};
    std::size_t requested_bytes{};
    std::size_t current_used_bytes{};
    std::size_t peak_used_bytes{};
    std::size_t padding_bytes{};
    std::size_t current_reserved_bytes{};
    std::size_t peak_reserved_bytes{};
    std::size_t current_chunks{};
    std::size_t peak_chunks{};
    std::size_t chunk_allocations{};
    std::size_t chunk_releases{};
    std::size_t reset_calls{};
    std::size_t object_constructions{};
    std::size_t construction_failures{};
    std::size_t destructor_calls{};
};

}  // namespace memory_pool
