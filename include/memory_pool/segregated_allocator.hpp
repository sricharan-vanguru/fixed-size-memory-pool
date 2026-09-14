#pragma once

#include "memory_pool/memory_provider.hpp"
#include "memory_pool/segregated_allocator_options.hpp"
#include "memory_pool/segregated_allocator_statistics.hpp"
#include "memory_pool/size_class_selector.hpp"

#include <cstddef>
#include <memory>
#include <optional>

namespace memory_pool {

/// Routes mixed-size requests to growing pools and uses the provider as a
/// fallback for requests unsupported by the configured size classes.
class SegregatedAllocator {
  public:
    explicit SegregatedAllocator(SegregatedAllocatorOptions options = {},
                                 MemoryProviderPtr provider = {});
    ~SegregatedAllocator();

    SegregatedAllocator(const SegregatedAllocator&) = delete;
    SegregatedAllocator& operator=(const SegregatedAllocator&) = delete;
    SegregatedAllocator(SegregatedAllocator&&) = delete;
    SegregatedAllocator& operator=(SegregatedAllocator&&) = delete;

    [[nodiscard]] void* allocate(std::size_t size,
                                 std::size_t alignment = alignof(std::max_align_t));
    void deallocate(void* pointer);

    /// Detects a different size-class route. Fallback allocations require an
    /// exact match with the size and alignment supplied to allocate().
    void deallocate(void* pointer, std::size_t size, std::size_t alignment);

    [[nodiscard]] bool owns(const void* pointer) const noexcept;
    [[nodiscard]] std::optional<std::size_t>
    owning_size_class(const void* pointer) const noexcept;
    [[nodiscard]] const SizeClassSelector& selector() const noexcept;
    [[nodiscard]] const SegregatedAllocatorStatistics& statistics() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace memory_pool
