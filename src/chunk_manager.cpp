#include "memory_pool/chunk_manager.hpp"

#include "memory_pool/new_delete_memory_provider.hpp"
#include "memory_pool/pool_errors.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace memory_pool {

ChunkManager::ChunkManager(ChunkManagerOptions options, MemoryProviderPtr provider)
    : options_(options),
      provider_(provider != nullptr ? std::move(provider)
                                    : std::make_shared<NewDeleteMemoryProvider>()),
      next_growth_blocks_(options.initial_blocks) {
    if (options_.initial_blocks == 0) {
        throw std::invalid_argument("initial_blocks must be greater than zero");
    }
    if (options_.growth.mode == GrowthMode::geometric && options_.growth.factor < 2) {
        throw std::invalid_argument("geometric growth factor must be at least two");
    }
    if (options_.growth.maximum_blocks_per_chunk != 0 &&
        options_.growth.maximum_blocks_per_chunk < options_.initial_blocks) {
        throw std::invalid_argument(
            "maximum blocks per chunk cannot be smaller than initial_blocks");
    }

    MemoryChunk& initial_chunk = grow();
    block_size_ = initial_chunk.block_size();
    alignment_ = initial_chunk.alignment();
}

ChunkManager::~ChunkManager() = default;

void* ChunkManager::try_allocate() noexcept {
    // Chunks own stable allocations, so scanning never relocates live blocks.
    for (const auto& chunk : chunks_) {
        if (chunk->available() != 0) {
            return chunk->allocate();
        }
    }
    return nullptr;
}

void ChunkManager::deallocate(void* pointer) {
    if (pointer == nullptr) {
        return;
    }
    MemoryChunk* const chunk = find_chunk(pointer);
    if (chunk == nullptr) {
        throw InvalidPoolPointer("pointer is not owned by this chunk manager");
    }
    chunk->deallocate(pointer);
    reclaim_if_allowed(chunk);
}

MemoryChunk& ChunkManager::grow() {
    const std::size_t new_chunk_blocks = next_growth_blocks_;
    // Construct before modifying the vector. Provider or metadata failure then
    // leaves the manager and all existing allocations unchanged.
    auto new_chunk = std::make_unique<MemoryChunk>(
        options_.block_size, new_chunk_blocks, options_.alignment, provider_);
    MemoryChunk& result = *new_chunk;
    chunks_.push_back(std::move(new_chunk));
    next_growth_blocks_ = calculate_next_growth(new_chunk_blocks);
    return result;
}

MemoryChunk* ChunkManager::find_chunk(const void* pointer) noexcept {
    for (const auto& chunk : chunks_) {
        if (chunk->owns(pointer)) {
            return chunk.get();
        }
    }
    return nullptr;
}

const MemoryChunk* ChunkManager::find_chunk(const void* pointer) const noexcept {
    for (const auto& chunk : chunks_) {
        if (chunk->owns(pointer)) {
            return chunk.get();
        }
    }
    return nullptr;
}

std::size_t ChunkManager::chunk_count() const noexcept { return chunks_.size(); }

std::size_t ChunkManager::capacity() const noexcept {
    std::size_t total = 0;
    for (const auto& chunk : chunks_) {
        total += chunk->block_count();
    }
    return total;
}

std::size_t ChunkManager::available() const noexcept {
    std::size_t total = 0;
    for (const auto& chunk : chunks_) {
        total += chunk->available();
    }
    return total;
}

std::size_t ChunkManager::in_use() const noexcept { return capacity() - available(); }

std::size_t ChunkManager::block_size() const noexcept { return block_size_; }
std::size_t ChunkManager::alignment() const noexcept { return alignment_; }
std::size_t ChunkManager::next_growth_block_count() const noexcept {
    return next_growth_blocks_;
}
IMemoryProvider& ChunkManager::memory_provider() noexcept { return *provider_; }

void ChunkManager::reclaim_if_allowed(MemoryChunk* chunk) noexcept {
    if (!chunk->empty()) {
        return;
    }

    const auto empty_chunks = static_cast<std::size_t>(
        std::count_if(chunks_.begin(), chunks_.end(), [](const auto& candidate) {
            return candidate->empty();
        }));
    if (empty_chunks <= options_.reclamation.spare_empty_chunks) {
        return;
    }

    // Only the chunk that just became empty is considered. Erasing its owning
    // unique_ptr releases the provider allocation without moving other chunks.
    const auto candidate =
        std::find_if(chunks_.begin(), chunks_.end(), [chunk](const auto& owned_chunk) {
            return owned_chunk.get() == chunk;
        });
    if (candidate != chunks_.end()) {
        chunks_.erase(candidate);
    }
}

std::size_t
ChunkManager::calculate_next_growth(std::size_t current_blocks) const noexcept {
    if (options_.growth.mode == GrowthMode::fixed) {
        return options_.initial_blocks;
    }

    const std::size_t maximum = options_.growth.maximum_blocks_per_chunk;
    if (maximum != 0 && current_blocks >= maximum) {
        return maximum;
    }
    if (current_blocks >
        std::numeric_limits<std::size_t>::max() / options_.growth.factor) {
        // Saturation avoids wrapping to a small chunk count near size_t limits.
        return maximum != 0 ? maximum : std::numeric_limits<std::size_t>::max();
    }

    const std::size_t next = current_blocks * options_.growth.factor;
    return maximum != 0 ? std::min(next, maximum) : next;
}

}  // namespace memory_pool
