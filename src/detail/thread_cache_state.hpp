#pragma once

#include "memory_pool/concurrency_options.hpp"
#include "memory_pool/concurrency_statistics.hpp"
#include "memory_pool/memory_provider.hpp"
#include "memory_pool/segregated_allocator.hpp"
#include "memory_pool/segregated_allocator_options.hpp"
#include "memory_pool/segregated_allocator_statistics.hpp"
#include "memory_pool/size_class_selector.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace memory_pool::detail {

class ThreadCacheState {
  public:
    // A separate vector per size class belongs exclusively to one thread.
    using CacheBins = std::vector<std::vector<void*>>;

    ThreadCacheState(SegregatedAllocatorOptions allocator_options,
                     ThreadCacheOptions cache_options,
                     MemoryProviderPtr provider);

    [[nodiscard]] std::uint64_t id() const noexcept;
    [[nodiscard]] std::size_t class_count() const noexcept;
    [[nodiscard]] std::size_t cache_capacity() const noexcept;
    [[nodiscard]] void*
    allocate(CacheBins& bins, std::size_t size, std::size_t alignment);
    void deallocate(CacheBins& bins,
                    void* pointer,
                    const std::size_t* size,
                    const std::size_t* alignment);
    [[nodiscard]] std::size_t flush_bins(CacheBins& bins);
    [[nodiscard]] bool owns(const void* pointer) const;
    [[nodiscard]] std::optional<std::size_t>
    owning_size_class(const void* pointer) const;
    [[nodiscard]] ThreadCacheStatistics snapshot() const noexcept;
    [[nodiscard]] SegregatedAllocatorStatistics central_snapshot() const;

  private:
    static constexpr std::size_t shard_count = 64;
    // No valid size-class index can equal this sentinel.
    static constexpr std::size_t fallback_class = static_cast<std::size_t>(-1);

    struct AllocationRecord {
        // Records remain present while a pooled block is cached; `active`
        // distinguishes a live allocation from a locally cached free block.
        std::size_t class_index{};
        std::size_t requested_size{};
        std::size_t alignment{};
        std::uint64_t owner_thread{};
        bool active{};
    };

    struct RecordShard {
        // Pointer hashing spreads ownership lookups across independent locks.
        mutable std::mutex mutex;
        std::unordered_map<void*, AllocationRecord> records;
    };

    struct AtomicStatistics {
        std::atomic<std::size_t> allocation_requests{};
        std::atomic<std::size_t> deallocation_requests{};
        std::atomic<std::size_t> cache_hits{};
        std::atomic<std::size_t> central_allocations{};
        std::atomic<std::size_t> central_deallocations{};
        std::atomic<std::size_t> local_deallocations{};
        std::atomic<std::size_t> remote_deallocations{};
        std::atomic<std::size_t> batch_refills{};
        std::atomic<std::size_t> batch_flushes{};
        std::atomic<std::size_t> cached_blocks{};
        std::atomic<std::size_t> peak_cached_blocks{};
        std::atomic<std::size_t> live_allocations{};
        std::atomic<std::size_t> peak_live_allocations{};
    };

    [[nodiscard]] RecordShard& shard_for(const void* pointer) noexcept;
    [[nodiscard]] const RecordShard& shard_for(const void* pointer) const noexcept;
    [[nodiscard]] void* central_allocate(std::size_t size, std::size_t alignment);
    void central_deallocate(void* pointer);
    void central_deallocate(void* pointer, std::size_t size, std::size_t alignment);
    void record_allocation(void* pointer, AllocationRecord record);
    void validate_sized_deallocation(const AllocationRecord& record,
                                     std::size_t size,
                                     std::size_t alignment) const;
    [[nodiscard]] std::size_t flush_bin(std::vector<void*>& bin,
                                        std::size_t target_size);
    void note_live_allocation() noexcept;

    std::uint64_t id_;
    ThreadCacheOptions cache_options_;
    SizeClassSelector selector_;
    // SegregatedAllocator is intentionally unsynchronized, so every direct
    // central operation is serialized by this one mutex.
    mutable std::mutex central_mutex_;
    SegregatedAllocator central_;
    std::array<RecordShard, shard_count> shards_;
    AtomicStatistics statistics_;
};

}  // namespace memory_pool::detail
