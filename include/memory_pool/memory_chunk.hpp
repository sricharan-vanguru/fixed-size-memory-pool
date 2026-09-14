#pragma once

#include "memory_pool/memory_provider.hpp"

#include <cstddef>
#include <memory>

namespace memory_pool {

/// One stable backing allocation divided into equal-size blocks. Allocation
/// state is always tracked so invalid and duplicate returns are deterministic.
class MemoryChunk {
  public:
    MemoryChunk(std::size_t block_size,
                std::size_t block_count,
                std::size_t alignment,
                MemoryProviderPtr provider);
    ~MemoryChunk();

    MemoryChunk(const MemoryChunk&) = delete;
    MemoryChunk& operator=(const MemoryChunk&) = delete;
    MemoryChunk(MemoryChunk&&) = delete;
    MemoryChunk& operator=(MemoryChunk&&) = delete;

    [[nodiscard]] void* allocate() noexcept;
    void deallocate(void* pointer);

    [[nodiscard]] bool owns(const void* pointer) const noexcept;
    [[nodiscard]] bool is_block_start(const void* pointer) const noexcept;
    [[nodiscard]] bool empty() const noexcept;

    [[nodiscard]] const void* data() const noexcept;
    [[nodiscard]] std::size_t storage_size() const noexcept;
    [[nodiscard]] std::size_t block_size() const noexcept;
    [[nodiscard]] std::size_t block_count() const noexcept;
    [[nodiscard]] std::size_t available() const noexcept;
    [[nodiscard]] std::size_t in_use() const noexcept;
    [[nodiscard]] std::size_t alignment() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace memory_pool
