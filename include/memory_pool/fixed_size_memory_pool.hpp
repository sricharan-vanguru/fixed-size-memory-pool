#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace memory_pool {

class FixedSizeMemoryPool {
public:
    FixedSizeMemoryPool(std::size_t block_size,
                        std::size_t block_count,
                        std::size_t alignment = alignof(std::max_align_t))
        : block_size_(normalize_block_size(block_size, normalize_alignment(alignment))),
          block_count_(block_count),
          alignment_(normalize_alignment(alignment)),
          storage_size_(checked_multiply(block_size_, block_count_)) {
        validate_alignment(alignment_);
        if (block_count_ == 0) {
            throw std::invalid_argument("block_count must be greater than zero");
        }

        storage_ = static_cast<std::byte*>(
            ::operator new(storage_size_, std::align_val_t{alignment_}));
        initialize_free_list();
    }

    ~FixedSizeMemoryPool() {
        ::operator delete(storage_, std::align_val_t{alignment_});
    }

    FixedSizeMemoryPool(const FixedSizeMemoryPool&) = delete;
    FixedSizeMemoryPool& operator=(const FixedSizeMemoryPool&) = delete;
    FixedSizeMemoryPool(FixedSizeMemoryPool&&) = delete;
    FixedSizeMemoryPool& operator=(FixedSizeMemoryPool&&) = delete;

    [[nodiscard]] void* allocate() noexcept {
        if (free_head_ == nullptr) {
            return nullptr;
        }

        FreeNode* const node = free_head_;
        free_head_ = node->next;
        --free_blocks_;
        return node;
    }

    void deallocate(void* pointer) {
        if (pointer == nullptr) {
            return;
        }
        if (!owns(pointer)) {
            throw std::invalid_argument("pointer does not belong to this pool");
        }

        const auto offset = static_cast<std::size_t>(
            static_cast<std::byte*>(pointer) - storage_);
        if (offset % block_size_ != 0) {
            throw std::invalid_argument("pointer is not aligned to a block boundary");
        }

        auto* const node = ::new (pointer) FreeNode{free_head_};
        free_head_ = node;
        ++free_blocks_;
    }

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
        std::destroy_at(object);
        deallocate(object);
    }

    [[nodiscard]] bool owns(const void* pointer) const noexcept {
        const auto address = reinterpret_cast<std::uintptr_t>(pointer);
        const auto begin = reinterpret_cast<std::uintptr_t>(storage_);
        return address >= begin && address < begin + storage_size_;
    }

    [[nodiscard]] std::size_t block_size() const noexcept { return block_size_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return block_count_; }
    [[nodiscard]] std::size_t available() const noexcept { return free_blocks_; }

private:
    struct FreeNode {
        FreeNode* next;
    };

    static void validate_alignment(std::size_t alignment) {
        if (alignment == 0 || (alignment & (alignment - 1U)) != 0U) {
            throw std::invalid_argument("alignment must be a non-zero power of two");
        }
    }

    static std::size_t normalize_alignment(std::size_t alignment) {
        validate_alignment(alignment);
        return alignment < alignof(FreeNode) ? alignof(FreeNode) : alignment;
    }

    static std::size_t checked_multiply(std::size_t left, std::size_t right) {
        if (right != 0 && left > std::numeric_limits<std::size_t>::max() / right) {
            throw std::length_error("requested pool size overflows size_t");
        }
        return left * right;
    }

    static std::size_t normalize_block_size(std::size_t size, std::size_t alignment) {
        validate_alignment(alignment);
        size = size < sizeof(FreeNode) ? sizeof(FreeNode) : size;
        if (size > std::numeric_limits<std::size_t>::max() - (alignment - 1U)) {
            throw std::length_error("block size is too large");
        }
        return (size + alignment - 1U) & ~(alignment - 1U);
    }

    void initialize_free_list() noexcept {
        free_head_ = nullptr;
        for (std::size_t index = block_count_; index > 0; --index) {
            void* const block = storage_ + ((index - 1U) * block_size_);
            free_head_ = ::new (block) FreeNode{free_head_};
        }
        free_blocks_ = block_count_;
    }

    std::byte* storage_{nullptr};
    FreeNode* free_head_{nullptr};
    std::size_t block_size_{};
    std::size_t block_count_{};
    std::size_t alignment_{};
    std::size_t storage_size_{};
    std::size_t free_blocks_{};
};

}  // namespace memory_pool
