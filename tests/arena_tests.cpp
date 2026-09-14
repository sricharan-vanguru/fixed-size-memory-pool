#include "memory_pool/monotonic_arena.hpp"
#include "memory_pool/new_delete_memory_provider.hpp"
#include "test_support.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <stdexcept>
#include <vector>

namespace arena_tests {
namespace {

// Alignment, growth, reset retention, destruction order, and failure recovery.
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

memory_pool::MonotonicArenaOptions arena_options(
    std::size_t initial_size = 32,
    memory_pool::ArenaGrowthPolicy growth =
        memory_pool::ArenaGrowthPolicy::geometric(2, 128),
    memory_pool::ArenaResetPolicy reset_policy =
        memory_pool::ArenaResetPolicy::retain_all_chunks) {
    return {
        .initial_chunk_size = initial_size,
        .initial_alignment = alignof(std::max_align_t),
        .growth = growth,
        .reset_policy = reset_policy,
    };
}

void configuration_validation(TestContext& test) {
    test.expect_throws<std::invalid_argument>(
        [] {
            memory_pool::MonotonicArena arena({.initial_chunk_size = 0});
        },
        "an arena should reject a zero initial chunk size");
    test.expect_throws<std::invalid_argument>(
        [] {
            memory_pool::MonotonicArena arena({.initial_alignment = 0});
        },
        "an arena should reject zero initial alignment");
    test.expect_throws<std::invalid_argument>(
        [] {
            memory_pool::MonotonicArena arena({.initial_alignment = 3});
        },
        "an arena should reject non-power-of-two initial alignment");
    test.expect_throws<std::invalid_argument>(
        [] {
            memory_pool::MonotonicArena arena(
                arena_options(32,
                              memory_pool::ArenaGrowthPolicy::geometric(1, 128)));
        },
        "geometric arena growth should reject a factor below two");
    test.expect_throws<std::invalid_argument>(
        [] {
            memory_pool::MonotonicArena arena(
                arena_options(64,
                              memory_pool::ArenaGrowthPolicy::geometric(2, 32)));
        },
        "an arena growth cap should not be smaller than its initial chunk");
}

void aligned_bump_allocation(TestContext& test) {
    memory_pool::MonotonicArena arena(
        arena_options(64, memory_pool::ArenaGrowthPolicy::fixed()));
    void* const first = arena.allocate(1, 1);
    void* const second = arena.allocate(8, 16);
    void* const zero = arena.allocate(0, 8);
    void* const over_aligned = arena.allocate(16, 256);

    test.expect(reinterpret_cast<std::uintptr_t>(second) % 16 == 0 &&
                    reinterpret_cast<std::uintptr_t>(zero) % 8 == 0 &&
                    reinterpret_cast<std::uintptr_t>(over_aligned) % 256 == 0,
                "arena allocations should preserve every requested alignment");
    test.expect(first != second && second != zero && arena.chunk_count() == 2,
                "bump allocation should return unique live regions and grow for over-alignment");
    int external = 0;
    test.expect(arena.owns(first) && arena.owns(over_aligned) &&
                    !arena.owns(nullptr) && !arena.owns(&external),
                "arena ownership should cover every backing chunk");
    test.expect_throws<std::invalid_argument>(
        [&] { static_cast<void>(arena.allocate(8, 3)); },
        "arena allocation should reject non-power-of-two alignment");

    const auto& statistics = arena.statistics();
    test.expect(statistics.allocation_requests == 5 &&
                    statistics.successful_allocations == 4 &&
                    statistics.failed_allocations == 1,
                "arena statistics should include valid and invalid requests");
    test.expect(statistics.current_used_bytes == arena.bytes_used() &&
                    statistics.current_reserved_bytes == arena.bytes_reserved() &&
                    statistics.padding_bytes > 0,
                "arena statistics should report used, reserved, and padding bytes");
}

void fixed_growth_and_oversized_chunks(TestContext& test) {
    memory_pool::MonotonicArena fixed_arena(
        arena_options(32, memory_pool::ArenaGrowthPolicy::fixed()));
    static_cast<void>(fixed_arena.allocate(32, 1));
    static_cast<void>(fixed_arena.allocate(1, 1));
    test.expect(fixed_arena.chunk_count() == 2 &&
                    fixed_arena.bytes_reserved() == 64,
                "fixed growth should add chunks with the initial capacity");

    memory_pool::MonotonicArena capped_arena(arena_options());
    static_cast<void>(capped_arena.allocate(32, 1));
    void* const oversized = capped_arena.allocate(200, 64);
    test.expect(capped_arena.chunk_count() == 2 &&
                    capped_arena.bytes_reserved() == 232 &&
                    reinterpret_cast<std::uintptr_t>(oversized) % 64 == 0,
                "a request above the growth cap should receive a dedicated fitting chunk");
}

void geometric_growth_and_pointer_stability(TestContext& test) {
    memory_pool::MonotonicArena arena(arena_options());
    auto* const first = static_cast<std::uint64_t*>(arena.allocate(32, 8));
    *first = 0x123456789ABCDEF0ULL;
    void* const second = arena.allocate(1, 1);
    void* const third = arena.allocate(64, 1);

    test.expect(arena.chunk_count() == 3 && arena.bytes_reserved() == 224,
                "geometric growth should create 32, 64, and 128-byte chunks");
    test.expect(*first == 0x123456789ABCDEF0ULL && arena.owns(second) &&
                    arena.owns(third),
                "arena growth should preserve existing addresses and contents");
    test.expect(arena.statistics().peak_chunks == 3 &&
                    arena.statistics().chunk_allocations == 3,
                "arena statistics should track growth and peak chunks");
}

void retain_all_reuses_chunks(TestContext& test) {
    memory_pool::MonotonicArena arena(arena_options());
    void* const first = arena.allocate(32, 1);
    void* const second = arena.allocate(1, 1);
    void* const third = arena.allocate(64, 1);
    const std::size_t reserved = arena.bytes_reserved();

    arena.reset();
    test.expect(arena.chunk_count() == 3 && arena.bytes_used() == 0 &&
                    arena.bytes_reserved() == reserved,
                "retain-all reset should rewind without releasing chunks");
    test.expect(arena.allocate(32, 1) == first &&
                    arena.allocate(1, 1) == second &&
                    arena.allocate(64, 1) == third,
                "retain-all reset should reuse chunk addresses in the same order");

    arena.reset();
    arena.reset();
    test.expect(arena.bytes_used() == 0 && arena.chunk_count() == 3 &&
                    arena.statistics().reset_calls == 3,
                "repeated reset should remain safe and preserve retained chunks");
}

void retain_initial_releases_extra_chunks(TestContext& test) {
    auto provider = std::make_shared<TrackingProvider>();
    {
        memory_pool::MonotonicArena arena(
            arena_options(
                32,
                memory_pool::ArenaGrowthPolicy::geometric(2, 128),
                memory_pool::ArenaResetPolicy::retain_initial_chunk),
            provider);
        static_cast<void>(arena.allocate(32, 1));
        static_cast<void>(arena.allocate(64, 1));
        test.expect(arena.chunk_count() == 2 && provider->allocation_calls == 2,
                    "an oversized request should add a second arena chunk");

        arena.reset();
        test.expect(arena.chunk_count() == 1 && arena.bytes_reserved() == 32 &&
                        provider->deallocation_calls == 1,
                    "retain-initial reset should immediately release extra chunks");
        test.expect(arena.statistics().chunk_releases == 1,
                    "arena statistics should count reset-time chunk release");
    }
    test.expect(provider->successful_allocations == provider->deallocation_calls,
                "arena destruction should return the retained initial chunk");
}

struct TrackedObject {
    TrackedObject(int object_id, std::vector<int>& destruction_log)
        : id(object_id), log(&destruction_log) {}

    ~TrackedObject() noexcept { log->push_back(id); }

    int id;
    std::vector<int>* log;
};

void destructor_policy(TestContext& test) {
    std::vector<int> destruction_log;
    destruction_log.reserve(6);
    {
        memory_pool::MonotonicArena arena(arena_options(128));
        static_cast<void>(arena.create<TrackedObject>(1, destruction_log));
        static_cast<void>(arena.create<TrackedObject>(2, destruction_log));
        static_cast<void>(arena.create<TrackedObject>(3, destruction_log));
        arena.reset();

        test.expect(destruction_log == std::vector<int>({3, 2, 1}),
                    "reset should destroy arena-managed objects in reverse order");
        test.expect(arena.statistics().object_constructions == 3 &&
                        arena.statistics().destructor_calls == 3,
                    "typed arena statistics should track construction and destruction");

        static_cast<void>(arena.create<TrackedObject>(4, destruction_log));
        static_cast<void>(arena.create<int>(42));
    }
    test.expect(destruction_log == std::vector<int>({3, 2, 1, 4}),
                "arena destruction should destroy objects created after the last reset");
}

struct ThrowingObject {
    ThrowingObject() {
        attempted_address = this;
        throw std::runtime_error("construction failed");
    }

    static inline void* attempted_address{};
};

void constructor_failure_rolls_back(TestContext& test) {
    memory_pool::MonotonicArena arena(arena_options(64));
    test.expect_throws<std::runtime_error>(
        [&] { static_cast<void>(arena.create<ThrowingObject>()); },
        "a throwing arena-managed constructor should propagate");
    test.expect(arena.bytes_used() == 0 &&
                    arena.statistics().construction_failures == 1,
                "constructor failure should rewind the most recent bump allocation");

    void* const reused = arena.allocate(
        sizeof(ThrowingObject), alignof(ThrowingObject));
    test.expect(reused == ThrowingObject::attempted_address,
                "the allocation after constructor failure should reuse rolled-back storage");
}

struct NestedThrowingObject {
    explicit NestedThrowingObject(memory_pool::MonotonicArena& arena) {
        static_cast<void>(arena.allocate(8, 1));
        throw std::runtime_error("construction failed after nested allocation");
    }
};

void constructor_failure_after_nested_allocation(TestContext& test) {
    memory_pool::MonotonicArena arena(arena_options(64));
    auto* const existing = static_cast<std::uint64_t*>(arena.allocate(8, 8));
    *existing = 123;
    const std::size_t used_before = arena.bytes_used();

    test.expect_throws<std::runtime_error>(
        [&] { static_cast<void>(arena.create<NestedThrowingObject>(arena)); },
        "a constructor that allocates from the same arena should still propagate");
    test.expect(*existing == 123 && arena.owns(existing) &&
                    arena.bytes_used() > used_before &&
                    arena.statistics().construction_failures == 1,
                "nested constructor failure should preserve earlier allocations and defer unrecoverable bump space to reset");

    arena.reset();
    test.expect(arena.bytes_used() == 0,
                "reset should recover storage retained after nested construction failure");
}

struct alignas(256) OverAlignedObject {
    explicit OverAlignedObject(std::uint64_t initial_value) : value(initial_value) {}
    std::uint64_t value;
};

void over_aligned_typed_object(TestContext& test) {
    memory_pool::MonotonicArena arena(arena_options(64));
    OverAlignedObject* const object = arena.create<OverAlignedObject>(77);
    test.expect(reinterpret_cast<std::uintptr_t>(object) %
                        alignof(OverAlignedObject) ==
                    0 &&
                    object->value == 77,
                "create<T> should support over-aligned arena-managed objects");
    arena.reset();
}

void growth_failure_preserves_existing_storage(TestContext& test) {
    auto provider = std::make_shared<TrackingProvider>();
    memory_pool::MonotonicArena arena(arena_options(32), provider);
    auto* const first = static_cast<std::uint64_t*>(arena.allocate(32, 8));
    *first = 99;
    provider->fail_on_call = 2;

    test.expect_throws<std::bad_alloc>(
        [&] { static_cast<void>(arena.allocate(1, 1)); },
        "arena growth should propagate provider failure");
    test.expect(arena.chunk_count() == 1 && arena.owns(first) && *first == 99,
                "failed arena growth should preserve existing chunks and contents");
    test.expect(arena.statistics().failed_allocations == 1,
                "arena growth failure should update failure statistics");
}

void provider_lifetime_is_owned(TestContext& test) {
    auto provider = std::make_shared<TrackingProvider>();
    std::weak_ptr<memory_pool::IMemoryProvider> lifetime = provider;
    {
        memory_pool::MonotonicArena arena(arena_options(), provider);
        provider.reset();
        test.expect(!lifetime.expired(),
                    "the arena and its chunks should retain their memory provider");
    }
    test.expect(lifetime.expired(),
                "the provider should be released after the arena and chunks");
}

}  // namespace

void run(TestContext& test) {
    configuration_validation(test);
    aligned_bump_allocation(test);
    fixed_growth_and_oversized_chunks(test);
    geometric_growth_and_pointer_stability(test);
    retain_all_reuses_chunks(test);
    retain_initial_releases_extra_chunks(test);
    destructor_policy(test);
    constructor_failure_rolls_back(test);
    constructor_failure_after_nested_allocation(test);
    over_aligned_typed_object(test);
    growth_failure_preserves_existing_storage(test);
    provider_lifetime_is_owned(test);
}

}  // namespace arena_tests
