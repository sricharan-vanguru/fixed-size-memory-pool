#pragma once

#include <cstddef>
#include <cstdint>

namespace memory_pool::detail {

inline constexpr std::size_t guard_size = sizeof(std::uint64_t);

/// Writes the allocated poison pattern and fresh front/back canaries according
/// to the enabled options.
void prepare_allocated_memory(void* raw_block,
                              std::size_t payload_offset,
                              std::size_t block_size,
                              bool poison_memory,
                              bool guard_bytes) noexcept;

/// Checks canaries before overwriting payload bytes with the freed pattern.
void validate_and_prepare_freed_memory(void* raw_block,
                                       std::size_t payload_offset,
                                       std::size_t block_size,
                                       bool poison_memory,
                                       bool guard_bytes);

}  // namespace memory_pool::detail
