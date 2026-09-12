#pragma once

#include "memory_pool/pool_statistics.hpp"

#include <cstddef>

namespace memory_pool::detail {

void record_allocation_request(PoolStatistics& statistics,
                               std::size_t block_size) noexcept;
void record_successful_allocation(PoolStatistics& statistics) noexcept;
void record_failed_allocation(PoolStatistics& statistics) noexcept;
void record_deallocation(PoolStatistics& statistics) noexcept;

}  // namespace memory_pool::detail
