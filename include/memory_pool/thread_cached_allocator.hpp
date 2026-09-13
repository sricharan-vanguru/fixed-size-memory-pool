#pragma once

#include "memory_pool/concurrency_options.hpp"
#include "memory_pool/concurrency_statistics.hpp"
#include "memory_pool/memory_provider.hpp"
#include "memory_pool/segregated_allocator_options.hpp"
#include "memory_pool/segregated_allocator_statistics.hpp"

#include <cstddef>
#include <memory>
#include <optional>

namespace memory_pool {

/// A shared allocator with per-thread small-block caches. The allocator must
/// outlive all calls, and its destructor must not race with worker threads.
class ThreadCachedAllocator {
public:
    explicit ThreadCachedAllocator(
        SegregatedAllocatorOptions allocator_options = {},
        ThreadCacheOptions cache_options = {},
        MemoryProviderPtr provider = {});
    ~ThreadCachedAllocator();

    ThreadCachedAllocator(const ThreadCachedAllocator&) = delete;
    ThreadCachedAllocator& operator=(const ThreadCachedAllocator&) = delete;
    ThreadCachedAllocator(ThreadCachedAllocator&&) = delete;
    ThreadCachedAllocator& operator=(ThreadCachedAllocator&&) = delete;

    [[nodiscard]] void* allocate(
        std::size_t size,
        std::size_t alignment = alignof(std::max_align_t));
    void deallocate(void* pointer);
    void deallocate(void* pointer,
                    std::size_t size,
                    std::size_t alignment);

    /// Returns cached blocks owned by the calling thread to central storage.
    [[nodiscard]] std::size_t release_current_thread_cache();

    [[nodiscard]] bool owns(const void* pointer) const;
    [[nodiscard]] std::optional<std::size_t> owning_size_class(
        const void* pointer) const;
    [[nodiscard]] ThreadCacheStatistics statistics() const noexcept;
    [[nodiscard]] SegregatedAllocatorStatistics central_statistics() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace memory_pool
