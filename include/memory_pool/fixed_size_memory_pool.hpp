#pragma once

#include "memory_pool/pool_errors.hpp"
#include "memory_pool/pool_options.hpp"
#include "memory_pool/pool_statistics.hpp"

#include <cstddef>
#include <iosfwd>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace memory_pool {

namespace detail {
class DiagnosticState;
}

/// Fixed-capacity allocator for equally sized, equally aligned blocks.
/// The class is not thread-safe. Copying and moving are intentionally disabled.
class FixedSizeMemoryPool {
private:
    // A free block stores its next link inside its own unused storage.
    struct FreeNode {
        FreeNode* next;
    };

public:
    FixedSizeMemoryPool(std::size_t block_size,
                        std::size_t block_count,
                        std::size_t alignment = alignof(std::max_align_t),
                        PoolOptions options = {});
    ~FixedSizeMemoryPool();

    FixedSizeMemoryPool(const FixedSizeMemoryPool&) = delete;
    FixedSizeMemoryPool& operator=(const FixedSizeMemoryPool&) = delete;
    FixedSizeMemoryPool(FixedSizeMemoryPool&&) = delete;
    FixedSizeMemoryPool& operator=(FixedSizeMemoryPool&&) = delete;

    /// Returns one block, or nullptr when the pool is exhausted.
    [[nodiscard]] void* allocate() noexcept;

    /// Returns an allocated block. The pointer must be an exact address returned
    /// by this pool and must not have already been deallocated.
    void deallocate(void* pointer);

    template <typename T, typename... Args>
    [[nodiscard]] T* create(Args&&... args) {
        static_assert(!std::is_array_v<T>, "array types are not supported");
        if (sizeof(T) > block_size_) {
            throw std::invalid_argument("object is larger than a pool block");
        }
        if (alignof(T) > alignment_) {
            throw std::invalid_argument("object alignment exceeds pool alignment");
        }

        void* const memory = allocate();
        if (memory == nullptr) {
            throw std::bad_alloc{};
        }

        try {
            return std::construct_at(static_cast<T*>(memory), std::forward<Args>(args)...);
        } catch (...) {
            deallocate(memory);
            throw;
        }
    }

    template <typename T>
    void destroy(T* object) {
        if (object == nullptr) {
            return;
        }

        validate_allocated_pointer(object);
        std::destroy_at(object);
        deallocate(object);
    }

    [[nodiscard]] bool owns(const void* pointer) const noexcept;
    [[nodiscard]] bool is_block_start(const void* pointer) const noexcept;
    [[nodiscard]] std::size_t block_index(const void* pointer) const;
    [[nodiscard]] bool is_allocated(const void* pointer) const;

    /// Performs an O(n) free-list and state-table consistency check.
    void validate_integrity() const;
    void debug_dump(std::ostream& output) const;

    [[nodiscard]] std::size_t block_size() const noexcept { return block_size_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return block_count_; }
    [[nodiscard]] std::size_t available() const noexcept { return free_blocks_; }
    [[nodiscard]] std::size_t in_use() const noexcept { return block_count_ - free_blocks_; }
    [[nodiscard]] bool diagnostics_enabled() const noexcept;
    [[nodiscard]] const PoolStatistics& statistics() const noexcept { return statistics_; }

private:
    void initialize_free_list() noexcept;
    void validate_allocated_pointer(const void* pointer) const;

    [[nodiscard]] bool is_raw_block_start(const void* pointer) const noexcept;
    [[nodiscard]] std::size_t raw_block_index(const void* pointer) const noexcept;
    [[nodiscard]] void* payload_from_raw_block(void* raw_block) const noexcept;
    [[nodiscard]] std::byte* raw_block_from_payload(void* payload) const noexcept;

    std::byte* storage_{nullptr};
    FreeNode* free_head_{nullptr};
    std::size_t block_size_{};
    std::size_t block_count_{};
    std::size_t alignment_{};
    std::size_t payload_offset_{};
    std::size_t block_stride_{};
    std::size_t storage_size_{};
    std::size_t free_blocks_{};
    PoolOptions options_{};
    bool fast_path_{true};
    PoolStatistics statistics_{};
    std::unique_ptr<detail::DiagnosticState> diagnostic_state_;
};

}  // namespace memory_pool
