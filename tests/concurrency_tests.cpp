#include "memory_pool/new_delete_memory_provider.hpp"
#include "memory_pool/pool_errors.hpp"
#include "memory_pool/synchronized_allocator.hpp"
#include "memory_pool/thread_cached_allocator.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <random>
#include <thread>
#include <utility>
#include <vector>

namespace concurrency_tests {
namespace {

// Shared correctness, cache batching, remote frees, and thread-exit cleanup.
memory_pool::SegregatedAllocatorOptions
concurrency_options(std::size_t initial_blocks = 16) {
    return memory_pool::SegregatedAllocatorOptions{
        .size_classes = {64},
        .initial_blocks_per_class = initial_blocks,
        .growth = memory_pool::GrowthPolicy::geometric(2, 1024),
        .reclamation = {.spare_empty_chunks = 1},
    };
}

memory_pool::ThreadCacheOptions small_cache_options() {
    return memory_pool::ThreadCacheOptions{
        .low_watermark = 2,
        .high_watermark = 4,
        .refill_batch = 4,
    };
}

class CountingMutex {
  public:
    void lock() {
        mutex_.lock();
        lock_calls.fetch_add(1, std::memory_order_relaxed);
    }

    void unlock() { mutex_.unlock(); }

    static std::atomic<std::size_t> lock_calls;

  private:
    std::mutex mutex_;
};

std::atomic<std::size_t> CountingMutex::lock_calls{};

class FailAfterInitialProvider final : public memory_pool::IMemoryProvider {
  public:
    [[nodiscard]] void* allocate(std::size_t bytes, std::size_t alignment) override {
        if (allocation_calls.fetch_add(1, std::memory_order_relaxed) != 0) {
            throw std::bad_alloc{};
        }
        return delegate.allocate(bytes, alignment);
    }

    void deallocate(void* memory,
                    std::size_t bytes,
                    std::size_t alignment) noexcept override {
        delegate.deallocate(memory, bytes, alignment);
    }

  private:
    std::atomic<std::size_t> allocation_calls{};
    memory_pool::NewDeleteMemoryProvider delegate;
};

void synchronized_shared_use(TestContext& test) {
    memory_pool::SynchronizedAllocator allocator(concurrency_options());
    constexpr std::size_t thread_count = 8;
    constexpr std::size_t operations_per_thread = 2'000;
    std::atomic<bool> failed{false};
    std::vector<std::thread> workers;

    for (std::size_t thread = 0; thread < thread_count; ++thread) {
        workers.emplace_back([&, thread] {
            try {
                for (std::size_t operation = 0; operation < operations_per_thread;
                     ++operation) {
                    const std::size_t size = 1 + ((operation + thread) % 64);
                    void* const pointer = allocator.allocate(size, 8);
                    *static_cast<std::byte*>(pointer) =
                        static_cast<std::byte>(operation & 0xffU);
                    allocator.deallocate(pointer, size, 8);
                }
            } catch (...) {
                failed.store(true, std::memory_order_relaxed);
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }

    const auto statistics = allocator.statistics();
    test.expect(!failed.load(std::memory_order_relaxed),
                "synchronized allocation should remain valid under contention");
    test.expect(statistics.successful_allocations ==
                        thread_count * operations_per_thread &&
                    statistics.successful_allocations == statistics.deallocations,
                "synchronized statistics should be protected by the same lock");
}

void synchronized_failure_and_lock_policy(TestContext& test) {
    auto provider = std::make_shared<FailAfterInitialProvider>();
    memory_pool::SynchronizedAllocator allocator(concurrency_options(1), provider);
    void* const held = allocator.allocate(64, 8);
    std::atomic<std::size_t> failures{0};
    std::vector<std::thread> workers;

    for (std::size_t index = 0; index < 2; ++index) {
        workers.emplace_back([&] {
            try {
                static_cast<void>(allocator.allocate(64, 8));
            } catch (const std::bad_alloc&) {
                failures.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    allocator.deallocate(held, 64, 8);

    test.expect(failures.load(std::memory_order_relaxed) == 2,
                "synchronized exhaustion should propagate provider failures safely");

    CountingMutex::lock_calls.store(0, std::memory_order_relaxed);
    memory_pool::BasicSynchronizedAllocator<CountingMutex> custom_lock(
        concurrency_options());
    void* const pointer = custom_lock.allocate(32, 8);
    custom_lock.deallocate(pointer, 32, 8);
    static_cast<void>(custom_lock.statistics());
    test.expect(CountingMutex::lock_calls.load(std::memory_order_relaxed) >= 3,
                "the synchronized wrapper should use its injected lock policy");
}

void cache_reuse_and_watermarks(TestContext& test) {
    memory_pool::ThreadCachedAllocator allocator(concurrency_options(),
                                                 small_cache_options());

    void* const first = allocator.allocate(32, 8);
    allocator.deallocate(first, 32, 8);
    void* const reused = allocator.allocate(24, 8);
    test.expect(reused == first,
                "same-thread allocation should reuse the most recently cached block");
    allocator.deallocate(reused, 24, 8);

    std::vector<void*> live;
    for (std::size_t index = 0; index < 8; ++index) {
        live.push_back(allocator.allocate(32, 8));
    }
    for (void* pointer : live) {
        allocator.deallocate(pointer, 32, 8);
    }

    const auto before_release = allocator.statistics();
    test.expect(before_release.cache_hits > 0 && before_release.batch_refills > 0 &&
                    before_release.batch_flushes > 0,
                "thread caches should refill and flush in batches at their watermarks");
    const std::size_t released = allocator.release_current_thread_cache();
    const auto after_release = allocator.statistics();
    test.expect(released > 0 && after_release.cached_blocks == 0,
                "explicit cache release should return this thread's spare blocks");
    test.expect(after_release.live_allocations == 0 &&
                    after_release.central_allocations ==
                        after_release.central_deallocations,
                "cache release should leave central ownership balanced");
}

void cache_validation(TestContext& test) {
    test.expect_throws<std::invalid_argument>(
        [] {
            memory_pool::ThreadCachedAllocator allocator(
                concurrency_options(),
                {.low_watermark = 5, .high_watermark = 4, .refill_batch = 2});
        },
        "a low watermark above the high watermark should be rejected");
    test.expect_throws<std::invalid_argument>(
        [] {
            memory_pool::ThreadCachedAllocator allocator(
                concurrency_options(),
                {.low_watermark = 0, .high_watermark = 4, .refill_batch = 0});
        },
        "a zero refill batch should be rejected");

    memory_pool::ThreadCachedAllocator allocator(concurrency_options(),
                                                 small_cache_options());
    void* const pointer = allocator.allocate(32, 8);
    test.expect(allocator.owns(pointer) && allocator.owning_size_class(pointer) == 64,
                "thread-cached ownership should expose the central size class");
    test.expect_throws<memory_pool::AllocationMismatchError>(
        [&] { allocator.deallocate(pointer, 65, 8); },
        "thread-cached sized deallocation should validate the route");
    allocator.deallocate(pointer, 32, 8);
    test.expect_throws<memory_pool::DoubleFreeError>(
        [&] { allocator.deallocate(pointer, 32, 8); },
        "a block already stored in a thread cache should reject double-free");
    static_cast<void>(allocator.release_current_thread_cache());
}

void cross_thread_deallocation(TestContext& test) {
    memory_pool::ThreadCachedAllocator allocator(concurrency_options(),
                                                 small_cache_options());
    void* const pointer = allocator.allocate(32, 8);
    std::atomic<bool> failed{false};

    std::thread consumer([&] {
        try {
            allocator.deallocate(pointer, 32, 8);
        } catch (...) {
            failed.store(true, std::memory_order_relaxed);
        }
    });
    consumer.join();

    const auto statistics = allocator.statistics();
    test.expect(!failed.load(std::memory_order_relaxed) &&
                    statistics.remote_deallocations == 1,
                "a remote free should return directly to synchronized central storage");
    test.expect(
        !allocator.owns(pointer),
        "a remotely freed pointer should no longer have live or cached metadata");
    static_cast<void>(allocator.release_current_thread_cache());
}

void fallback_bypasses_thread_cache(TestContext& test) {
    memory_pool::ThreadCachedAllocator allocator(concurrency_options(),
                                                 small_cache_options());
    void* const pointer = allocator.allocate(128, 256);

    test.expect(
        reinterpret_cast<std::uintptr_t>(pointer) % 256 == 0 &&
            !allocator.owning_size_class(pointer).has_value(),
        "large over-aligned requests should preserve central fallback behavior");
    test.expect_throws<memory_pool::AllocationMismatchError>(
        [&] { allocator.deallocate(pointer, 128, 128); },
        "thread-cached fallback deallocation should require exact metadata");

    std::thread consumer([&] { allocator.deallocate(pointer, 128, 256); });
    consumer.join();
    const auto statistics = allocator.statistics();
    test.expect(statistics.remote_deallocations == 1 && statistics.cached_blocks == 0 &&
                    statistics.live_allocations == 0,
                "remote fallback frees should bypass local caches");
}

void thread_exit_cleanup(TestContext& test) {
    memory_pool::ThreadCachedAllocator allocator(concurrency_options(),
                                                 small_cache_options());

    std::thread worker([&] {
        void* const pointer = allocator.allocate(32, 8);
        allocator.deallocate(pointer, 32, 8);
    });
    worker.join();

    const auto statistics = allocator.statistics();
    test.expect(statistics.cached_blocks == 0 && statistics.live_allocations == 0,
                "thread shutdown should flush its local cache automatically");
    test.expect(statistics.central_allocations == statistics.central_deallocations,
                "thread-exit cleanup should balance central allocations");
}

void remote_free_stress(TestContext& test) {
    memory_pool::ThreadCachedAllocator allocator(
        concurrency_options(64),
        {.low_watermark = 8, .high_watermark = 32, .refill_batch = 16});
    constexpr std::size_t remote_count = 2'000;
    constexpr std::size_t thread_count = 8;
    constexpr std::size_t local_operations = 2'000;
    std::vector<void*> remote_allocations;
    remote_allocations.reserve(remote_count);
    for (std::size_t index = 0; index < remote_count; ++index) {
        remote_allocations.push_back(allocator.allocate(32, 8));
    }
    std::mt19937_64 generator(0xC0FFEEU);
    std::shuffle(remote_allocations.begin(), remote_allocations.end(), generator);

    std::atomic<bool> failed{false};
    std::vector<std::thread> workers;
    for (std::size_t thread = 0; thread < thread_count; ++thread) {
        workers.emplace_back([&, thread] {
            try {
                for (std::size_t index = thread; index < remote_count;
                     index += thread_count) {
                    allocator.deallocate(remote_allocations[index], 32, 8);
                }
                for (std::size_t operation = 0; operation < local_operations;
                     ++operation) {
                    void* const pointer = allocator.allocate(32, 8);
                    *static_cast<std::uint64_t*>(pointer) = operation;
                    allocator.deallocate(pointer, 32, 8);
                }
            } catch (...) {
                failed.store(true, std::memory_order_relaxed);
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    static_cast<void>(allocator.release_current_thread_cache());

    const auto statistics = allocator.statistics();
    test.expect(!failed.load(std::memory_order_relaxed),
                "mixed local and remote frees should survive sustained contention");
    test.expect(statistics.remote_deallocations == remote_count,
                "every producer-owned allocation should be counted as a remote free");
    test.expect(statistics.cache_hits > 0,
                "the stress workload should exercise local cache hits");
    test.expect(statistics.live_allocations == 0 && statistics.cached_blocks == 0 &&
                    statistics.central_allocations == statistics.central_deallocations,
                "stress cleanup should return all blocks to central storage");
}

}  // namespace

void run(TestContext& test) {
    synchronized_shared_use(test);
    synchronized_failure_and_lock_policy(test);
    cache_reuse_and_watermarks(test);
    cache_validation(test);
    cross_thread_deallocation(test);
    fallback_bypasses_thread_cache(test);
    thread_exit_cleanup(test);
    remote_free_stress(test);
}

}  // namespace concurrency_tests
