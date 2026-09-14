#include "test_support.hpp"

namespace statistics_tests {

// Verify both cumulative counters and current/peak gauges around exhaustion.
void run(TestContext& test) {
    memory_pool::FixedSizeMemoryPool pool(24, 2, alignof(std::max_align_t),
                                          diagnostic_options(true));
    void* first = pool.allocate();
    void* second = pool.allocate();
    test.expect(pool.allocate() == nullptr, "a third allocation should exhaust the pool");
    pool.deallocate(first);

    const auto& stats = pool.statistics();
    test.expect(stats.allocation_requests == 3, "allocation attempts should be counted");
    test.expect(stats.successful_allocations == 2, "successful allocations should be counted");
    test.expect(stats.failed_allocations == 1, "failed allocations should be counted");
    test.expect(stats.deallocation_requests == 1, "deallocations should be counted");
    test.expect(stats.currently_allocated == 1, "live blocks should be reported");
    test.expect(stats.peak_allocated == 2, "peak simultaneous use should be reported");
    test.expect(stats.reserved_bytes >= pool.capacity() * pool.block_size(),
                "reserved storage should include all usable blocks");
    pool.deallocate(second);
}

}  // namespace statistics_tests
