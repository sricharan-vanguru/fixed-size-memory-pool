#include "memory_pool/pool_errors.hpp"
#include "memory_pool/new_delete_memory_provider.hpp"
#include "memory_pool/segregated_allocator.hpp"
#include "memory_pool/size_class_selector.hpp"
#include "test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <new>
#include <random>
#include <stdexcept>
#include <unordered_set>
#include <utility>
#include <vector>

namespace segregated_allocator_tests {
namespace {

// Size-class boundaries, fallback metadata, routing errors, and random traffic.
class TrackingProvider final : public memory_pool::IMemoryProvider {
public:
    [[nodiscard]] void* allocate(std::size_t bytes,
                                 std::size_t alignment) override {
        ++allocation_calls;
        if (fail_on_call != 0 && allocation_calls == fail_on_call) {
            throw std::bad_alloc{};
        }
        ++successful_allocations;
        return delegate.allocate(bytes, alignment);
    }

    void deallocate(void* memory,
                    std::size_t bytes,
                    std::size_t alignment) noexcept override {
        ++deallocation_calls;
        delegate.deallocate(memory, bytes, alignment);
    }

    memory_pool::NewDeleteMemoryProvider delegate;
    std::size_t allocation_calls{};
    std::size_t successful_allocations{};
    std::size_t deallocation_calls{};
    std::size_t fail_on_call{};
};

memory_pool::SegregatedAllocatorOptions allocator_options(
    std::vector<std::size_t> classes =
        memory_pool::SizeClassSelector::default_size_classes()) {
    return memory_pool::SegregatedAllocatorOptions{
        .size_classes = std::move(classes),
        .initial_blocks_per_class = 2,
        .growth = memory_pool::GrowthPolicy::geometric(2, 64),
        .reclamation = {.spare_empty_chunks = 1},
    };
}

void default_size_class_boundaries(TestContext& test) {
    memory_pool::SizeClassSelector selector;
    const auto classes = selector.size_classes();

    test.expect(classes.front() == 8 && classes.back() == 4096 &&
                    classes.size() == 10,
                "default size classes should span 8 through 4096 bytes");

    std::size_t previous = 0;
    for (std::size_t index = 0; index < classes.size(); ++index) {
        const std::size_t lower_boundary = previous + 1;
        test.expect(selector.select(lower_boundary, 1) == index,
                    "a boundary request should select the smallest fitting class");
        test.expect(selector.select(classes[index], 1) == index,
                    "an exact size should remain in its matching class");
        test.expect(selector.select(1, classes[index]) == index,
                    "alignment should participate in size-class selection");
        previous = classes[index];
    }

    test.expect(!selector.select(4097, 1).has_value(),
                "a request above the largest class should use fallback");
    test.expect(!selector.select(1, 8192).has_value(),
                "unsupported over-alignment should use fallback");
}

void size_class_validation(TestContext& test) {
    test.expect_throws<std::invalid_argument>(
        [] { memory_pool::SizeClassSelector selector({}); },
        "an empty size-class configuration should be rejected");
    test.expect_throws<std::invalid_argument>(
        [] { memory_pool::SizeClassSelector selector({8, 16, 16}); },
        "duplicate size classes should be rejected");
    test.expect_throws<std::invalid_argument>(
        [] { memory_pool::SizeClassSelector selector({8, 24, 32}); },
        "non-power-of-two size classes should be rejected");
    test.expect_throws<std::invalid_argument>(
        [] {
            memory_pool::SizeClassSelector selector({sizeof(void*) / 2});
        },
        "classes smaller than a free-list pointer should be rejected");

    memory_pool::SizeClassSelector selector;
    test.expect_throws<std::invalid_argument>(
        [&] { static_cast<void>(selector.select(8, 3)); },
        "request alignment should be a non-zero power of two");
    test.expect_throws<std::out_of_range>(
        [&] { static_cast<void>(selector.class_size(selector.class_count())); },
        "an invalid class index should be rejected");
    test.expect_throws<std::invalid_argument>(
        [] {
            auto options = allocator_options();
            options.initial_blocks_per_class = 0;
            memory_pool::SegregatedAllocator allocator(std::move(options));
        },
        "zero initial capacity per class should be rejected");
}

void routes_every_boundary(TestContext& test) {
    memory_pool::SegregatedAllocator allocator(allocator_options());
    const auto classes = allocator.selector().size_classes();
    std::vector<void*> allocations;

    std::size_t previous = 0;
    for (const std::size_t class_size : classes) {
        const std::size_t request = previous + 1;
        void* pointer = allocator.allocate(request, 1);
        test.expect(allocator.owning_size_class(pointer) == class_size,
                    "allocation should be owned by its selected size class");
        allocations.push_back(pointer);
        previous = class_size;
    }

    previous = 0;
    for (std::size_t index = 0; index < allocations.size(); ++index) {
        allocator.deallocate(allocations[index], previous + 1, 1);
        previous = classes[index];
    }
    test.expect(allocator.statistics().deallocations == classes.size(),
                "every boundary allocation should route back to its owner");
}

void size_and_alignment_routing(TestContext& test) {
    memory_pool::SegregatedAllocator allocator(allocator_options());
    void* sized = allocator.allocate(24, 8);
    void* aligned = allocator.allocate(24, 64);

    test.expect(allocator.owning_size_class(sized) == 32,
                "24 bytes should use the 32-byte class");
    test.expect(allocator.owning_size_class(aligned) == 64,
                "a 64-byte alignment should promote the request to class 64");
    test.expect(reinterpret_cast<std::uintptr_t>(aligned) % 64 == 0,
                "the selected class should preserve requested alignment");

    allocator.deallocate(sized);
    allocator.deallocate(aligned, 24, 64);
}

void large_and_over_aligned_fallback(TestContext& test) {
    memory_pool::SegregatedAllocator allocator(allocator_options());
    void* large = allocator.allocate(4097, 64);
    void* over_aligned = allocator.allocate(16, 8192);

    test.expect(allocator.owns(large) &&
                    !allocator.owning_size_class(large).has_value(),
                "a large allocation should be tracked by fallback");
    test.expect(reinterpret_cast<std::uintptr_t>(large) % 64 == 0,
                "large fallback should preserve alignment");
    test.expect(reinterpret_cast<std::uintptr_t>(over_aligned) % 8192 == 0,
                "over-aligned fallback should preserve alignment");
    test.expect(allocator.statistics().current_fallback_allocations == 2,
                "fallback statistics should track live allocations");

    allocator.deallocate(large, 4097, 64);
    allocator.deallocate(over_aligned, 16, 8192);
    test.expect(allocator.statistics().current_fallback_allocations == 0 &&
                    allocator.statistics().fallback_deallocations == 2,
                "fallback deallocation should release and update metadata");
}

void sized_deallocation_mismatch_detection(TestContext& test) {
    memory_pool::SegregatedAllocator allocator(allocator_options());
    void* pooled = allocator.allocate(17, 8);
    test.expect_throws<memory_pool::AllocationMismatchError>(
        [&] { allocator.deallocate(pooled, 16, 8); },
        "sized deallocation should detect a different pooled class");
    test.expect(allocator.owns(pooled),
                "a rejected sized deallocation should leave the block live");
    allocator.deallocate(pooled, 17, 8);

    void* fallback = allocator.allocate(5000, 64);
    test.expect_throws<memory_pool::AllocationMismatchError>(
        [&] { allocator.deallocate(fallback, 5001, 64); },
        "fallback deallocation should require the exact original size");
    test.expect_throws<memory_pool::AllocationMismatchError>(
        [&] { allocator.deallocate(fallback, 5000, 128); },
        "fallback deallocation should require the exact original alignment");
    allocator.deallocate(fallback, 5000, 64);

    int external = 0;
    test.expect_throws<memory_pool::InvalidPoolPointer>(
        [&] { allocator.deallocate(&external); },
        "an unknown pointer should be rejected");
}

void internal_fragmentation_statistics(TestContext& test) {
    memory_pool::SegregatedAllocator allocator(
        allocator_options({8, 16, 32}));
    void* first = allocator.allocate(9, 1);
    void* second = allocator.allocate(15, 1);

    const auto& statistics = allocator.statistics().size_classes[1];
    test.expect(statistics.class_size == 16 &&
                    statistics.requested_bytes == 24 &&
                    statistics.served_bytes == 32 &&
                    statistics.internal_fragmentation_bytes == 8,
                "statistics should accumulate internal fragmentation per class");
    test.expect(statistics.currently_allocated == 2 &&
                    statistics.peak_allocated == 2,
                "class statistics should track current and peak use");

    allocator.deallocate(first);
    allocator.deallocate(second);
    test.expect(statistics.currently_allocated == 0 && statistics.deallocations == 2,
                "class statistics should track successful returns");
}

void zero_size_and_invalid_alignment(TestContext& test) {
    memory_pool::SegregatedAllocator allocator(allocator_options());
    void* zero = allocator.allocate(0, 8);
    test.expect(allocator.owning_size_class(zero) == 8,
                "a zero-size request should reserve one byte in the smallest class");
    allocator.deallocate(zero, 0, 8);

    test.expect_throws<std::invalid_argument>(
        [&] { static_cast<void>(allocator.allocate(8, 0)); },
        "zero alignment should be rejected");
    test.expect(allocator.statistics().failed_allocations == 1,
                "invalid allocation requests should be counted as failures");
}

void fallback_failure_and_destruction(TestContext& test) {
    auto failure_provider = std::make_shared<TrackingProvider>();
    {
        memory_pool::SegregatedAllocator allocator(
            allocator_options({8}), failure_provider);
        failure_provider->fail_on_call = 2;
        test.expect_throws<std::bad_alloc>(
            [&] { static_cast<void>(allocator.allocate(9, 1)); },
            "fallback provider failure should propagate to the caller");
        test.expect(allocator.statistics().failed_allocations == 1 &&
                        allocator.statistics().current_fallback_allocations == 0,
                    "failed fallback should not leave allocation metadata");
    }
    test.expect(failure_provider->successful_allocations ==
                    failure_provider->deallocation_calls,
                "failed fallback should preserve provider ownership balance");

    auto cleanup_provider = std::make_shared<TrackingProvider>();
    {
        memory_pool::SegregatedAllocator allocator(
            allocator_options({8}), cleanup_provider);
        static_cast<void>(allocator.allocate(9, 1));
    }
    test.expect(cleanup_provider->successful_allocations ==
                    cleanup_provider->deallocation_calls,
                "allocator destruction should release outstanding fallback storage");
}

void randomized_mixed_allocations(TestContext& test) {
    memory_pool::SegregatedAllocator allocator(allocator_options());
    constexpr std::array<std::size_t, 10> alignments{
        1, 2, 4, 8, 16, 32, 64, 128, 4096, 8192};

    struct Allocation {
        void* pointer;
        std::size_t size;
        std::size_t alignment;
    };

    std::mt19937_64 generator(0xC0FFEEU);
    std::uniform_int_distribution<std::size_t> size_distribution(0, 6000);
    std::uniform_int_distribution<std::size_t> alignment_distribution(
        0, alignments.size() - 1);
    std::vector<Allocation> live;
    std::unordered_set<void*> unique_pointers;

    for (std::size_t operation = 0; operation < 5000; ++operation) {
        const bool should_allocate = live.empty() ||
                                     (live.size() < 128 && generator() % 2 == 0);
        if (should_allocate) {
            const std::size_t size = size_distribution(generator);
            const std::size_t alignment =
                alignments[alignment_distribution(generator)];
            void* pointer = allocator.allocate(size, alignment);
            test.expect(unique_pointers.insert(pointer).second,
                        "simultaneously live allocations should have unique addresses");
            live.push_back({pointer, size, alignment});
            continue;
        }

        const std::size_t index = generator() % live.size();
        const Allocation allocation = live[index];
        allocator.deallocate(
            allocation.pointer, allocation.size, allocation.alignment);
        unique_pointers.erase(allocation.pointer);
        live[index] = live.back();
        live.pop_back();
    }

    for (const Allocation& allocation : live) {
        allocator.deallocate(
            allocation.pointer, allocation.size, allocation.alignment);
    }
    test.expect(allocator.statistics().successful_allocations ==
                    allocator.statistics().deallocations,
                "random mixed allocations should all return to their owners");
}

}  // namespace

void run(TestContext& test) {
    default_size_class_boundaries(test);
    size_class_validation(test);
    routes_every_boundary(test);
    size_and_alignment_routing(test);
    large_and_over_aligned_fallback(test);
    sized_deallocation_mismatch_detection(test);
    internal_fragmentation_statistics(test);
    zero_size_and_invalid_alignment(test);
    fallback_failure_and_destruction(test);
    randomized_mixed_allocations(test);
}

}  // namespace segregated_allocator_tests
