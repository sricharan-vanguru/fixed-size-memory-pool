#pragma once

#include "memory_pool/chunk_policies.hpp"
#include "memory_pool/memory_chunk.hpp"
#include "memory_pool/memory_provider.hpp"

#include <cstddef>
#include <memory>
#include <vector>

namespace memory_pool {

/// Owns stable chunks and performs growth and empty-chunk reclamation.
class ChunkManager {
  public:
    explicit ChunkManager(ChunkManagerOptions options, MemoryProviderPtr provider = {});
    ~ChunkManager();

    ChunkManager(const ChunkManager&) = delete;
    ChunkManager& operator=(const ChunkManager&) = delete;
    ChunkManager(ChunkManager&&) = delete;
    ChunkManager& operator=(ChunkManager&&) = delete;

    /// Uses existing chunks only and never calls the memory provider.
    [[nodiscard]] void* try_allocate() noexcept;
    void deallocate(void* pointer);

    /// Adds one chunk. Existing allocation addresses remain unchanged.
    MemoryChunk& grow();

    [[nodiscard]] MemoryChunk* find_chunk(const void* pointer) noexcept;
    [[nodiscard]] const MemoryChunk* find_chunk(const void* pointer) const noexcept;

    [[nodiscard]] std::size_t chunk_count() const noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept;
    [[nodiscard]] std::size_t available() const noexcept;
    [[nodiscard]] std::size_t in_use() const noexcept;
    [[nodiscard]] std::size_t block_size() const noexcept;
    [[nodiscard]] std::size_t alignment() const noexcept;
    [[nodiscard]] std::size_t next_growth_block_count() const noexcept;
    [[nodiscard]] IMemoryProvider& memory_provider() noexcept;

  private:
    void reclaim_if_allowed(MemoryChunk* chunk) noexcept;
    [[nodiscard]] std::size_t
    calculate_next_growth(std::size_t current_blocks) const noexcept;

    ChunkManagerOptions options_;
    MemoryProviderPtr provider_;
    std::vector<std::unique_ptr<MemoryChunk>> chunks_;
    std::size_t block_size_{};
    std::size_t alignment_{};
    std::size_t next_growth_blocks_{};
};

}  // namespace memory_pool
