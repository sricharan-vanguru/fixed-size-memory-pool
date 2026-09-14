#include "memory_pool/new_delete_memory_provider.hpp"
#include "memory_pool/pool_errors.hpp"
#include "memory_pool/synchronized_allocator.hpp"
#include "memory_pool/thread_cached_allocator.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <cstddef>
#include <limits>
#include <memory>
#include <new>
#include <thread>
#include <vector>

namespace concurrency_edge_tests {
namespace {

// Adversarial races and boundary configurations supplement the normal suite.
memory_pool::SegregatedAllocatorOptions
single_class_options(std::size_t initial_blocks = 16) {
    return memory_pool::SegregatedAllocatorOptions{
        .size_classes = {64},
        .initial_blocks_per_class = initial_blocks,
        .growth = memory_pool::GrowthPolicy::geometric(2, 1024),
        .reclamation = {.spare_empty_chunks = 1},
    };
}

memory_pool::ThreadCacheOptions small_cache_options() {
    return {
        .low_watermark = 2,
        .high_watermark = 4,
        .refill_batch = 4,
    };
}

class FailAfterInitialProvider final : public memory_pool::IMemoryProvider {
  public:
    [[nodiscard]] void* allocate(std::size_t bytes, std::size_t alignment) override {
        if (allocation_calls_.fetch_add(1, std::memory_order_relaxed) != 0) {
            throw std::bad_alloc{};
        }
        return delegate_.allocate(bytes, alignment);
    }

    void deallocate(void* memory,
                    std::size_t bytes,
                    std::size_t alignment) noexcept override {
        delegate_.deallocate(memory, bytes, alignment);
    }

  private:
    std::atomic<std::size_t> allocation_calls_{};
    memory_pool::NewDeleteMemoryProvider delegate_;
};

void extreme_configuration_and_invalid_inputs(TestContext& test) {
    test.expect_throws<std::invalid_argument>(
        [] {
            memory_pool::ThreadCachedAllocator allocator(
                single_class_options(),
                {.low_watermark = 0,
                 .high_watermark = std::numeric_limits<std::size_t>::max(),
                 .refill_batch = 1});
        },
        "the maximum watermark should be rejected before cache capacity overflows");

    memory_pool::SynchronizedAllocator synchronized(single_class_options());
    void* const synchronized_zero = synchronized.allocate(0, 8);
    synchronized.deallocate(synchronized_zero, 0, 8);
    synchronized.deallocate(nullptr);
    synchronized.deallocate(nullptr, 0, 8);
    test.expect_throws<std::invalid_argument>(
        [&] { static_cast<void>(synchronized.allocate(8, 3)); },
        "the synchronized wrapper should preserve alignment validation");
    int external = 0;
    test.expect_throws<memory_pool::InvalidPoolPointer>(
        [&] { synchronized.deallocate(&external); },
        "the synchronized wrapper should reject foreign pointers");

    memory_pool::ThreadCachedAllocator cached(single_class_options(),
                                              small_cache_options());
    void* const cached_zero = cached.allocate(0, 8);
    cached.deallocate(cached_zero, 0, 8);
    cached.deallocate(nullptr);
    cached.deallocate(nullptr, 0, 8);
    test.expect_throws<std::invalid_argument>(
        [&] { static_cast<void>(cached.allocate(8, 3)); },
        "the thread-cached wrapper should preserve alignment validation");
    test.expect_throws<memory_pool::InvalidPoolPointer>(
        [&] { cached.deallocate(&external); },
        "the thread-cached wrapper should reject foreign pointers");
    static_cast<void>(cached.release_current_thread_cache());
}

void partial_refill_failure(TestContext& test) {
    auto provider = std::make_shared<FailAfterInitialProvider>();
    memory_pool::ThreadCachedAllocator allocator(
        single_class_options(1), small_cache_options(), provider);

    void* const pointer = allocator.allocate(32, 8);
    test.expect(pointer != nullptr,
                "a refill should return its first block when later prefetch fails");
    const auto after_allocation = allocator.statistics();
    const auto central_after_allocation = allocator.central_statistics();
    test.expect(after_allocation.central_allocations == 1 &&
                    after_allocation.cached_blocks == 0 &&
                    after_allocation.live_allocations == 1 &&
                    central_after_allocation.failed_allocations == 1,
                "partial refill failure should keep only the successful live block");
    allocator.deallocate(pointer, 32, 8);
    static_cast<void>(allocator.release_current_thread_cache());
    const auto after_release = allocator.statistics();
    test.expect(after_release.central_allocations ==
                        after_release.central_deallocations &&
                    after_release.live_allocations == 0,
                "partial refill failure should preserve ownership balance");
}

void simultaneous_double_free(TestContext& test) {
    memory_pool::ThreadCachedAllocator allocator(single_class_options(),
                                                 small_cache_options());
    void* const pointer = allocator.allocate(32, 8);
    std::barrier start_line(3);
    std::atomic<std::size_t> successes{};
    std::atomic<std::size_t> expected_failures{};
    std::atomic<std::size_t> unexpected_failures{};

    auto deallocate_once = [&] {
        start_line.arrive_and_wait();
        try {
            allocator.deallocate(pointer, 32, 8);
            successes.fetch_add(1, std::memory_order_relaxed);
        } catch (const memory_pool::InvalidPoolPointer&) {
            expected_failures.fetch_add(1, std::memory_order_relaxed);
        } catch (const memory_pool::DoubleFreeError&) {
            expected_failures.fetch_add(1, std::memory_order_relaxed);
        } catch (...) {
            unexpected_failures.fetch_add(1, std::memory_order_relaxed);
        }
    };

    std::thread first(deallocate_once);
    std::thread second(deallocate_once);
    start_line.arrive_and_wait();
    first.join();
    second.join();
    static_cast<void>(allocator.release_current_thread_cache());

    test.expect(successes.load(std::memory_order_relaxed) == 1 &&
                    expected_failures.load(std::memory_order_relaxed) == 1 &&
                    unexpected_failures.load(std::memory_order_relaxed) == 0,
                "simultaneous double-free should permit exactly one return");
    test.expect(allocator.statistics().live_allocations == 0,
                "a rejected concurrent double-free should not corrupt live counts");
}

void owner_thread_exit_with_live_allocation(TestContext& test) {
    memory_pool::ThreadCachedAllocator allocator(single_class_options(),
                                                 small_cache_options());
    void* pointer = nullptr;

    std::thread owner([&] { pointer = allocator.allocate(32, 8); });
    owner.join();
    test.expect(pointer != nullptr && allocator.owns(pointer),
                "an active allocation should survive its owner thread exiting");

    allocator.deallocate(pointer, 32, 8);
    const auto statistics = allocator.statistics();
    test.expect(statistics.remote_deallocations == 1 &&
                    statistics.live_allocations == 0 && statistics.cached_blocks == 0,
                "a later thread should safely return an exited owner's live block");
}

void multiple_allocator_tls_isolation(TestContext& test) {
    memory_pool::ThreadCachedAllocator first(single_class_options(),
                                             small_cache_options());
    memory_pool::ThreadCachedAllocator second(single_class_options(),
                                              small_cache_options());
    void* const first_pointer = first.allocate(32, 8);
    void* const second_pointer = second.allocate(32, 8);
    first.deallocate(first_pointer, 32, 8);
    second.deallocate(second_pointer, 32, 8);

    test.expect(first.statistics().cached_blocks > 0 &&
                    second.statistics().cached_blocks > 0,
                "two allocators should maintain independent TLS cache entries");
    static_cast<void>(first.release_current_thread_cache());
    test.expect(first.statistics().cached_blocks == 0 &&
                    second.statistics().cached_blocks > 0,
                "releasing one allocator cache should not affect another allocator");
    static_cast<void>(second.release_current_thread_cache());
    test.expect(second.statistics().cached_blocks == 0,
                "each allocator should release only its own cached blocks");
}

void all_size_classes_under_contention(TestContext& test) {
    memory_pool::SegregatedAllocatorOptions options{
        .initial_blocks_per_class = 8,
        .growth = memory_pool::GrowthPolicy::geometric(2, 256),
        .reclamation = {.spare_empty_chunks = 1},
    };
    memory_pool::ThreadCachedAllocator allocator(
        options, {.low_watermark = 2, .high_watermark = 8, .refill_batch = 4});
    constexpr std::array<std::size_t, 22> sizes{
        0,   1,   8,   9,   16,  17,   32,   33,   64,   65,   128,
        129, 256, 257, 512, 513, 1024, 1025, 2048, 2049, 4096, 4097};
    constexpr std::array<std::size_t, 7> alignments{1, 8, 16, 64, 256, 4096, 8192};
    constexpr std::size_t thread_count = 8;
    constexpr std::size_t rounds = 50;
    std::atomic<bool> failed{false};
    std::vector<std::thread> workers;

    for (std::size_t thread = 0; thread < thread_count; ++thread) {
        workers.emplace_back([&, thread] {
            try {
                for (std::size_t round = 0; round < rounds; ++round) {
                    for (std::size_t index = 0; index < sizes.size(); ++index) {
                        const std::size_t size = sizes[index];
                        const std::size_t alignment =
                            alignments[(index + round + thread) % alignments.size()];
                        void* const pointer = allocator.allocate(size, alignment);
                        *static_cast<std::byte*>(pointer) = std::byte{0x5a};
                        allocator.deallocate(pointer, size, alignment);
                    }
                }
            } catch (...) {
                failed.store(true, std::memory_order_relaxed);
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }

    const auto cache_statistics = allocator.statistics();
    const auto central_statistics = allocator.central_statistics();
    const bool every_class_used =
        std::all_of(central_statistics.size_classes.begin(),
                    central_statistics.size_classes.end(),
                    [](const auto& entry) { return entry.successful_allocations > 0; });
    test.expect(!failed.load(std::memory_order_relaxed) && every_class_used,
                "concurrent requests should exercise every configured size class");
    test.expect(central_statistics.fallback_allocations > 0,
                "mixed-size contention should also exercise fallback requests");
    test.expect(cache_statistics.live_allocations == 0 &&
                    cache_statistics.cached_blocks == 0,
                "all-size-class workers should flush their caches on exit");
}

void concurrent_inspection(TestContext& test) {
    memory_pool::ThreadCachedAllocator cached(single_class_options(),
                                              small_cache_options());
    void* const stable = cached.allocate(32, 8);
    std::atomic<std::size_t> running_workers{4};
    std::atomic<bool> failed{false};
    std::vector<std::thread> workers;

    for (std::size_t index = 0; index < 4; ++index) {
        workers.emplace_back([&] {
            try {
                for (std::size_t operation = 0; operation < 2'000; ++operation) {
                    void* const pointer = cached.allocate(32, 8);
                    cached.deallocate(pointer, 32, 8);
                }
            } catch (...) {
                failed.store(true, std::memory_order_relaxed);
            }
            running_workers.fetch_sub(1, std::memory_order_release);
        });
    }

    std::thread observer([&] {
        while (running_workers.load(std::memory_order_acquire) != 0) {
            if (!cached.owns(stable) || cached.owning_size_class(stable) != 64) {
                failed.store(true, std::memory_order_relaxed);
            }
            static_cast<void>(cached.statistics());
            static_cast<void>(cached.central_statistics());
            std::this_thread::yield();
        }
    });
    for (std::thread& worker : workers) {
        worker.join();
    }
    observer.join();
    cached.deallocate(stable, 32, 8);
    static_cast<void>(cached.release_current_thread_cache());
    test.expect(!failed.load(std::memory_order_relaxed),
                "ownership and statistics queries should be safe during allocation");

    memory_pool::SynchronizedAllocator synchronized(single_class_options());
    void* const synchronized_stable = synchronized.allocate(32, 8);
    std::atomic<bool> synchronized_failed{false};
    std::atomic<bool> mutator_done{false};
    std::thread mutator([&] {
        try {
            for (std::size_t operation = 0; operation < 2'000; ++operation) {
                void* const pointer = synchronized.allocate(32, 8);
                synchronized.deallocate(pointer, 32, 8);
            }
        } catch (...) {
            synchronized_failed.store(true, std::memory_order_relaxed);
        }
        mutator_done.store(true, std::memory_order_release);
    });
    while (!mutator_done.load(std::memory_order_acquire)) {
        static_cast<void>(synchronized.statistics());
        static_cast<void>(synchronized.size_classes());
        if (!synchronized.owns(synchronized_stable)) {
            synchronized_failed.store(true, std::memory_order_relaxed);
        }
        std::this_thread::yield();
    }
    mutator.join();
    synchronized.deallocate(synchronized_stable, 32, 8);
    test.expect(!synchronized_failed.load(std::memory_order_relaxed),
                "synchronized inspection APIs should share the allocator lock");
}

void concurrent_unsized_remote_deallocation(TestContext& test) {
    memory_pool::ThreadCachedAllocator allocator(single_class_options(64),
                                                 small_cache_options());
    constexpr std::size_t allocation_count = 512;
    constexpr std::size_t thread_count = 8;
    std::vector<void*> allocations;
    allocations.reserve(allocation_count);
    for (std::size_t index = 0; index < allocation_count; ++index) {
        allocations.push_back(allocator.allocate(32, 8));
    }

    std::atomic<bool> failed{false};
    std::vector<std::thread> workers;
    for (std::size_t thread = 0; thread < thread_count; ++thread) {
        workers.emplace_back([&, thread] {
            try {
                for (std::size_t index = thread; index < allocations.size();
                     index += thread_count) {
                    allocator.deallocate(allocations[index]);
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
    test.expect(!failed.load(std::memory_order_relaxed) &&
                    statistics.remote_deallocations == allocation_count,
                "unsized remote frees should route safely to central storage");
    test.expect(statistics.live_allocations == 0 && statistics.cached_blocks == 0,
                "unsized remote-free cleanup should leave no retained blocks");
}

void oversubscribed_stress(TestContext& test) {
    memory_pool::ThreadCachedAllocator allocator(
        single_class_options(64),
        {.low_watermark = 4, .high_watermark = 16, .refill_batch = 8});
    const std::size_t hardware_threads = std::thread::hardware_concurrency();
    const std::size_t doubled_threads =
        hardware_threads > 16 ? 32 : hardware_threads * 2;
    const std::size_t thread_count =
        std::min<std::size_t>(32, std::max<std::size_t>(16, doubled_threads));
    constexpr std::size_t operations_per_thread = 10'000;
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
                "oversubscribed thread-cache stress should complete without errors");
    test.expect(statistics.allocation_requests ==
                        thread_count * operations_per_thread &&
                    statistics.live_allocations == 0 && statistics.cached_blocks == 0,
                "oversubscribed workers should finish with balanced logical state");
}

}  // namespace

void run(TestContext& test) {
    extreme_configuration_and_invalid_inputs(test);
    partial_refill_failure(test);
    simultaneous_double_free(test);
    owner_thread_exit_with_live_allocation(test);
    multiple_allocator_tls_isolation(test);
    all_size_classes_under_contention(test);
    concurrent_inspection(test);
    concurrent_unsized_remote_deallocation(test);
    oversubscribed_stress(test);
}

}  // namespace concurrency_edge_tests
