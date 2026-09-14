#pragma once

#include <cstddef>

namespace memory_pool::detail {

/// Normalized dimensions shared by allocation, pointer validation, and guard
/// handling. `block_stride` is the distance between two raw block starts.
struct BlockLayout {
    std::size_t block_size{};
    std::size_t block_count{};
    std::size_t alignment{};
    std::size_t payload_offset{};
    std::size_t block_stride{};
    std::size_t storage_size{};
};

BlockLayout make_block_layout(std::size_t requested_block_size,
                              std::size_t block_count,
                              std::size_t requested_alignment,
                              bool guard_bytes,
                              std::size_t free_node_size,
                              std::size_t free_node_alignment,
                              std::size_t guard_size);

}  // namespace memory_pool::detail
