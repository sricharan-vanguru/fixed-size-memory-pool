#include "arena_chunk.hpp"

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>

namespace memory_pool::detail {

ArenaChunk::ArenaChunk(std::size_t capacity,
                       std::size_t alignment,
                       MemoryProviderPtr provider)
    : provider_(std::move(provider)), capacity_(capacity), alignment_(alignment) {
    if (provider_ == nullptr) {
        throw std::invalid_argument("arena chunk provider must not be null");
    }
    storage_ = static_cast<std::byte*>(provider_->allocate(capacity_, alignment_));
}

ArenaChunk::~ArenaChunk() { provider_->deallocate(storage_, capacity_, alignment_); }

std::optional<ArenaAllocation>
ArenaChunk::try_allocate(std::size_t size, std::size_t alignment) noexcept {
    if (alignment > alignment_ || offset_ > capacity_) {
        return std::nullopt;
    }

    void* candidate = storage_ + offset_;
    std::size_t space = capacity_ - offset_;
    // std::align advances candidate past any required padding and reduces
    // space. Example: offset 3 with alignment 8 starts the object at offset 8.
    void* const aligned = std::align(alignment, size, candidate, space);
    if (aligned == nullptr) {
        return std::nullopt;
    }

    const std::size_t previous_offset = offset_;
    const auto aligned_address = static_cast<std::byte*>(aligned);
    // Padding is consumed together with the payload; monotonic allocations do
    // not create holes that can be individually reused.
    offset_ = static_cast<std::size_t>(aligned_address - storage_) + size;
    return ArenaAllocation{
        .pointer = aligned,
        .previous_offset = previous_offset,
        .consumed_bytes = offset_ - previous_offset,
    };
}

void ArenaChunk::rewind(std::size_t previous_offset) noexcept {
    if (previous_offset <= offset_) {
        offset_ = previous_offset;
    }
}

void ArenaChunk::reset() noexcept { offset_ = 0; }

bool ArenaChunk::owns(const void* pointer) const noexcept {
    if (pointer == nullptr) {
        return false;
    }
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    const auto begin = reinterpret_cast<std::uintptr_t>(storage_);
    return address >= begin && address - begin < capacity_;
}

std::size_t ArenaChunk::capacity() const noexcept { return capacity_; }
std::size_t ArenaChunk::used() const noexcept { return offset_; }
std::size_t ArenaChunk::alignment() const noexcept { return alignment_; }

}  // namespace memory_pool::detail
