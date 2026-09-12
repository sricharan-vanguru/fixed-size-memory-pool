#include "detail/statistics_tracker.hpp"

#include <limits>

namespace memory_pool::detail {

void record_allocation_request(PoolStatistics& statistics,
                               std::size_t block_size) noexcept {
    ++statistics.allocation_requests;
    if (statistics.requested_bytes >
        std::numeric_limits<std::size_t>::max() - block_size) {
        statistics.requested_bytes = std::numeric_limits<std::size_t>::max();
    } else {
        statistics.requested_bytes += block_size;
    }
}

void record_successful_allocation(PoolStatistics& statistics) noexcept {
    ++statistics.successful_allocations;
    ++statistics.currently_allocated;
    if (statistics.currently_allocated > statistics.peak_allocated) {
        statistics.peak_allocated = statistics.currently_allocated;
    }
}

void record_failed_allocation(PoolStatistics& statistics) noexcept {
    ++statistics.failed_allocations;
}

void record_deallocation(PoolStatistics& statistics) noexcept {
    ++statistics.deallocation_requests;
    --statistics.currently_allocated;
}

}  // namespace memory_pool::detail
