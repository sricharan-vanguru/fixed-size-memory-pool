#pragma once

#include <cstddef>

namespace memory_pool {

enum class ArenaGrowthMode {
    fixed,
    geometric,
};

struct ArenaGrowthPolicy {
    ArenaGrowthMode mode{ArenaGrowthMode::geometric};
    std::size_t factor{2};
    std::size_t maximum_chunk_size{1024 * 1024};

    [[nodiscard]] static constexpr ArenaGrowthPolicy fixed() noexcept {
        return {.mode = ArenaGrowthMode::fixed,
                .factor = 1,
                .maximum_chunk_size = 0};
    }

    [[nodiscard]] static constexpr ArenaGrowthPolicy geometric(
        std::size_t growth_factor = 2,
        std::size_t maximum_size = 1024 * 1024) noexcept {
        return {.mode = ArenaGrowthMode::geometric,
                .factor = growth_factor,
                .maximum_chunk_size = maximum_size};
    }
};

enum class ArenaResetPolicy {
    retain_all_chunks,
    retain_initial_chunk,
};

struct MonotonicArenaOptions {
    std::size_t initial_chunk_size{4096};
    std::size_t initial_alignment{alignof(std::max_align_t)};
    ArenaGrowthPolicy growth{};
    ArenaResetPolicy reset_policy{ArenaResetPolicy::retain_all_chunks};
};

}  // namespace memory_pool
