#include "thread_cache_state.hpp"

#include "memory_pool/pool_errors.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>

namespace memory_pool::detail {
namespace {

std::atomic<std::uint64_t> next_allocator_id{1};
std::atomic<std::uint64_t> next_thread_id{1};

std::uint64_t current_thread_id() noexcept {
    // A small stable integer is cheaper to store than std::thread::id and is
    // sufficient for comparing allocation and deallocation threads.
    thread_local const std::uint64_t id =
        next_thread_id.fetch_add(1, std::memory_order_relaxed);
    return id;
}

ThreadCacheOptions validate_cache_options(ThreadCacheOptions options) {
    if (options.high_watermark == 0) {
        throw std::invalid_argument("high_watermark must be greater than zero");
    }
    if (options.high_watermark == std::numeric_limits<std::size_t>::max()) {
        throw std::invalid_argument(
            "high_watermark is too large to reserve cache overflow space");
    }
    if (options.low_watermark > options.high_watermark) {
        throw std::invalid_argument(
            "low_watermark cannot exceed high_watermark");
    }
    if (options.refill_batch == 0 ||
        options.refill_batch > options.high_watermark) {
        throw std::invalid_argument(
            "refill_batch must be between one and high_watermark");
    }
    return options;
}

void update_peak(std::atomic<std::size_t>& peak,
                 std::size_t candidate) noexcept {
    // Relaxed ordering is enough: counters provide telemetry, not synchronization.
    std::size_t observed = peak.load(std::memory_order_relaxed);
    while (observed < candidate &&
           !peak.compare_exchange_weak(observed,
                                       candidate,
                                       std::memory_order_relaxed,
                                       std::memory_order_relaxed)) {
    }
}

std::size_t normalized_size(std::size_t size) noexcept {
    return std::max<std::size_t>(size, 1);
}

}  // namespace

ThreadCacheState::ThreadCacheState(
    SegregatedAllocatorOptions allocator_options,
    ThreadCacheOptions cache_options,
    MemoryProviderPtr provider)
    : id_(next_allocator_id.fetch_add(1, std::memory_order_relaxed)),
      cache_options_(validate_cache_options(cache_options)),
      selector_(allocator_options.size_classes),
      central_(std::move(allocator_options), std::move(provider)) {}

std::uint64_t ThreadCacheState::id() const noexcept { return id_; }

std::size_t ThreadCacheState::class_count() const noexcept {
    return selector_.class_count();
}

std::size_t ThreadCacheState::cache_capacity() const noexcept {
    return cache_options_.high_watermark + 1;
}

ThreadCacheState::RecordShard& ThreadCacheState::shard_for(
    const void* pointer) noexcept {
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    // Ignore the low alignment bits, which are commonly identical for blocks.
    return shards_[(address >> 4U) % shard_count];
}

const ThreadCacheState::RecordShard& ThreadCacheState::shard_for(
    const void* pointer) const noexcept {
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    return shards_[(address >> 4U) % shard_count];
}

void* ThreadCacheState::central_allocate(std::size_t size,
                                         std::size_t alignment) {
    const std::lock_guard<std::mutex> lock(central_mutex_);
    void* const pointer = central_.allocate(size, alignment);
    statistics_.central_allocations.fetch_add(1, std::memory_order_relaxed);
    return pointer;
}

void ThreadCacheState::central_deallocate(void* pointer) {
    const std::lock_guard<std::mutex> lock(central_mutex_);
    central_.deallocate(pointer);
    statistics_.central_deallocations.fetch_add(1, std::memory_order_relaxed);
}

void ThreadCacheState::central_deallocate(void* pointer,
                                          std::size_t size,
                                          std::size_t alignment) {
    const std::lock_guard<std::mutex> lock(central_mutex_);
    central_.deallocate(pointer, size, alignment);
    statistics_.central_deallocations.fetch_add(1, std::memory_order_relaxed);
}

void ThreadCacheState::record_allocation(void* pointer,
                                         AllocationRecord record) {
    RecordShard& shard = shard_for(pointer);
    const std::lock_guard<std::mutex> lock(shard.mutex);
    const auto [entry, inserted] = shard.records.emplace(pointer, record);
    static_cast<void>(entry);
    if (!inserted) {
        throw std::logic_error(
            "central allocator returned a duplicate live address");
    }
}

void* ThreadCacheState::allocate(CacheBins& bins,
                                 std::size_t size,
                                 std::size_t alignment) {
    statistics_.allocation_requests.fetch_add(1, std::memory_order_relaxed);
    const std::size_t request_size = normalized_size(size);
    const auto selected = selector_.select(request_size, alignment);
    const std::uint64_t thread_id = current_thread_id();

    if (!selected.has_value()) {
        // Fallback allocations cannot enter a size-class cache because their
        // exact size and alignment must be preserved for deallocation.
        void* const pointer = central_allocate(request_size, alignment);
        try {
            record_allocation(pointer,
                              {.class_index = fallback_class,
                               .requested_size = request_size,
                               .alignment = alignment,
                               .owner_thread = thread_id,
                               .active = true});
        } catch (...) {
            central_deallocate(pointer, request_size, alignment);
            throw;
        }
        note_live_allocation();
        return pointer;
    }

    std::vector<void*>& bin = bins[*selected];
    if (!bin.empty()) {
        // LIFO reuse tends to return a recently touched cache line.
        void* const pointer = bin.back();
        bin.pop_back();
        RecordShard& shard = shard_for(pointer);
        const std::lock_guard<std::mutex> lock(shard.mutex);
        auto record = shard.records.find(pointer);
        if (record == shard.records.end() || record->second.active) {
            throw std::logic_error("thread cache metadata is inconsistent");
        }
        record->second.requested_size = request_size;
        record->second.alignment = alignment;
        record->second.owner_thread = thread_id;
        record->second.active = true;
        statistics_.cache_hits.fetch_add(1, std::memory_order_relaxed);
        statistics_.cached_blocks.fetch_sub(1, std::memory_order_relaxed);
        note_live_allocation();
        return pointer;
    }

    // The first central block satisfies this request; remaining blocks become
    // inactive cache entries for later allocations on the same thread.
    statistics_.batch_refills.fetch_add(1, std::memory_order_relaxed);
    void* result = nullptr;
    for (std::size_t index = 0; index < cache_options_.refill_batch; ++index) {
        void* pointer = nullptr;
        try {
            pointer = central_allocate(request_size, alignment);
            record_allocation(pointer,
                              {.class_index = *selected,
                               .requested_size = request_size,
                               .alignment = alignment,
                               .owner_thread = thread_id,
                               .active = index == 0});
        } catch (...) {
            if (pointer != nullptr) {
                central_deallocate(pointer, request_size, alignment);
            }
            if (index == 0) {
                throw;
            }
            // A partial refill is useful and remains internally consistent.
            break;
        }

        if (index == 0) {
            result = pointer;
        } else {
            bin.push_back(pointer);
            const std::size_t cached = statistics_.cached_blocks.fetch_add(
                                           1, std::memory_order_relaxed) +
                                       1;
            update_peak(statistics_.peak_cached_blocks, cached);
        }
    }

    note_live_allocation();
    return result;
}

void ThreadCacheState::validate_sized_deallocation(
    const AllocationRecord& record,
    std::size_t size,
    std::size_t alignment) const {
    const std::size_t request_size = normalized_size(size);
    if (record.class_index == fallback_class) {
        if (record.requested_size != request_size ||
            record.alignment != alignment) {
            throw AllocationMismatchError(
                "supplied size or alignment does not match the fallback allocation");
        }
        return;
    }

    const auto selected = selector_.select(request_size, alignment);
    if (!selected.has_value() || *selected != record.class_index) {
        throw AllocationMismatchError(
            "supplied size and alignment select a different size class");
    }
}

void ThreadCacheState::deallocate(CacheBins& bins,
                                  void* pointer,
                                  const std::size_t* size,
                                  const std::size_t* alignment) {
    statistics_.deallocation_requests.fetch_add(1, std::memory_order_relaxed);
    const std::uint64_t thread_id = current_thread_id();
    RecordShard& shard = shard_for(pointer);
    std::unique_lock<std::mutex> record_lock(shard.mutex);
    auto record = shard.records.find(pointer);
    if (record == shard.records.end()) {
        throw InvalidPoolPointer(
            "pointer is not owned by this thread-cached allocator");
    }
    if (!record->second.active) {
        throw DoubleFreeError("allocation is already cached as free");
    }
    if (size != nullptr && alignment != nullptr) {
        validate_sized_deallocation(record->second, *size, *alignment);
    }

    const bool remote = record->second.owner_thread != thread_id;
    const bool fallback = record->second.class_index == fallback_class;
    if (remote || fallback) {
        // Never place a block into a different thread's bins. Remote frees and
        // fallback blocks go directly to synchronized central storage.
        if (size != nullptr && alignment != nullptr) {
            central_deallocate(pointer, *size, *alignment);
        } else {
            central_deallocate(pointer);
        }
        shard.records.erase(record);
        if (remote) {
            statistics_.remote_deallocations.fetch_add(
                1, std::memory_order_relaxed);
        }
        statistics_.live_allocations.fetch_sub(1, std::memory_order_relaxed);
        return;
    }

    const std::size_t class_index = record->second.class_index;
    // Mark inactive while holding the shard lock before publishing the pointer
    // into this thread's private bin; this makes double-free detection precise.
    record->second.active = false;
    statistics_.local_deallocations.fetch_add(1, std::memory_order_relaxed);
    statistics_.live_allocations.fetch_sub(1, std::memory_order_relaxed);
    const std::size_t cached = statistics_.cached_blocks.fetch_add(
                                   1, std::memory_order_relaxed) +
                               1;
    update_peak(statistics_.peak_cached_blocks, cached);
    record_lock.unlock();

    std::vector<void*>& bin = bins[class_index];
    bin.push_back(pointer);
    if (bin.size() > cache_options_.high_watermark) {
        static_cast<void>(flush_bin(bin, cache_options_.low_watermark));
    }
}

std::size_t ThreadCacheState::flush_bin(std::vector<void*>& bin,
                                        std::size_t target_size) {
    std::size_t released = 0;
    while (bin.size() > target_size) {
        void* const pointer = bin.back();
        RecordShard& shard = shard_for(pointer);
        const std::lock_guard<std::mutex> lock(shard.mutex);
        auto record = shard.records.find(pointer);
        if (record == shard.records.end() || record->second.active) {
            throw std::logic_error("thread cache metadata is inconsistent");
        }
        // Keep the record lock until central storage accepts the block, so no
        // concurrent ownership query observes a half-completed transition.
        central_deallocate(pointer);
        shard.records.erase(record);
        bin.pop_back();
        statistics_.cached_blocks.fetch_sub(1, std::memory_order_relaxed);
        ++released;
    }
    if (released != 0) {
        statistics_.batch_flushes.fetch_add(1, std::memory_order_relaxed);
    }
    return released;
}

std::size_t ThreadCacheState::flush_bins(CacheBins& bins) {
    std::size_t released = 0;
    for (std::vector<void*>& bin : bins) {
        released += flush_bin(bin, 0);
    }
    return released;
}

bool ThreadCacheState::owns(const void* pointer) const {
    if (pointer == nullptr) {
        return false;
    }
    const RecordShard& shard = shard_for(pointer);
    const std::lock_guard<std::mutex> lock(shard.mutex);
    return shard.records.contains(const_cast<void*>(pointer));
}

std::optional<std::size_t> ThreadCacheState::owning_size_class(
    const void* pointer) const {
    if (pointer == nullptr) {
        return std::nullopt;
    }
    const RecordShard& shard = shard_for(pointer);
    const std::lock_guard<std::mutex> lock(shard.mutex);
    const auto record = shard.records.find(const_cast<void*>(pointer));
    if (record == shard.records.end() ||
        record->second.class_index == fallback_class) {
        return std::nullopt;
    }
    return selector_.class_size(record->second.class_index);
}

ThreadCacheStatistics ThreadCacheState::snapshot() const noexcept {
    return {
        .allocation_requests = statistics_.allocation_requests.load(
            std::memory_order_relaxed),
        .deallocation_requests = statistics_.deallocation_requests.load(
            std::memory_order_relaxed),
        .cache_hits = statistics_.cache_hits.load(std::memory_order_relaxed),
        .central_allocations = statistics_.central_allocations.load(
            std::memory_order_relaxed),
        .central_deallocations = statistics_.central_deallocations.load(
            std::memory_order_relaxed),
        .local_deallocations = statistics_.local_deallocations.load(
            std::memory_order_relaxed),
        .remote_deallocations = statistics_.remote_deallocations.load(
            std::memory_order_relaxed),
        .batch_refills = statistics_.batch_refills.load(std::memory_order_relaxed),
        .batch_flushes = statistics_.batch_flushes.load(std::memory_order_relaxed),
        .cached_blocks = statistics_.cached_blocks.load(std::memory_order_relaxed),
        .peak_cached_blocks = statistics_.peak_cached_blocks.load(
            std::memory_order_relaxed),
        .live_allocations = statistics_.live_allocations.load(
            std::memory_order_relaxed),
        .peak_live_allocations = statistics_.peak_live_allocations.load(
            std::memory_order_relaxed),
    };
}

SegregatedAllocatorStatistics ThreadCacheState::central_snapshot() const {
    const std::lock_guard<std::mutex> lock(central_mutex_);
    return central_.statistics();
}

void ThreadCacheState::note_live_allocation() noexcept {
    const std::size_t live = statistics_.live_allocations.fetch_add(
                                 1, std::memory_order_relaxed) +
                             1;
    update_peak(statistics_.peak_live_allocations, live);
}

}  // namespace memory_pool::detail
