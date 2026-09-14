#pragma once

#include "memory_pool/memory_provider.hpp"

#include <cstddef>
#include <optional>

namespace memory_pool::detail {

struct ArenaAllocation {
    void* pointer{};
    std::size_t previous_offset{};
    std::size_t consumed_bytes{};
};

class ArenaChunk {
public:
    ArenaChunk(std::size_t capacity,
               std::size_t alignment,
               MemoryProviderPtr provider);
    ~ArenaChunk();

    ArenaChunk(const ArenaChunk&) = delete;
    ArenaChunk& operator=(const ArenaChunk&) = delete;
    ArenaChunk(ArenaChunk&&) = delete;
    ArenaChunk& operator=(ArenaChunk&&) = delete;

    [[nodiscard]] std::optional<ArenaAllocation> try_allocate(
        std::size_t size,
        std::size_t alignment) noexcept;
    void rewind(std::size_t previous_offset) noexcept;
    void reset() noexcept;

    [[nodiscard]] bool owns(const void* pointer) const noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept;
    [[nodiscard]] std::size_t used() const noexcept;
    [[nodiscard]] std::size_t alignment() const noexcept;

private:
    MemoryProviderPtr provider_;
    std::byte* storage_{};
    std::size_t capacity_{};
    std::size_t alignment_{};
    std::size_t offset_{};
};

}  // namespace memory_pool::detail
