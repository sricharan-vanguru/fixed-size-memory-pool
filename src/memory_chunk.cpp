#include "memory_pool/memory_chunk.hpp"

#include "detail/block_layout.hpp"
#include "memory_pool/pool_errors.hpp"

#include <cstdint>
#include <new>
#include <stdexcept>
#include <utility>
#include <vector>

namespace memory_pool {

struct MemoryChunk::Impl {
    struct FreeNode {
        FreeNode* next;
    };

    Impl(std::size_t requested_block_size,
         std::size_t requested_block_count,
         std::size_t requested_alignment,
         MemoryProviderPtr memory_provider)
        : provider(std::move(memory_provider)) {
        if (provider == nullptr) {
            throw std::invalid_argument("memory provider must not be null");
        }

        const detail::BlockLayout layout = detail::make_block_layout(
            requested_block_size,
            requested_block_count,
            requested_alignment,
            false,
            sizeof(FreeNode),
            alignof(FreeNode),
            0);
        block_size = layout.block_size;
        block_count = layout.block_count;
        alignment = layout.alignment;
        block_stride = layout.block_stride;
        storage_size = layout.storage_size;
        // Unlike the minimal fixed pool, chunks always track state because a
        // manager may need deterministic ownership and reclamation decisions.
        states.assign(block_count, 0U);

        storage = static_cast<std::byte*>(provider->allocate(storage_size, alignment));
        initialize_free_list();
    }

    ~Impl() {
        provider->deallocate(storage, storage_size, alignment);
    }

    void initialize_free_list() noexcept {
        free_head = nullptr;
        for (std::size_t index = block_count; index > 0; --index) {
            void* const block = storage + ((index - 1U) * block_stride);
            free_head = ::new (block) FreeNode{free_head};
        }
        free_blocks = block_count;
    }

    [[nodiscard]] std::size_t block_index(const void* pointer) const noexcept {
        return static_cast<std::size_t>(
                   static_cast<const std::byte*>(pointer) - storage) /
               block_stride;
    }

    MemoryProviderPtr provider;
    std::byte* storage{nullptr};
    FreeNode* free_head{nullptr};
    std::vector<std::uint8_t> states;
    std::size_t block_size{};
    std::size_t block_count{};
    std::size_t alignment{};
    std::size_t block_stride{};
    std::size_t storage_size{};
    std::size_t free_blocks{};
};

MemoryChunk::MemoryChunk(std::size_t block_size,
                         std::size_t block_count,
                         std::size_t alignment,
                         MemoryProviderPtr provider)
    : impl_(std::make_unique<Impl>(
          block_size, block_count, alignment, std::move(provider))) {}

MemoryChunk::~MemoryChunk() = default;

void* MemoryChunk::allocate() noexcept {
    if (impl_->free_head == nullptr) {
        return nullptr;
    }
    Impl::FreeNode* const node = impl_->free_head;
    impl_->free_head = node->next;
    --impl_->free_blocks;
    // The list and state byte are updated together: 0 means free, 1 means live.
    impl_->states[impl_->block_index(node)] = 1U;
    return node;
}

void MemoryChunk::deallocate(void* pointer) {
    if (pointer == nullptr) {
        return;
    }
    if (!is_block_start(pointer)) {
        throw InvalidPoolPointer("pointer is not the start of a block in this chunk");
    }

    const std::size_t index = impl_->block_index(pointer);
    if (impl_->states[index] == 0U) {
        throw DoubleFreeError("block is not currently allocated by this chunk");
    }

    impl_->states[index] = 0U;
    impl_->free_head = ::new (pointer) Impl::FreeNode{impl_->free_head};
    ++impl_->free_blocks;
}

bool MemoryChunk::owns(const void* pointer) const noexcept {
    if (pointer == nullptr) {
        return false;
    }
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    const auto begin = reinterpret_cast<std::uintptr_t>(impl_->storage);
    return address >= begin && address - begin < impl_->storage_size;
}

bool MemoryChunk::is_block_start(const void* pointer) const noexcept {
    if (!owns(pointer)) {
        return false;
    }
    const auto offset = static_cast<std::size_t>(
        static_cast<const std::byte*>(pointer) - impl_->storage);
    return offset % impl_->block_stride == 0U;
}

bool MemoryChunk::empty() const noexcept {
    return impl_->free_blocks == impl_->block_count;
}

const void* MemoryChunk::data() const noexcept { return impl_->storage; }
std::size_t MemoryChunk::storage_size() const noexcept { return impl_->storage_size; }
std::size_t MemoryChunk::block_size() const noexcept { return impl_->block_size; }
std::size_t MemoryChunk::block_count() const noexcept { return impl_->block_count; }
std::size_t MemoryChunk::available() const noexcept { return impl_->free_blocks; }
std::size_t MemoryChunk::in_use() const noexcept {
    return impl_->block_count - impl_->free_blocks;
}
std::size_t MemoryChunk::alignment() const noexcept { return impl_->alignment; }

}  // namespace memory_pool
