#include "memory_pool/pool_errors.hpp"
#include "memory_pool/pool_memory_resource.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <memory_resource>
#include <new>
#include <numeric>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pmr_tests {
namespace {

// Standard containers, resource identity, upstream behavior, and lifetimes.
memory_pool::SegregatedAllocatorOptions
pmr_options(std::vector<std::size_t> classes =
                memory_pool::SizeClassSelector::default_size_classes()) {
    return memory_pool::SegregatedAllocatorOptions{
        .size_classes = std::move(classes),
        .initial_blocks_per_class = 2,
        .growth = memory_pool::GrowthPolicy::geometric(2, 64),
        .reclamation = {.spare_empty_chunks = 1},
    };
}

class TrackingMemoryResource final : public std::pmr::memory_resource {
  public:
    std::size_t allocation_calls{};
    std::size_t successful_allocations{};
    std::size_t deallocation_calls{};
    bool fail_next_allocation{false};

  private:
    [[nodiscard]] void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        ++allocation_calls;
        if (fail_next_allocation) {
            fail_next_allocation = false;
            throw std::bad_alloc{};
        }
        ++successful_allocations;
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }

    void
    do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        ++deallocation_calls;
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }

    [[nodiscard]] bool
    do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

struct ContainerSnapshot {
    std::vector<int> vector_values;
    std::string text;
    int list_total{};
    std::size_t map_size{};
    int map_total{};

    bool operator==(const ContainerSnapshot&) const = default;
};

ContainerSnapshot exercise_containers(std::pmr::memory_resource* resource) {
    std::pmr::vector<int> values(resource);
    for (int value = 0; value < 100; ++value) {
        values.push_back(value * 3);
    }

    std::pmr::string text(resource);
    text.assign(200, 'p');

    std::pmr::list<int> nodes(resource);
    for (int value = 1; value <= 25; ++value) {
        nodes.push_back(value);
    }

    std::pmr::unordered_map<int, int> lookup(resource);
    for (int value = 0; value < 40; ++value) {
        lookup.emplace(value, value * value);
    }

    int map_total = 0;
    for (const auto& [key, value] : lookup) {
        static_cast<void>(key);
        map_total += value;
    }

    return ContainerSnapshot{
        .vector_values = {values.begin(), values.end()},
        .text = {text.data(), text.size()},
        .list_total = std::accumulate(nodes.begin(), nodes.end(), 0),
        .map_size = lookup.size(),
        .map_total = map_total,
    };
}

void pmr_container_behavior(TestContext& test) {
    memory_pool::PoolMemoryResource custom(pmr_options());
    std::pmr::unsynchronized_pool_resource standard;

    const ContainerSnapshot custom_result = exercise_containers(&custom);
    const ContainerSnapshot standard_result = exercise_containers(&standard);

    test.expect(
        custom_result == standard_result,
        "custom and standard PMR pools should produce identical container results");
    test.expect(
        custom.statistics().successful_allocations > 0 &&
            custom.statistics().successful_allocations ==
                custom.statistics().deallocations,
        "vector, string, list, and unordered_map should return all allocations");
}

void equality_and_allocator_propagation(TestContext& test) {
    memory_pool::PoolMemoryResource first(pmr_options());
    memory_pool::PoolMemoryResource second(pmr_options());

    test.expect(first.is_equal(first),
                "do_is_equal should compare a PMR resource equal to itself");
    test.expect(!first.is_equal(second) && !second.is_equal(first),
                "independent stateful pool resources should not compare equal");
    test.expect(first != *std::pmr::new_delete_resource(),
                "the pool resource should not equal an unrelated upstream resource");

    std::pmr::polymorphic_allocator<int> first_allocator(&first);
    std::pmr::polymorphic_allocator<int> matching_allocator(&first);
    std::pmr::polymorphic_allocator<int> second_allocator(&second);
    test.expect(first_allocator == matching_allocator &&
                    first_allocator != second_allocator,
                "polymorphic allocator equality should follow resource identity");

    std::pmr::vector<int> source(&first);
    source.assign({1, 2, 3, 4});
    std::pmr::vector<int> destination(&second);
    destination = std::move(source);
    test.expect(destination == std::pmr::vector<int>({1, 2, 3, 4}, &second) &&
                    destination.get_allocator().resource() == &second,
                "move assignment should retain the destination PMR resource");
}

struct alignas(256) OverAlignedValue {
    std::uint64_t value{};
};

void polymorphic_allocator_alignment(TestContext& test) {
    memory_pool::PoolMemoryResource resource(pmr_options());
    std::pmr::polymorphic_allocator<OverAlignedValue> allocator(&resource);
    OverAlignedValue* values = allocator.allocate(2);

    std::construct_at(values, OverAlignedValue{11});
    std::construct_at(values + 1, OverAlignedValue{22});
    test.expect(reinterpret_cast<std::uintptr_t>(values) % alignof(OverAlignedValue) ==
                    0,
                "polymorphic_allocator should preserve over-aligned element alignment");
    test.expect(
        values[0].value == 11 && values[1].value == 22,
        "polymorphic_allocator should support contiguous multi-element storage");

    std::destroy_at(values);
    std::destroy_at(values + 1);
    allocator.deallocate(values, 2);
}

void upstream_fallback_and_cleanup(TestContext& test) {
    TrackingMemoryResource upstream;
    {
        memory_pool::PoolMemoryResource resource(pmr_options({8, 16, 32, 64}),
                                                 &upstream);
        test.expect(resource.upstream_resource() == &upstream,
                    "the configured upstream resource should be observable");

        void* large = resource.allocate(128, 64);
        void* over_aligned = resource.allocate(16, 256);
        test.expect(reinterpret_cast<std::uintptr_t>(large) % 64 == 0 &&
                        reinterpret_cast<std::uintptr_t>(over_aligned) % 256 == 0,
                    "upstream fallback should preserve size-specific alignment");
        test.expect(resource.statistics().current_fallback_allocations == 2,
                    "PMR fallback should be visible in allocator statistics");

        resource.deallocate(large, 128, 64);
        resource.deallocate(over_aligned, 16, 256);
    }
    test.expect(upstream.successful_allocations == upstream.deallocation_calls,
                "resource destruction should return every chunk to the upstream");
}

void upstream_failure_and_deallocation_validation(TestContext& test) {
    TrackingMemoryResource upstream;
    memory_pool::PoolMemoryResource resource(pmr_options({8, 16, 32, 64}), &upstream);
    upstream.fail_next_allocation = true;

    test.expect_throws<std::bad_alloc>(
        [&] { static_cast<void>(resource.allocate(128, 64)); },
        "an upstream allocation failure should propagate through PMR");
    test.expect(resource.statistics().failed_allocations == 1,
                "upstream failures should be counted by the allocator");

    void* fallback = resource.allocate(128, 64);
    test.expect_throws<memory_pool::AllocationMismatchError>(
        [&] { resource.deallocate(fallback, 129, 64); },
        "PMR deallocation should validate fallback size metadata");
    resource.deallocate(fallback, 128, 64);

    test.expect_throws<std::invalid_argument>(
        [] { memory_pool::PoolMemoryResource invalid(pmr_options(), nullptr); },
        "a null upstream resource should be rejected");
}

}  // namespace

void run(TestContext& test) {
    pmr_container_behavior(test);
    equality_and_allocator_propagation(test);
    polymorphic_allocator_alignment(test);
    upstream_fallback_and_cleanup(test);
    upstream_failure_and_deallocation_validation(test);
}

}  // namespace pmr_tests
