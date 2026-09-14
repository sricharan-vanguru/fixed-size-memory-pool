#include "memory_pool/synchronized_allocator.hpp"
#include "memory_pool/thread_cached_allocator.hpp"

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <thread>
#include <vector>

namespace {

template <typename Allocator>
double measure_shared_pairs(Allocator& allocator,
                            std::size_t thread_count,
                            std::size_t operations_per_thread,
                            std::atomic<std::uintptr_t>& checksum) {
    // Start workers together so both allocators are measured under contention.
    std::barrier start_line(static_cast<std::ptrdiff_t>(thread_count));
    std::vector<std::thread> workers;
    workers.reserve(thread_count);

    const auto start = std::chrono::steady_clock::now();
    for (std::size_t thread = 0; thread < thread_count; ++thread) {
        workers.emplace_back([&] {
            start_line.arrive_and_wait();
            std::uintptr_t local_checksum = 0;
            for (std::size_t operation = 0;
                 operation < operations_per_thread;
                 ++operation) {
                void* const pointer = allocator.allocate(64, 64);
                local_checksum ^= reinterpret_cast<std::uintptr_t>(pointer);
                allocator.deallocate(pointer, 64, 64);
            }
            checksum.fetch_xor(local_checksum, std::memory_order_relaxed);
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const std::size_t total_operations = thread_count * operations_per_thread;
    return std::chrono::duration<double, std::nano>(elapsed).count() /
           static_cast<double>(total_operations);
}

}  // namespace

int main() {
    const std::size_t hardware_threads = std::thread::hardware_concurrency();
    const std::size_t thread_count =
        hardware_threads == 0 ? 4 : std::min<std::size_t>(hardware_threads, 8);
    constexpr std::size_t operations_per_thread = 100'000;

    memory_pool::SegregatedAllocatorOptions allocator_options{
        .initial_blocks_per_class = 64,
    };
    memory_pool::SynchronizedAllocator synchronized(allocator_options);
    // Both candidates use identical central size-class configuration; only the
    // per-thread caching layer differs.
    memory_pool::ThreadCachedAllocator cached(
        allocator_options,
        {.low_watermark = 8, .high_watermark = 64, .refill_batch = 32});
    std::atomic<std::uintptr_t> checksum{};

    const double synchronized_time = measure_shared_pairs(
        synchronized, thread_count, operations_per_thread, checksum);
    const double cached_time = measure_shared_pairs(
        cached, thread_count, operations_per_thread, checksum);

    std::cout << std::fixed << std::setprecision(2)
              << "threads:                        " << thread_count << '\n'
              << "synchronized allocator:        " << synchronized_time
              << " ns/pair\n"
              << "thread-cached allocator:        " << cached_time
              << " ns/pair\n"
              << "thread-cache hits:              "
              << cached.statistics().cache_hits << '\n'
              << "checksum:                       "
              << checksum.load(std::memory_order_relaxed) << '\n';
}
