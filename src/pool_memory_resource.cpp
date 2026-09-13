#include "memory_pool/pool_memory_resource.hpp"

#include "memory_pool/memory_provider.hpp"
#include "memory_pool/segregated_allocator.hpp"

#include <memory>
#include <stdexcept>
#include <utility>

namespace memory_pool {
namespace {

std::pmr::memory_resource* validate_upstream(
    std::pmr::memory_resource* upstream) {
    if (upstream == nullptr) {
        throw std::invalid_argument("upstream memory resource must not be null");
    }
    return upstream;
}

class PmrUpstreamProvider final : public IMemoryProvider {
public:
    explicit PmrUpstreamProvider(std::pmr::memory_resource* upstream)
        : upstream_(validate_upstream(upstream)) {}

    [[nodiscard]] void* allocate(std::size_t bytes,
                                 std::size_t alignment) override {
        return upstream_->allocate(bytes, alignment);
    }

    void deallocate(void* memory,
                    std::size_t bytes,
                    std::size_t alignment) noexcept override {
        upstream_->deallocate(memory, bytes, alignment);
    }

private:
    std::pmr::memory_resource* upstream_;
};

}  // namespace

struct PoolMemoryResource::Impl {
    Impl(SegregatedAllocatorOptions options,
         std::pmr::memory_resource* upstream_resource)
        : upstream(validate_upstream(upstream_resource)),
          provider(std::make_shared<PmrUpstreamProvider>(upstream)),
          allocator(std::move(options), provider) {}

    std::pmr::memory_resource* upstream;
    MemoryProviderPtr provider;
    SegregatedAllocator allocator;
};

PoolMemoryResource::PoolMemoryResource(
    SegregatedAllocatorOptions options,
    std::pmr::memory_resource* upstream)
    : impl_(std::make_unique<Impl>(std::move(options), upstream)) {}

PoolMemoryResource::~PoolMemoryResource() = default;

std::pmr::memory_resource* PoolMemoryResource::upstream_resource() const noexcept {
    return impl_->upstream;
}

const SegregatedAllocatorStatistics& PoolMemoryResource::statistics() const noexcept {
    return impl_->allocator.statistics();
}

void* PoolMemoryResource::do_allocate(std::size_t bytes, std::size_t alignment) {
    return impl_->allocator.allocate(bytes, alignment);
}

void PoolMemoryResource::do_deallocate(void* pointer,
                                       std::size_t bytes,
                                       std::size_t alignment) {
    impl_->allocator.deallocate(pointer, bytes, alignment);
}

bool PoolMemoryResource::do_is_equal(
    const std::pmr::memory_resource& other) const noexcept {
    return this == &other;
}

}  // namespace memory_pool
