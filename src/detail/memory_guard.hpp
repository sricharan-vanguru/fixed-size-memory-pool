#pragma once

#include <cstddef>
#include <cstdint>

namespace memory_pool::detail {

inline constexpr std::size_t guard_size = sizeof(std::uint64_t);

void prepare_allocated_memory(void* raw_block,
                              std::size_t payload_offset,
                              std::size_t block_size,
                              bool poison_memory,
                              bool guard_bytes) noexcept;

void validate_and_prepare_freed_memory(void* raw_block,
                                       std::size_t payload_offset,
                                       std::size_t block_size,
                                       bool poison_memory,
                                       bool guard_bytes);

}  // namespace memory_pool::detail
