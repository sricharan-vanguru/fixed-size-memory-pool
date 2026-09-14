#pragma once

#include <cstddef>

namespace memory_pool {

/// Per-size-class cache tuning. A refill obtains `refill_batch` blocks from
/// central storage; an overflowing bin is reduced to `low_watermark` blocks.
struct ThreadCacheOptions {
    std::size_t low_watermark{8};
    std::size_t high_watermark{32};
    std::size_t refill_batch{16};
};

}  // namespace memory_pool
