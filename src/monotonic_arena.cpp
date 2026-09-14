#include "memory_pool/monotonic_arena.hpp"

#include "detail/arena_chunk.hpp"
#include "memory_pool/new_delete_memory_provider.hpp"

#include <algorithm>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace memory_pool {
namespace {

bool is_power_of_two(std::size_t value) noexcept {
    return value != 0 && (value & (value - 1U)) == 0U;
}

MonotonicArenaOptions validate_options(MonotonicArenaOptions options) {
    if (options.initial_chunk_size == 0) {
        throw std::invalid_argument(
            "initial_chunk_size must be greater than zero");
    }
    if (!is_power_of_two(options.initial_alignment)) {
        throw std::invalid_argument(
            "initial_alignment must be a non-zero power of two");
    }
    options.initial_alignment = std::max(
        options.initial_alignment, alignof(std::max_align_t));

    if (options.growth.mode == ArenaGrowthMode::geometric &&
        options.growth.factor < 2) {
        throw std::invalid_argument(
            "geometric arena growth factor must be at least two");
    }
    if (options.growth.maximum_chunk_size != 0 &&
        options.growth.maximum_chunk_size < options.initial_chunk_size) {
        throw std::invalid_argument(
            "maximum arena chunk size cannot be smaller than the initial chunk");
    }
    return options;
}

std::size_t normalized_size(std::size_t size) noexcept {
    return std::max<std::size_t>(size, 1);
}

}  // namespace

struct MonotonicArena::Impl {
    struct DestructorRecord {
        // Type erasure stores only what reset needs: address plus destructor.
        void* object;
        Destructor destructor;
    };

    struct LastAllocation {
        detail::ArenaChunk* chunk{};
        void* pointer{};
        std::size_t previous_offset{};
        std::size_t consumed_bytes{};
    };

    Impl(MonotonicArenaOptions requested_options, MemoryProviderPtr memory_provider)
        : options(validate_options(requested_options)),
          provider(memory_provider != nullptr
                       ? std::move(memory_provider)
                       : std::make_shared<NewDeleteMemoryProvider>()),
          next_chunk_size(calculate_next_chunk_size(
              options.initial_chunk_size)) {
        add_chunk(options.initial_chunk_size, options.initial_alignment);
    }

    ~Impl() { destroy_registered_objects(); }

    [[nodiscard]] std::size_t calculate_next_chunk_size(
        std::size_t current_size) const noexcept {
        if (options.growth.mode == ArenaGrowthMode::fixed) {
            return options.initial_chunk_size;
        }

        const std::size_t maximum = options.growth.maximum_chunk_size;
        if (maximum != 0 && current_size >= maximum) {
            return maximum;
        }
        if (current_size >
            std::numeric_limits<std::size_t>::max() / options.growth.factor) {
            // Saturate instead of allowing multiplication to wrap around.
            return maximum != 0 ? maximum
                                : std::numeric_limits<std::size_t>::max();
        }
        const std::size_t next = current_size * options.growth.factor;
        return maximum != 0 ? std::min(next, maximum) : next;
    }

    detail::ArenaChunk& add_chunk(std::size_t capacity,
                                  std::size_t alignment) {
        auto chunk = std::make_unique<detail::ArenaChunk>(
            capacity, alignment, provider);
        detail::ArenaChunk& result = *chunk;
        chunks.push_back(std::move(chunk));

        ++statistics.chunk_allocations;
        statistics.current_chunks = chunks.size();
        statistics.peak_chunks = std::max(
            statistics.peak_chunks, statistics.current_chunks);
        statistics.current_reserved_bytes += capacity;
        statistics.peak_reserved_bytes = std::max(
            statistics.peak_reserved_bytes,
            statistics.current_reserved_bytes);
        return result;
    }

    void record_allocation(detail::ArenaChunk& chunk,
                           const detail::ArenaAllocation& allocation,
                           std::size_t requested_size) noexcept {
        ++statistics.successful_allocations;
        statistics.requested_bytes += requested_size;
        statistics.current_used_bytes += allocation.consumed_bytes;
        statistics.peak_used_bytes = std::max(
            statistics.peak_used_bytes, statistics.current_used_bytes);
        statistics.padding_bytes += allocation.consumed_bytes - requested_size;
        last_allocation = LastAllocation{
            .chunk = &chunk,
            .pointer = allocation.pointer,
            .previous_offset = allocation.previous_offset,
            .consumed_bytes = allocation.consumed_bytes,
        };
    }

    [[nodiscard]] void* allocate(std::size_t size, std::size_t alignment) {
        ++statistics.allocation_requests;
        try {
            if (!is_power_of_two(alignment)) {
                throw std::invalid_argument(
                    "arena alignment must be a non-zero power of two");
            }
            const std::size_t request_size = normalized_size(size);

            for (std::size_t index = current_chunk; index < chunks.size(); ++index) {
                // After reset, retained chunks are tried in their original
                // order. During a lifetime we never move backwards.
                auto& chunk = *chunks[index];
                const auto allocation = chunk.try_allocate(request_size, alignment);
                if (allocation.has_value()) {
                    current_chunk = index;
                    record_allocation(chunk, *allocation, request_size);
                    return allocation->pointer;
                }
            }

            // Oversized requests get a fitting chunk even when larger than the
            // configured geometric-growth cap.
            const std::size_t capacity = std::max(next_chunk_size, request_size);
            const std::size_t chunk_alignment = std::max(
                options.initial_alignment, alignment);
            detail::ArenaChunk& chunk = add_chunk(capacity, chunk_alignment);
            current_chunk = chunks.size() - 1;
            next_chunk_size = calculate_next_chunk_size(next_chunk_size);
            const auto allocation = chunk.try_allocate(request_size, alignment);
            if (!allocation.has_value()) {
                throw std::logic_error(
                    "new arena chunk could not satisfy its triggering allocation");
            }
            record_allocation(chunk, *allocation, request_size);
            return allocation->pointer;
        } catch (...) {
            ++statistics.failed_allocations;
            throw;
        }
    }

    void rollback_last(void* pointer) noexcept {
        ++statistics.construction_failures;
        // Rewinding is safe only if no nested arena allocation occurred after
        // this storage was handed to the constructor. Otherwise reset recovers
        // the consumed space later.
        if (last_allocation.pointer != pointer || last_allocation.chunk == nullptr) {
            return;
        }
        last_allocation.chunk->rewind(last_allocation.previous_offset);
        statistics.current_used_bytes -= last_allocation.consumed_bytes;
        last_allocation = {};
    }

    void destroy_registered_objects() noexcept {
        // LIFO order mirrors automatic local variables: dependencies created
        // first normally outlive objects created later.
        while (!destructors.empty()) {
            const DestructorRecord record = destructors.back();
            destructors.pop_back();
            record.destructor(record.object);
            ++statistics.destructor_calls;
        }
    }

    void reset() noexcept {
        destroy_registered_objects();
        ++statistics.reset_calls;
        statistics.current_used_bytes = 0;
        current_chunk = 0;
        last_allocation = {};

        if (options.reset_policy == ArenaResetPolicy::retain_all_chunks) {
            // Rewind without freeing: a repeated workload can reuse the exact
            // same addresses and avoid future provider calls.
            for (const auto& chunk : chunks) {
                chunk->reset();
            }
            return;
        }

        // Memory-sensitive mode keeps a cheap initial allocation but releases
        // capacity acquired for an earlier peak workload.
        chunks.front()->reset();
        if (chunks.size() > 1) {
            std::size_t released_bytes = 0;
            for (std::size_t index = 1; index < chunks.size(); ++index) {
                released_bytes += chunks[index]->capacity();
            }
            statistics.chunk_releases += chunks.size() - 1;
            statistics.current_reserved_bytes -= released_bytes;
            chunks.erase(chunks.begin() + 1, chunks.end());
            statistics.current_chunks = 1;
        }
        next_chunk_size = calculate_next_chunk_size(
            options.initial_chunk_size);
    }

    [[nodiscard]] bool owns(const void* pointer) const noexcept {
        return std::any_of(
            chunks.begin(), chunks.end(), [pointer](const auto& chunk) {
                return chunk->owns(pointer);
            });
    }

    MonotonicArenaOptions options;
    MemoryProviderPtr provider;
    std::vector<std::unique_ptr<detail::ArenaChunk>> chunks;
    std::vector<DestructorRecord> destructors;
    std::size_t current_chunk{};
    std::size_t next_chunk_size{};
    LastAllocation last_allocation;
    ArenaStatistics statistics;
};

MonotonicArena::MonotonicArena(MonotonicArenaOptions options,
                               MemoryProviderPtr provider)
    : impl_(std::make_unique<Impl>(options, std::move(provider))) {}

MonotonicArena::~MonotonicArena() = default;

void* MonotonicArena::allocate(std::size_t size, std::size_t alignment) {
    return impl_->allocate(size, alignment);
}

void MonotonicArena::register_destructor(void* object, Destructor destructor) {
    impl_->destructors.push_back({.object = object, .destructor = destructor});
}

void MonotonicArena::rollback_last_allocation(void* pointer) noexcept {
    impl_->rollback_last(pointer);
}

void MonotonicArena::note_object_construction() noexcept {
    ++impl_->statistics.object_constructions;
}

void MonotonicArena::reset() noexcept { impl_->reset(); }

bool MonotonicArena::owns(const void* pointer) const noexcept {
    return impl_->owns(pointer);
}

std::size_t MonotonicArena::chunk_count() const noexcept {
    return impl_->chunks.size();
}

std::size_t MonotonicArena::bytes_used() const noexcept {
    return impl_->statistics.current_used_bytes;
}

std::size_t MonotonicArena::bytes_reserved() const noexcept {
    return impl_->statistics.current_reserved_bytes;
}

const ArenaStatistics& MonotonicArena::statistics() const noexcept {
    return impl_->statistics;
}

}  // namespace memory_pool
