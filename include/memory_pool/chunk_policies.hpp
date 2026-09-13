#pragma once

#include <cstddef>

namespace memory_pool {

enum class GrowthMode {
    fixed,
    geometric,
};

struct GrowthPolicy {
    GrowthMode mode{GrowthMode::fixed};
    std::size_t factor{2};
    std::size_t maximum_blocks_per_chunk{0};

    [[nodiscard]] static constexpr GrowthPolicy fixed() noexcept {
        return GrowthPolicy{};
    }

    [[nodiscard]] static constexpr GrowthPolicy geometric(
        std::size_t growth_factor = 2,
        std::size_t maximum_blocks = 0) noexcept {
        return GrowthPolicy{
            .mode = GrowthMode::geometric,
            .factor = growth_factor,
            .maximum_blocks_per_chunk = maximum_blocks,
        };
    }
};

/// Number of completely free chunks retained after deallocation.
struct ReclamationPolicy {
    std::size_t spare_empty_chunks{1};
};

struct ChunkManagerOptions {
    std::size_t block_size{};
    std::size_t initial_blocks{};
    std::size_t alignment{alignof(std::max_align_t)};
    GrowthPolicy growth{};
    ReclamationPolicy reclamation{};
};

}  // namespace memory_pool
