#pragma once

#include "memory_pool/segregated_allocator_options.hpp"
#include "memory_pool/segregated_allocator_statistics.hpp"

#include <cstddef>
#include <memory>
#include <memory_resource>

namespace memory_pool {

/// Unsynchronized PMR adapter over SegregatedAllocator. Containers using this
/// resource must be destroyed before the resource, and the upstream resource
/// must outlive this object.
class PoolMemoryResource final : public std::pmr::memory_resource {
public:
    explicit PoolMemoryResource(
        SegregatedAllocatorOptions options = {},
        std::pmr::memory_resource* upstream = std::pmr::get_default_resource());
    ~PoolMemoryResource() override;

    PoolMemoryResource(const PoolMemoryResource&) = delete;
    PoolMemoryResource& operator=(const PoolMemoryResource&) = delete;
    PoolMemoryResource(PoolMemoryResource&&) = delete;
    PoolMemoryResource& operator=(PoolMemoryResource&&) = delete;

    [[nodiscard]] std::pmr::memory_resource* upstream_resource() const noexcept;
    [[nodiscard]] const SegregatedAllocatorStatistics& statistics() const noexcept;

private:
    [[nodiscard]] void* do_allocate(std::size_t bytes,
                                    std::size_t alignment) override;
    void do_deallocate(void* pointer,
                       std::size_t bytes,
                       std::size_t alignment) override;
    [[nodiscard]] bool do_is_equal(
        const std::pmr::memory_resource& other) const noexcept override;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace memory_pool
