#include "detail/block_layout.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace memory_pool::detail {
namespace {

void validate_alignment(std::size_t alignment) {
    if (alignment == 0 || (alignment & (alignment - 1U)) != 0U) {
        throw std::invalid_argument("alignment must be a non-zero power of two");
    }
}

std::size_t checked_add(std::size_t left, std::size_t right) {
    if (left > std::numeric_limits<std::size_t>::max() - right) {
        throw std::length_error("requested pool size overflows size_t");
    }
    return left + right;
}

std::size_t checked_multiply(std::size_t left, std::size_t right) {
    if (right != 0 && left > std::numeric_limits<std::size_t>::max() / right) {
        throw std::length_error("requested pool size overflows size_t");
    }
    return left * right;
}

std::size_t round_up(std::size_t size, std::size_t alignment) {
    validate_alignment(alignment);
    if (size > std::numeric_limits<std::size_t>::max() - (alignment - 1U)) {
        throw std::length_error("block size is too large");
    }
    return (size + alignment - 1U) & ~(alignment - 1U);
}

}  // namespace

BlockLayout make_block_layout(std::size_t requested_block_size,
                              std::size_t block_count,
                              std::size_t requested_alignment,
                              bool guard_bytes,
                              std::size_t free_node_size,
                              std::size_t free_node_alignment,
                              std::size_t guard_size) {
    if (block_count == 0) {
        throw std::invalid_argument("block_count must be greater than zero");
    }

    validate_alignment(requested_alignment);
    const std::size_t alignment = std::max(requested_alignment, free_node_alignment);
    const std::size_t block_size =
        round_up(std::max(requested_block_size, free_node_size), alignment);
    const std::size_t payload_offset =
        guard_bytes ? round_up(std::max(guard_size, free_node_size), alignment) : 0U;

    std::size_t block_stride = block_size;
    if (guard_bytes) {
        const std::size_t payload_end = checked_add(payload_offset, block_size);
        block_stride = round_up(checked_add(payload_end, guard_size), alignment);
    }

    return BlockLayout{
        .block_size = block_size,
        .block_count = block_count,
        .alignment = alignment,
        .payload_offset = payload_offset,
        .block_stride = block_stride,
        .storage_size = checked_multiply(block_stride, block_count),
    };
}

}  // namespace memory_pool::detail
