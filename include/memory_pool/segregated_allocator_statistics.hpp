#pragma once

#include <cstddef>
#include <vector>

namespace memory_pool {

/// Per-class counters make rounding waste visible. For example, a 24-byte
/// request served by a 32-byte class contributes 8 fragmentation bytes.
struct SizeClassStatistics {
    std::size_t class_size{};
    std::size_t allocation_requests{};
    std::size_t successful_allocations{};
    std::size_t deallocations{};
    std::size_t currently_allocated{};
    std::size_t peak_allocated{};
    std::size_t requested_bytes{};
    std::size_t served_bytes{};
    std::size_t internal_fragmentation_bytes{};
};

struct SegregatedAllocatorStatistics {
    std::vector<SizeClassStatistics> size_classes;
    std::size_t allocation_requests{};
    std::size_t successful_allocations{};
    std::size_t deallocations{};
    std::size_t failed_allocations{};
    std::size_t fallback_allocations{};
    std::size_t fallback_deallocations{};
    std::size_t current_fallback_allocations{};
    std::size_t peak_fallback_allocations{};
    std::size_t fallback_requested_bytes{};
};

}  // namespace memory_pool
