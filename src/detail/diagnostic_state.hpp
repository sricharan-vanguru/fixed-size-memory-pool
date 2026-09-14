#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace memory_pool::detail {

/// Independent per-block truth used to validate the intrusive free list and to
/// report double frees without changing the normal fast-path representation.
class DiagnosticState {
  public:
    explicit DiagnosticState(std::size_t block_count);

    [[nodiscard]] bool try_mark_allocated(std::size_t index) noexcept;
    void mark_free(std::size_t index);
    [[nodiscard]] bool is_allocated(std::size_t index) const noexcept;
    [[nodiscard]] const char* state_name(std::size_t index) const noexcept;

    void validate_free_blocks(const std::vector<std::size_t>& free_indices,
                              std::size_t expected_free_count) const;

  private:
    enum class BlockState : std::uint8_t {
        free,
        allocated,
    };

    std::vector<BlockState> states_;
};

}  // namespace memory_pool::detail
