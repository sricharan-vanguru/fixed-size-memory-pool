#pragma once

#include "memory_pool/memory_provider.hpp"
#include "memory_pool/segregated_allocator.hpp"
#include "memory_pool/segregated_allocator_options.hpp"
#include "memory_pool/segregated_allocator_statistics.hpp"

#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace memory_pool {

/// Serializes access to one otherwise unsynchronized SegregatedAllocator.
/// Mutex must meet the C++ BasicLockable requirements.
template <typename Mutex = std::mutex>
class BasicSynchronizedAllocator {
public:
    using mutex_type = Mutex;

    explicit BasicSynchronizedAllocator(
        SegregatedAllocatorOptions options = {},
        MemoryProviderPtr provider = {})
        : allocator_(std::move(options), std::move(provider)) {}

    BasicSynchronizedAllocator(const BasicSynchronizedAllocator&) = delete;
    BasicSynchronizedAllocator& operator=(const BasicSynchronizedAllocator&) = delete;
    BasicSynchronizedAllocator(BasicSynchronizedAllocator&&) = delete;
    BasicSynchronizedAllocator& operator=(BasicSynchronizedAllocator&&) = delete;

    [[nodiscard]] void* allocate(
        std::size_t size,
        std::size_t alignment = alignof(std::max_align_t)) {
        const std::lock_guard<Mutex> lock(mutex_);
        return allocator_.allocate(size, alignment);
    }

    void deallocate(void* pointer) {
        const std::lock_guard<Mutex> lock(mutex_);
        allocator_.deallocate(pointer);
    }

    void deallocate(void* pointer,
                    std::size_t size,
                    std::size_t alignment) {
        const std::lock_guard<Mutex> lock(mutex_);
        allocator_.deallocate(pointer, size, alignment);
    }

    [[nodiscard]] bool owns(const void* pointer) const {
        const std::lock_guard<Mutex> lock(mutex_);
        return allocator_.owns(pointer);
    }

    [[nodiscard]] std::optional<std::size_t> owning_size_class(
        const void* pointer) const {
        const std::lock_guard<Mutex> lock(mutex_);
        return allocator_.owning_size_class(pointer);
    }

    [[nodiscard]] std::vector<std::size_t> size_classes() const {
        const std::lock_guard<Mutex> lock(mutex_);
        const auto& classes = allocator_.selector().size_classes();
        // Return a copy because exposing the allocator's span after unlocking
        // would make the synchronization boundary unclear.
        return {classes.begin(), classes.end()};
    }

    [[nodiscard]] SegregatedAllocatorStatistics statistics() const {
        const std::lock_guard<Mutex> lock(mutex_);
        return allocator_.statistics();
    }

private:
    mutable Mutex mutex_;
    SegregatedAllocator allocator_;
};

using SynchronizedAllocator = BasicSynchronizedAllocator<>;

}  // namespace memory_pool
