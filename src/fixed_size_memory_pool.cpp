#include "memory_pool/fixed_size_memory_pool.hpp"

#include "detail/block_layout.hpp"
#include "detail/diagnostic_state.hpp"
#include "detail/memory_guard.hpp"
#include "detail/statistics_tracker.hpp"
#include "memory_pool/pool_errors.hpp"

#include <cstdint>
#include <exception>
#include <ostream>
#include <vector>

namespace memory_pool {

FixedSizeMemoryPool::FixedSizeMemoryPool(std::size_t block_size,
                                         std::size_t block_count,
                                         std::size_t alignment,
                                         PoolOptions options)
    : options_(options),
      fast_path_(options.diagnostics == DiagnosticMode::disabled &&
                 !options.poison_memory && !options.guard_bytes &&
                 !options.collect_statistics) {
    // One layout calculation keeps payload alignment, optional guards, and the
    // intrusive FreeNode storage consistent across every operation.
    const detail::BlockLayout layout = detail::make_block_layout(
        block_size,
        block_count,
        alignment,
        options.guard_bytes,
        sizeof(FreeNode),
        alignof(FreeNode),
        detail::guard_size);

    block_size_ = layout.block_size;
    block_count_ = layout.block_count;
    alignment_ = layout.alignment;
    payload_offset_ = layout.payload_offset;
    block_stride_ = layout.block_stride;
    storage_size_ = layout.storage_size;

    if (options.diagnostics == DiagnosticMode::enabled) {
        diagnostic_state_ = std::make_unique<detail::DiagnosticState>(block_count_);
    }

    storage_ = static_cast<std::byte*>(
        ::operator new(storage_size_, std::align_val_t{alignment_}));
    statistics_.reserved_bytes = storage_size_;
    initialize_free_list();
}

FixedSizeMemoryPool::~FixedSizeMemoryPool() {
    if (diagnostic_state_ != nullptr && options_.leak_handler != nullptr &&
        free_blocks_ != block_count_) {
        options_.leak_handler(block_count_ - free_blocks_);
    }
    ::operator delete(storage_, std::align_val_t{alignment_});
}

void* FixedSizeMemoryPool::allocate() noexcept {
    // The default path deliberately performs only a free-list pop and counter
    // update. Optional diagnostics never add branches inside this path.
    if (fast_path_) {
        if (free_head_ == nullptr) {
            return nullptr;
        }
        FreeNode* const node = free_head_;
        free_head_ = node->next;
        --free_blocks_;
        return node;
    }

    if (options_.collect_statistics) {
        detail::record_allocation_request(statistics_, block_size_);
    }
    if (free_head_ == nullptr) {
        if (options_.collect_statistics) {
            detail::record_failed_allocation(statistics_);
        }
        return nullptr;
    }

    FreeNode* const node = free_head_;
    free_head_ = node->next;
    --free_blocks_;

    // A reachable node marked allocated means an internal invariant has been
    // violated. allocate() is noexcept, so continuing would be unsafe.
    if (diagnostic_state_ != nullptr &&
        !diagnostic_state_->try_mark_allocated(raw_block_index(node))) {
        std::terminate();
    }

    if (options_.poison_memory || options_.guard_bytes) {
        detail::prepare_allocated_memory(
            node, payload_offset_, block_size_, options_.poison_memory, options_.guard_bytes);
    }
    if (options_.collect_statistics) {
        detail::record_successful_allocation(statistics_);
    }
    return payload_from_raw_block(node);
}

void FixedSizeMemoryPool::deallocate(void* pointer) {
    if (pointer == nullptr) {
        return;
    }

    const std::size_t index = block_index(pointer);
    std::byte* const raw_block = raw_block_from_payload(pointer);

    if (fast_path_) {
        // Freed payload storage becomes the next intrusive list link.
        free_head_ = ::new (raw_block) FreeNode{free_head_};
        ++free_blocks_;
        return;
    }

    if (options_.poison_memory || options_.guard_bytes) {
        detail::validate_and_prepare_freed_memory(
            raw_block,
            payload_offset_,
            block_size_,
            options_.poison_memory,
            options_.guard_bytes);
    }
    if (diagnostic_state_ != nullptr) {
        diagnostic_state_->mark_free(index);
    }
    if (options_.collect_statistics) {
        detail::record_deallocation(statistics_);
    }

    free_head_ = ::new (raw_block) FreeNode{free_head_};
    ++free_blocks_;
}

bool FixedSizeMemoryPool::owns(const void* pointer) const noexcept {
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    const auto begin = reinterpret_cast<std::uintptr_t>(storage_);
    return address >= begin && address < begin + storage_size_;
}

bool FixedSizeMemoryPool::is_block_start(const void* pointer) const noexcept {
    if (pointer == nullptr || !owns(pointer)) {
        return false;
    }

    const auto offset = static_cast<std::size_t>(
        static_cast<const std::byte*>(pointer) - storage_);
    return offset >= payload_offset_ &&
           (offset - payload_offset_) % block_stride_ == 0U &&
           (offset - payload_offset_) / block_stride_ < block_count_;
}

std::size_t FixedSizeMemoryPool::block_index(const void* pointer) const {
    if (!is_block_start(pointer)) {
        throw InvalidPoolPointer("pointer is not the start of a block in this pool");
    }

    const auto offset = static_cast<std::size_t>(
        static_cast<const std::byte*>(pointer) - storage_);
    return (offset - payload_offset_) / block_stride_;
}

bool FixedSizeMemoryPool::is_allocated(const void* pointer) const {
    if (diagnostic_state_ == nullptr) {
        throw std::logic_error("allocation state requires diagnostic mode");
    }
    return diagnostic_state_->is_allocated(block_index(pointer));
}

void FixedSizeMemoryPool::validate_integrity() const {
    if (diagnostic_state_ == nullptr) {
        throw std::logic_error("integrity validation requires diagnostic mode");
    }

    // Compare two independent views of the pool: the linked free list and the
    // diagnostic allocation-state table.
    std::vector<std::size_t> free_indices;
    free_indices.reserve(free_blocks_);
    for (const FreeNode* node = free_head_; node != nullptr; node = node->next) {
        if (!is_raw_block_start(node)) {
            throw MemoryCorruptionError("free list contains an invalid block address");
        }
        free_indices.push_back(raw_block_index(node));
        if (free_indices.size() > block_count_) {
            throw MemoryCorruptionError("free list is longer than pool capacity");
        }
    }

    diagnostic_state_->validate_free_blocks(free_indices, free_blocks_);
}

void FixedSizeMemoryPool::debug_dump(std::ostream& output) const {
    output << "FixedSizeMemoryPool{block_size=" << block_size_
           << ", capacity=" << block_count_
           << ", available=" << free_blocks_
           << ", diagnostics=" << (diagnostics_enabled() ? "enabled" : "disabled")
           << "}\n";

    if (diagnostic_state_ == nullptr) {
        output << "block states unavailable\n";
        return;
    }

    for (std::size_t index = 0; index < block_count_; ++index) {
        output << "block[" << index << "]=" << diagnostic_state_->state_name(index) << '\n';
    }

    output << "free_list=";
    const FreeNode* node = free_head_;
    std::size_t visited = 0;
    while (node != nullptr && visited < block_count_) {
        if (!is_raw_block_start(node)) {
            output << "invalid-address";
            node = nullptr;
            break;
        }
        if (visited != 0) {
            output << "->";
        }
        output << raw_block_index(node);
        node = node->next;
        ++visited;
    }
    if (node != nullptr) {
        output << "->cycle-or-overflow";
    } else if (visited == 0) {
        output << "empty";
    }
    output << '\n';
}

bool FixedSizeMemoryPool::diagnostics_enabled() const noexcept {
    return diagnostic_state_ != nullptr;
}

void FixedSizeMemoryPool::initialize_free_list() noexcept {
    free_head_ = nullptr;
    // Build backwards so the first allocation returns block zero. Returning a
    // block later pushes it to the head, giving normal operation LIFO reuse.
    for (std::size_t index = block_count_; index > 0; --index) {
        void* const block = storage_ + ((index - 1U) * block_stride_);
        free_head_ = ::new (block) FreeNode{free_head_};
    }
    free_blocks_ = block_count_;
}

void FixedSizeMemoryPool::validate_allocated_pointer(const void* pointer) const {
    const std::size_t index = block_index(pointer);
    if (diagnostic_state_ != nullptr && !diagnostic_state_->is_allocated(index)) {
        throw DoubleFreeError("object is not currently allocated by this pool");
    }
}

bool FixedSizeMemoryPool::is_raw_block_start(const void* pointer) const noexcept {
    if (pointer == nullptr) {
        return false;
    }
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    const auto begin = reinterpret_cast<std::uintptr_t>(storage_);
    return address >= begin && address < begin + storage_size_ &&
           (address - begin) % block_stride_ == 0U;
}

std::size_t FixedSizeMemoryPool::raw_block_index(const void* pointer) const noexcept {
    return static_cast<std::size_t>(
               static_cast<const std::byte*>(pointer) - storage_) /
           block_stride_;
}

void* FixedSizeMemoryPool::payload_from_raw_block(void* raw_block) const noexcept {
    return static_cast<std::byte*>(raw_block) + payload_offset_;
}

std::byte* FixedSizeMemoryPool::raw_block_from_payload(void* payload) const noexcept {
    return static_cast<std::byte*>(payload) - payload_offset_;
}

}  // namespace memory_pool
