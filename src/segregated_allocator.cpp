#include "memory_pool/segregated_allocator.hpp"

#include "memory_pool/chunk_pool.hpp"
#include "memory_pool/new_delete_memory_provider.hpp"
#include "memory_pool/pool_errors.hpp"

#include <algorithm>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace memory_pool {
namespace {

std::size_t normalize_size(std::size_t size) noexcept {
    return std::max<std::size_t>(size, 1);
}

}  // namespace

struct SegregatedAllocator::Impl {
    struct FallbackAllocation {
        std::size_t size;
        std::size_t alignment;
    };

    Impl(SegregatedAllocatorOptions options, MemoryProviderPtr memory_provider)
        : selector(std::move(options.size_classes)),
          provider(memory_provider != nullptr
                       ? std::move(memory_provider)
                       : std::make_shared<NewDeleteMemoryProvider>()) {
        if (options.initial_blocks_per_class == 0) {
            throw std::invalid_argument(
                "initial_blocks_per_class must be greater than zero");
        }

        pools.reserve(selector.class_count());
        statistics.size_classes.reserve(selector.class_count());
        for (const std::size_t class_size : selector.size_classes()) {
            // Power-of-two class size is also a sufficient alignment for every
            // request routed to that class.
            pools.push_back(std::make_unique<GrowingChunkPool>(
                ChunkManagerOptions{
                    .block_size = class_size,
                    .initial_blocks = options.initial_blocks_per_class,
                    .alignment = class_size,
                    .growth = options.growth,
                    .reclamation = options.reclamation,
                },
                provider));
            statistics.size_classes.push_back(
                SizeClassStatistics{.class_size = class_size});
        }
    }

    ~Impl() {
        for (const auto& [pointer, metadata] : fallback_allocations) {
            provider->deallocate(pointer, metadata.size, metadata.alignment);
        }
    }

    [[nodiscard]] std::optional<std::size_t>
    owning_class(const void* pointer) const noexcept {
        for (std::size_t index = 0; index < pools.size(); ++index) {
            if (pools[index]->owns(pointer)) {
                return index;
            }
        }
        return std::nullopt;
    }

    SizeClassSelector selector;
    MemoryProviderPtr provider;
    std::vector<std::unique_ptr<GrowingChunkPool>> pools;
    std::unordered_map<void*, FallbackAllocation> fallback_allocations;
    SegregatedAllocatorStatistics statistics;
};

SegregatedAllocator::SegregatedAllocator(SegregatedAllocatorOptions options,
                                         MemoryProviderPtr provider)
    : impl_(std::make_unique<Impl>(std::move(options), std::move(provider))) {}

SegregatedAllocator::~SegregatedAllocator() = default;

void* SegregatedAllocator::allocate(std::size_t size, std::size_t alignment) {
    ++impl_->statistics.allocation_requests;
    try {
        const std::size_t requested_size = normalize_size(size);
        const auto selected = impl_->selector.select(requested_size, alignment);
        if (selected.has_value()) {
            const std::size_t index = *selected;
            SizeClassStatistics& class_statistics =
                impl_->statistics.size_classes[index];
            ++class_statistics.allocation_requests;

            void* const pointer = impl_->pools[index]->allocate();
            ++class_statistics.successful_allocations;
            ++class_statistics.currently_allocated;
            class_statistics.peak_allocated = std::max(
                class_statistics.peak_allocated, class_statistics.currently_allocated);
            class_statistics.requested_bytes += requested_size;
            class_statistics.served_bytes += class_statistics.class_size;
            class_statistics.internal_fragmentation_bytes +=
                class_statistics.class_size - requested_size;
            ++impl_->statistics.successful_allocations;
            return pointer;
        }

        // Large or unsupported over-aligned requests bypass size classes. Exact
        // metadata is required because the provider needs it during release.
        void* const pointer = impl_->provider->allocate(requested_size, alignment);
        try {
            const auto [allocation, inserted] =
                impl_->fallback_allocations.emplace(pointer,
                                                    Impl::FallbackAllocation{
                                                        .size = requested_size,
                                                        .alignment = alignment,
                                                    });
            static_cast<void>(allocation);
            if (!inserted) {
                throw std::logic_error(
                    "memory provider returned a duplicate live address");
            }
        } catch (...) {
            // Strong exception safety: a failed metadata insertion must not
            // orphan the provider allocation.
            impl_->provider->deallocate(pointer, requested_size, alignment);
            throw;
        }

        ++impl_->statistics.successful_allocations;
        ++impl_->statistics.fallback_allocations;
        ++impl_->statistics.current_fallback_allocations;
        impl_->statistics.peak_fallback_allocations =
            std::max(impl_->statistics.peak_fallback_allocations,
                     impl_->statistics.current_fallback_allocations);
        impl_->statistics.fallback_requested_bytes += requested_size;
        return pointer;
    } catch (...) {
        ++impl_->statistics.failed_allocations;
        throw;
    }
}

void SegregatedAllocator::deallocate(void* pointer) {
    if (pointer == nullptr) {
        return;
    }

    // Pooled blocks carry no per-allocation header, so unsized deallocation
    // discovers the owner by examining stable chunk ranges.
    if (const auto owner = impl_->owning_class(pointer); owner.has_value()) {
        impl_->pools[*owner]->deallocate(pointer);
        SizeClassStatistics& class_statistics = impl_->statistics.size_classes[*owner];
        ++class_statistics.deallocations;
        --class_statistics.currently_allocated;
        ++impl_->statistics.deallocations;
        return;
    }

    const auto fallback = impl_->fallback_allocations.find(pointer);
    if (fallback == impl_->fallback_allocations.end()) {
        throw InvalidPoolPointer("pointer is not owned by this segregated allocator");
    }

    impl_->provider->deallocate(
        pointer, fallback->second.size, fallback->second.alignment);
    impl_->fallback_allocations.erase(fallback);
    ++impl_->statistics.deallocations;
    ++impl_->statistics.fallback_deallocations;
    --impl_->statistics.current_fallback_allocations;
}

void SegregatedAllocator::deallocate(void* pointer,
                                     std::size_t size,
                                     std::size_t alignment) {
    if (pointer == nullptr) {
        return;
    }

    if (const auto owner = impl_->owning_class(pointer); owner.has_value()) {
        // Sizes inside one class are intentionally indistinguishable. This
        // check catches a different routing class, not every byte mismatch.
        const auto supplied_class = impl_->selector.select(size, alignment);
        if (!supplied_class.has_value() || *supplied_class != *owner) {
            throw AllocationMismatchError(
                "supplied size and alignment select a different size class");
        }
        deallocate(pointer);
        return;
    }

    const auto fallback = impl_->fallback_allocations.find(pointer);
    if (fallback == impl_->fallback_allocations.end()) {
        throw InvalidPoolPointer("pointer is not owned by this segregated allocator");
    }
    if (fallback->second.size != normalize_size(size) ||
        fallback->second.alignment != alignment) {
        throw AllocationMismatchError(
            "supplied size or alignment does not match the fallback allocation");
    }
    deallocate(pointer);
}

bool SegregatedAllocator::owns(const void* pointer) const noexcept {
    return impl_->owning_class(pointer).has_value() ||
           impl_->fallback_allocations.contains(const_cast<void*>(pointer));
}

std::optional<std::size_t>
SegregatedAllocator::owning_size_class(const void* pointer) const noexcept {
    const auto owner = impl_->owning_class(pointer);
    return owner.has_value()
               ? std::optional<std::size_t>{impl_->selector.class_size(*owner)}
               : std::nullopt;
}

const SizeClassSelector& SegregatedAllocator::selector() const noexcept {
    return impl_->selector;
}

const SegregatedAllocatorStatistics& SegregatedAllocator::statistics() const noexcept {
    return impl_->statistics;
}

}  // namespace memory_pool
