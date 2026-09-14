#include "memory_pool/thread_cached_allocator.hpp"

#include <cstddef>
#include <iostream>
#include <thread>
#include <vector>

int main() {
    memory_pool::ThreadCachedAllocator allocator;
    std::vector<void*> producer_allocations;
    for (std::size_t index = 0; index < 100; ++index) {
        producer_allocations.push_back(allocator.allocate(48, 16));
    }

    std::thread consumer([&] {
        // These blocks were allocated by the main thread, so returning them
        // here exercises the safe cross-thread (remote-free) path.
        for (void* pointer : producer_allocations) {
            allocator.deallocate(pointer, 48, 16);
        }

        // Same-thread reuse below can remain in this worker's local cache.
        for (std::size_t index = 0; index < 1'000; ++index) {
            void* const pointer = allocator.allocate(48, 16);
            allocator.deallocate(pointer, 48, 16);
        }
    });
    consumer.join();
    static_cast<void>(allocator.release_current_thread_cache());

    const auto statistics = allocator.statistics();
    std::cout << "allocation requests: " << statistics.allocation_requests << '\n'
              << "cache hits:          " << statistics.cache_hits << '\n'
              << "remote frees:        " << statistics.remote_deallocations << '\n'
              << "cached blocks:       " << statistics.cached_blocks << '\n';
}
