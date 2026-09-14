#pragma once

#include "memory_pool/chunk_manager.hpp"
#include "memory_pool/exhaustion_policies.hpp"
#include "memory_pool/pool_errors.hpp"

#include <cstddef>
#include <utility>

namespace memory_pool {

/// Combines stable chunk storage with one compile-time exhaustion strategy.
/// For example, GrowingChunkPool grows whereas NullableChunkPool returns null.
template <typename ExhaustionPolicy>
class BasicChunkPool {
public:
    explicit BasicChunkPool(ChunkManagerOptions options,
                            MemoryProviderPtr provider = {},
                            ExhaustionPolicy exhaustion_policy = {})
        : manager_(options, std::move(provider)),
          exhaustion_policy_(std::move(exhaustion_policy)) {}

    ~BasicChunkPool() { exhaustion_policy_.release_all(manager_); }

    BasicChunkPool(const BasicChunkPool&) = delete;
    BasicChunkPool& operator=(const BasicChunkPool&) = delete;
    BasicChunkPool(BasicChunkPool&&) = delete;
    BasicChunkPool& operator=(BasicChunkPool&&) = delete;

    [[nodiscard]] void* allocate() {
        // Provider work is avoided while an existing chunk still has a block.
        if (void* pointer = manager_.try_allocate(); pointer != nullptr) {
            return pointer;
        }
        return exhaustion_policy_.on_exhaustion(manager_);
    }

    void deallocate(void* pointer) {
        if (pointer == nullptr) {
            return;
        }
        if (manager_.find_chunk(pointer) != nullptr) {
            manager_.deallocate(pointer);
            return;
        }
        // Heap-fallback allocations are owned by the policy, not by a chunk.
        if (!exhaustion_policy_.try_deallocate(pointer, manager_)) {
            throw InvalidPoolPointer("pointer is not owned by this chunk pool");
        }
    }

    [[nodiscard]] bool owns(const void* pointer) const noexcept {
        return manager_.find_chunk(pointer) != nullptr ||
               exhaustion_policy_.owns(pointer);
    }

    [[nodiscard]] std::size_t chunk_count() const noexcept {
        return manager_.chunk_count();
    }
    [[nodiscard]] std::size_t capacity() const noexcept { return manager_.capacity(); }
    [[nodiscard]] std::size_t available() const noexcept { return manager_.available(); }
    [[nodiscard]] std::size_t in_use() const noexcept { return manager_.in_use(); }
    [[nodiscard]] std::size_t fallback_allocations() const noexcept {
        return exhaustion_policy_.fallback_allocations();
    }
    [[nodiscard]] ChunkManager& chunk_manager() noexcept { return manager_; }
    [[nodiscard]] const ChunkManager& chunk_manager() const noexcept { return manager_; }

private:
    ChunkManager manager_;
    ExhaustionPolicy exhaustion_policy_;
};

using NullableChunkPool = BasicChunkPool<ReturnNullOnExhaustion>;
using ThrowingChunkPool = BasicChunkPool<ThrowOnExhaustion>;
using GrowingChunkPool = BasicChunkPool<GrowOnExhaustion>;
using HeapFallbackChunkPool = BasicChunkPool<HeapFallbackOnExhaustion>;

}  // namespace memory_pool
