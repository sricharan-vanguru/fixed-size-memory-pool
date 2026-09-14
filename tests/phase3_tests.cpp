#include "memory_pool/chunk_manager.hpp"
#include "memory_pool/chunk_pool.hpp"
#include "memory_pool/memory_chunk.hpp"
#include "memory_pool/new_delete_memory_provider.hpp"
#include "test_support.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <stdexcept>

namespace phase3_tests {
namespace {

// Provider injection, stable chunks, growth, reclamation, and exhaustion modes.
class FailureInjectionProvider final : public memory_pool::IMemoryProvider {
public:
    [[nodiscard]] void* allocate(std::size_t bytes,
                                 std::size_t alignment) override {
        ++allocation_calls;
        if (fail_on_call != 0 && allocation_calls == fail_on_call) {
            throw std::bad_alloc{};
        }
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
    std::size_t deallocation_calls{};
    std::size_t fail_on_call{};
};

memory_pool::ChunkManagerOptions options(
    std::size_t blocks,
    memory_pool::GrowthPolicy growth = memory_pool::GrowthPolicy::fixed(),
    std::size_t spare_empty_chunks = 1) {
    return memory_pool::ChunkManagerOptions{
        .block_size = 32,
        .initial_blocks = blocks,
        .alignment = 32,
        .growth = growth,
        .reclamation = {.spare_empty_chunks = spare_empty_chunks},
    };
}

void provider_contract(TestContext& test) {
    memory_pool::NewDeleteMemoryProvider provider;
    test.expect_throws<std::invalid_argument>(
        [&] { static_cast<void>(provider.allocate(0, 8)); },
        "provider should reject zero-byte allocations");
    test.expect_throws<std::invalid_argument>(
        [&] { static_cast<void>(provider.allocate(8, 3)); },
        "provider should reject non-power-of-two alignment");

    void* pointer = provider.allocate(64, 64);
    test.expect(reinterpret_cast<std::uintptr_t>(pointer) % 64 == 0,
                "provider should honor extended alignment");
    provider.deallocate(pointer, 64, 64);
}

void memory_chunk_behavior(TestContext& test) {
    auto provider = std::make_shared<FailureInjectionProvider>();
    memory_pool::MemoryChunk chunk(12, 2, 32, provider);

    test.expect(chunk.block_count() == 2 && chunk.available() == 2 && chunk.empty(),
                "a chunk should expose its initial block state");
    test.expect(chunk.block_size() >= 12 && chunk.storage_size() >= 24,
                "a chunk should expose its normalized storage layout");

    void* first = chunk.allocate();
    void* second = chunk.allocate();
    test.expect(first != nullptr && second != nullptr && chunk.allocate() == nullptr,
                "a chunk should allocate each block exactly once");
    test.expect(chunk.owns(first) && chunk.is_block_start(first),
                "a chunk should identify its block addresses");

    auto* interior = static_cast<std::byte*>(first) + 1;
    test.expect_throws<memory_pool::InvalidPoolPointer>(
        [&] { chunk.deallocate(interior); },
        "a chunk should reject an interior pointer");
    chunk.deallocate(first);
    test.expect_throws<memory_pool::DoubleFreeError>(
        [&] { chunk.deallocate(first); },
        "a chunk should detect a duplicate return");
    chunk.deallocate(second);
    test.expect(chunk.empty(), "a chunk should become empty after all returns");
}

void manager_configuration_validation(TestContext& test) {
    test.expect_throws<std::invalid_argument>(
        [] { memory_pool::ChunkManager manager(options(0)); },
        "a chunk manager should reject zero initial capacity");
    test.expect_throws<std::invalid_argument>(
        [] {
            memory_pool::ChunkManager manager(
                options(2, memory_pool::GrowthPolicy::geometric(1)));
        },
        "geometric growth should reject a factor below two");
    test.expect_throws<std::invalid_argument>(
        [] {
            memory_pool::ChunkManager manager(
                options(4, memory_pool::GrowthPolicy::geometric(2, 2)));
        },
        "a growth cap should not be smaller than initial capacity");
}

void provider_stays_off_normal_path(TestContext& test) {
    auto provider = std::make_shared<FailureInjectionProvider>();
    memory_pool::ChunkManager manager(options(2), provider);
    void* pointer = manager.try_allocate();
    manager.deallocate(pointer);
    pointer = manager.try_allocate();

    test.expect(provider->allocation_calls == 1 &&
                    provider->deallocation_calls == 0,
                "normal block reuse should not call the memory provider");
    manager.deallocate(pointer);
}

void fixed_growth_and_pointer_stability(TestContext& test) {
    memory_pool::ChunkManager manager(options(2));
    auto* first = static_cast<std::uint64_t*>(manager.try_allocate());
    void* second = manager.try_allocate();
    *first = 0x123456789ABCDEF0ULL;

    test.expect(manager.try_allocate() == nullptr,
                "try_allocate should not invoke the provider when exhausted");
    const void* original_address = first;
    manager.grow();
    void* third = manager.try_allocate();

    test.expect(first == original_address && *first == 0x123456789ABCDEF0ULL,
                "growing should preserve existing addresses and contents");
    test.expect(manager.find_chunk(first) != nullptr &&
                    manager.find_chunk(third) != nullptr,
                "manager lookup should find the owning chunk");
    test.expect(manager.chunk_count() == 2 && manager.capacity() == 4,
                "fixed growth should add the initial number of blocks");

    manager.deallocate(first);
    manager.deallocate(second);
    manager.deallocate(third);
}

void geometric_growth(TestContext& test) {
    memory_pool::ChunkManager manager(
        options(2, memory_pool::GrowthPolicy::geometric(2, 8), 3));
    test.expect(manager.next_growth_block_count() == 4,
                "geometric growth should double the next chunk size");
    manager.grow();
    test.expect(manager.capacity() == 6 && manager.next_growth_block_count() == 8,
                "the second chunk should contain four blocks");
    manager.grow();
    test.expect(manager.capacity() == 14 && manager.next_growth_block_count() == 8,
                "geometric growth should stop increasing at its configured maximum");
}

void reclamation_retains_spare_chunks(TestContext& test) {
    auto provider = std::make_shared<FailureInjectionProvider>();
    memory_pool::ChunkManager manager(options(1, {}, 1), provider);
    manager.grow();
    manager.grow();

    void* first = manager.try_allocate();
    void* second = manager.try_allocate();
    void* third = manager.try_allocate();
    manager.deallocate(second);
    test.expect(manager.chunk_count() == 3,
                "the configured spare empty chunk should be retained");
    manager.deallocate(third);
    test.expect(manager.chunk_count() == 2 && provider->deallocation_calls == 1,
                "an empty chunk beyond the spare count should be released");
    manager.deallocate(first);
    test.expect(manager.chunk_count() == 1 && provider->deallocation_calls == 2,
                "reclamation should leave exactly one spare empty chunk");
}

void acquisition_failure_has_strong_guarantee(TestContext& test) {
    auto provider = std::make_shared<FailureInjectionProvider>();
    memory_pool::ChunkManager manager(options(2), provider);
    auto* existing = static_cast<std::uint64_t*>(manager.try_allocate());
    *existing = 77;
    provider->fail_on_call = 2;

    test.expect_throws<std::bad_alloc>([&] { static_cast<void>(manager.grow()); },
                                      "provider failure should escape grow");
    test.expect(manager.chunk_count() == 1 && manager.capacity() == 2 &&
                    manager.find_chunk(existing) != nullptr && *existing == 77,
                "failed growth should preserve existing chunks and allocations");
    test.expect(manager.next_growth_block_count() == 2,
                "failed growth should not advance the growth sequence");
    manager.deallocate(existing);
}

void provider_lifetime_is_owned(TestContext& test) {
    auto provider = std::make_shared<FailureInjectionProvider>();
    std::weak_ptr<memory_pool::IMemoryProvider> lifetime = provider;
    {
        memory_pool::ChunkManager manager(options(1), provider);
        provider.reset();
        test.expect(!lifetime.expired(),
                    "manager and chunks should retain their memory provider");
    }
    test.expect(lifetime.expired(),
                "the provider should be released after the manager and chunks");
}

void exhaustion_policies(TestContext& test) {
    memory_pool::NullableChunkPool nullable(options(1));
    void* nullable_first = nullable.allocate();
    test.expect(nullable.allocate() == nullptr,
                "nullable policy should return null on exhaustion");
    nullable.deallocate(nullable_first);

    memory_pool::ThrowingChunkPool throwing(options(1));
    void* throwing_first = throwing.allocate();
    test.expect_throws<std::bad_alloc>(
        [&] { static_cast<void>(throwing.allocate()); },
        "throwing policy should report exhaustion with bad_alloc");
    throwing.deallocate(throwing_first);

    memory_pool::GrowingChunkPool growing(options(1));
    void* growing_first = growing.allocate();
    void* growing_second = growing.allocate();
    test.expect(growing_first != nullptr && growing_second != nullptr &&
                    growing.chunk_count() == 2,
                "growing policy should acquire a chunk on exhaustion");
    growing.deallocate(growing_first);
    growing.deallocate(growing_second);
}

void heap_fallback_policy(TestContext& test) {
    auto provider = std::make_shared<FailureInjectionProvider>();
    {
        memory_pool::HeapFallbackChunkPool pool(options(1), provider);
        void* pooled = pool.allocate();
        void* fallback = pool.allocate();

        test.expect(pool.owns(pooled) && pool.owns(fallback) &&
                        pool.fallback_allocations() == 1,
                    "heap fallback should track both pool and fallback ownership");
        pool.deallocate(fallback);
        test.expect(pool.fallback_allocations() == 0,
                    "returning a fallback allocation should release its storage");
        int external = 0;
        test.expect_throws<memory_pool::InvalidPoolPointer>(
            [&] { pool.deallocate(&external); },
            "heap fallback should reject an unknown pointer");
        pool.deallocate(pooled);
    }
    test.expect(provider->allocation_calls == provider->deallocation_calls,
                "heap fallback and chunks should release all provider allocations");

    auto cleanup_provider = std::make_shared<FailureInjectionProvider>();
    {
        memory_pool::HeapFallbackChunkPool pool(options(1), cleanup_provider);
        static_cast<void>(pool.allocate());
        static_cast<void>(pool.allocate());
    }
    test.expect(cleanup_provider->allocation_calls ==
                    cleanup_provider->deallocation_calls,
                "pool destruction should release outstanding fallback storage");
}

}  // namespace

void run(TestContext& test) {
    provider_contract(test);
    memory_chunk_behavior(test);
    manager_configuration_validation(test);
    provider_stays_off_normal_path(test);
    fixed_growth_and_pointer_stability(test);
    geometric_growth(test);
    reclamation_retains_spare_chunks(test);
    acquisition_failure_has_strong_guarantee(test);
    provider_lifetime_is_owned(test);
    exhaustion_policies(test);
    heap_fallback_policy(test);
}

}  // namespace phase3_tests
