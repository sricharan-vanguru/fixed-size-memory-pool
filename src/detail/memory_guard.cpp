#include "detail/memory_guard.hpp"

#include "memory_pool/pool_errors.hpp"

#include <cstring>

namespace memory_pool::detail {
namespace {

constexpr std::uint64_t front_canary = 0xA110CA7EA110CA7EULL;
constexpr std::uint64_t back_canary = 0xB10CCAFEB10CCAFEULL;
constexpr int allocated_pattern = 0xCD;
constexpr int freed_pattern = 0xDD;

void write_canaries(std::byte* payload, std::size_t block_size) noexcept {
    // memcpy avoids dereferencing a possibly unaligned uint64_t guard address.
    std::memcpy(payload - guard_size, &front_canary, guard_size);
    std::memcpy(payload + block_size, &back_canary, guard_size);
}

bool canaries_are_valid(const std::byte* payload, std::size_t block_size) noexcept {
    std::uint64_t front{};
    std::uint64_t back{};
    std::memcpy(&front, payload - guard_size, guard_size);
    std::memcpy(&back, payload + block_size, guard_size);
    return front == front_canary && back == back_canary;
}

}  // namespace

void prepare_allocated_memory(void* raw_block,
                              std::size_t payload_offset,
                              std::size_t block_size,
                              bool poison_memory,
                              bool guard_bytes) noexcept {
    auto* const payload = static_cast<std::byte*>(raw_block) + payload_offset;
    if (poison_memory) {
        std::memset(payload, allocated_pattern, block_size);
    }
    if (guard_bytes) {
        write_canaries(payload, block_size);
    }
}

void validate_and_prepare_freed_memory(void* raw_block,
                                       std::size_t payload_offset,
                                       std::size_t block_size,
                                       bool poison_memory,
                                       bool guard_bytes) {
    auto* const payload = static_cast<std::byte*>(raw_block) + payload_offset;
    if (guard_bytes && !canaries_are_valid(payload, block_size)) {
        throw MemoryCorruptionError("pool block guard bytes were modified");
    }
    if (poison_memory) {
        // Distinct patterns make use-before-initialization and use-after-free
        // easier to recognize in a debugger or memory dump.
        std::memset(payload, freed_pattern, block_size);
    }
}

}  // namespace memory_pool::detail
