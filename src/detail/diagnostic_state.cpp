#include "detail/diagnostic_state.hpp"

#include "memory_pool/pool_errors.hpp"

namespace memory_pool::detail {

DiagnosticState::DiagnosticState(std::size_t block_count)
    : states_(block_count, BlockState::free) {}

bool DiagnosticState::try_mark_allocated(std::size_t index) noexcept {
    if (states_[index] != BlockState::free) {
        return false;
    }
    states_[index] = BlockState::allocated;
    return true;
}

void DiagnosticState::mark_free(std::size_t index) {
    if (states_[index] != BlockState::allocated) {
        throw DoubleFreeError("block is not currently allocated (possible double-free)");
    }
    states_[index] = BlockState::free;
}

bool DiagnosticState::is_allocated(std::size_t index) const noexcept {
    return states_[index] == BlockState::allocated;
}

const char* DiagnosticState::state_name(std::size_t index) const noexcept {
    return is_allocated(index) ? "allocated" : "free";
}

void DiagnosticState::validate_free_blocks(
    const std::vector<std::size_t>& free_indices,
    std::size_t expected_free_count) const {
    if (free_indices.size() != expected_free_count) {
        throw MemoryCorruptionError("free list length disagrees with available count");
    }

    // `seen` detects both a cycle/duplicate and disagreement between the state
    // table and free-list membership.
    std::vector<bool> seen(states_.size(), false);
    for (const std::size_t index : free_indices) {
        if (index >= states_.size()) {
            throw MemoryCorruptionError("free list contains an out-of-range block");
        }
        if (seen[index]) {
            throw MemoryCorruptionError("free list contains a duplicate block");
        }
        if (states_[index] != BlockState::free) {
            throw MemoryCorruptionError("allocated block is reachable from free list");
        }
        seen[index] = true;
    }

    for (std::size_t index = 0; index < states_.size(); ++index) {
        const bool state_is_free = states_[index] == BlockState::free;
        if (state_is_free != seen[index]) {
            throw MemoryCorruptionError("block state disagrees with free-list membership");
        }
    }
}

}  // namespace memory_pool::detail
