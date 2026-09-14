#pragma once

#include "memory_pool/chunk_policies.hpp"
#include "memory_pool/size_class_selector.hpp"

#include <cstddef>
#include <vector>

namespace memory_pool {

/// Configuration shared by all size-class pools. Requests unsupported by these
/// classes use a direct provider allocation rather than failing.
struct SegregatedAllocatorOptions {
    std::vector<std::size_t> size_classes{SizeClassSelector::default_size_classes()};
    std::size_t initial_blocks_per_class{64};
    GrowthPolicy growth{GrowthPolicy::geometric(2, 1024)};
    ReclamationPolicy reclamation{};
};

}  // namespace memory_pool
