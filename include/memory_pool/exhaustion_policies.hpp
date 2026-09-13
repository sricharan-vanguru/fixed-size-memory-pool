#pragma once

#include "memory_pool/chunk_manager.hpp"

#include <cstddef>
#include <new>
#include <unordered_set>

namespace memory_pool {

class ReturnNullOnExhaustion {
public:
    [[nodiscard]] void* on_exhaustion(ChunkManager&) const noexcept {
        return nullptr;
    }
    [[nodiscard]] bool owns(const void*) const noexcept { return false; }
    [[nodiscard]] bool try_deallocate(void*, ChunkManager&) noexcept { return false; }
    void release_all(ChunkManager&) noexcept {}
    [[nodiscard]] std::size_t fallback_allocations() const noexcept { return 0; }
};

class ThrowOnExhaustion {
public:
    [[noreturn]] void* on_exhaustion(ChunkManager&) const { throw std::bad_alloc{}; }
    [[nodiscard]] bool owns(const void*) const noexcept { return false; }
    [[nodiscard]] bool try_deallocate(void*, ChunkManager&) noexcept { return false; }
    void release_all(ChunkManager&) noexcept {}
    [[nodiscard]] std::size_t fallback_allocations() const noexcept { return 0; }
};

class GrowOnExhaustion {
public:
    [[nodiscard]] void* on_exhaustion(ChunkManager& manager) const {
        return manager.grow().allocate();
    }
    [[nodiscard]] bool owns(const void*) const noexcept { return false; }
    [[nodiscard]] bool try_deallocate(void*, ChunkManager&) noexcept { return false; }
    void release_all(ChunkManager&) noexcept {}
    [[nodiscard]] std::size_t fallback_allocations() const noexcept { return 0; }
};

class HeapFallbackOnExhaustion {
public:
    [[nodiscard]] void* on_exhaustion(ChunkManager& manager) {
        void* const pointer = manager.memory_provider().allocate(
            manager.block_size(), manager.alignment());
        try {
            fallback_allocations_.insert(pointer);
        } catch (...) {
            manager.memory_provider().deallocate(
                pointer, manager.block_size(), manager.alignment());
            throw;
        }
        return pointer;
    }

    [[nodiscard]] bool owns(const void* pointer) const noexcept {
        return fallback_allocations_.contains(const_cast<void*>(pointer));
    }

    [[nodiscard]] bool try_deallocate(void* pointer, ChunkManager& manager) noexcept {
        const auto allocation = fallback_allocations_.find(pointer);
        if (allocation == fallback_allocations_.end()) {
            return false;
        }
        manager.memory_provider().deallocate(
            pointer, manager.block_size(), manager.alignment());
        fallback_allocations_.erase(allocation);
        return true;
    }

    void release_all(ChunkManager& manager) noexcept {
        for (void* pointer : fallback_allocations_) {
            manager.memory_provider().deallocate(
                pointer, manager.block_size(), manager.alignment());
        }
        fallback_allocations_.clear();
    }

    [[nodiscard]] std::size_t fallback_allocations() const noexcept {
        return fallback_allocations_.size();
    }

private:
    std::unordered_set<void*> fallback_allocations_;
};

}  // namespace memory_pool
