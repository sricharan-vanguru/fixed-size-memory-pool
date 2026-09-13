#include "memory_pool/thread_cached_allocator.hpp"

#include "detail/thread_cache_state.hpp"

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <utility>

namespace memory_pool {

struct ThreadCachedAllocator::Impl {
    struct LocalCache {
        std::weak_ptr<detail::ThreadCacheState> state;
        detail::ThreadCacheState::CacheBins bins;
    };

    struct ThreadRegistry {
        ~ThreadRegistry() {
            for (auto& [id, cache] : caches) {
                static_cast<void>(id);
                if (const auto state = cache.state.lock()) {
                    try {
                        static_cast<void>(state->flush_bins(cache.bins));
                    } catch (...) {
                    }
                }
            }
        }

        [[nodiscard]] LocalCache& get(
            const std::shared_ptr<detail::ThreadCacheState>& state) {
            auto existing = caches.find(state->id());
            if (existing != caches.end() && !existing->second.state.expired()) {
                return existing->second;
            }

            LocalCache replacement;
            replacement.state = state;
            replacement.bins.resize(state->class_count());
            for (auto& bin : replacement.bins) {
                bin.reserve(state->cache_capacity());
            }

            if (existing != caches.end()) {
                existing->second = std::move(replacement);
                return existing->second;
            }
            return caches.emplace(state->id(), std::move(replacement))
                .first->second;
        }

        [[nodiscard]] std::size_t release(std::uint64_t id) {
            const auto entry = caches.find(id);
            if (entry == caches.end()) {
                return 0;
            }
            std::size_t released = 0;
            if (const auto state = entry->second.state.lock()) {
                released = state->flush_bins(entry->second.bins);
            }
            caches.erase(entry);
            return released;
        }

        std::unordered_map<std::uint64_t, LocalCache> caches;
    };

    Impl(SegregatedAllocatorOptions allocator_options,
         ThreadCacheOptions cache_options,
         MemoryProviderPtr provider)
        : state(std::make_shared<detail::ThreadCacheState>(
              std::move(allocator_options),
              cache_options,
              std::move(provider))) {}

    ~Impl() {
        try {
            static_cast<void>(registry.release(state->id()));
        } catch (...) {
        }
    }

    static thread_local ThreadRegistry registry;
    std::shared_ptr<detail::ThreadCacheState> state;
};

thread_local ThreadCachedAllocator::Impl::ThreadRegistry
    ThreadCachedAllocator::Impl::registry;

ThreadCachedAllocator::ThreadCachedAllocator(
    SegregatedAllocatorOptions allocator_options,
    ThreadCacheOptions cache_options,
    MemoryProviderPtr provider)
    : impl_(std::make_unique<Impl>(std::move(allocator_options),
                                   cache_options,
                                   std::move(provider))) {}

ThreadCachedAllocator::~ThreadCachedAllocator() = default;

void* ThreadCachedAllocator::allocate(std::size_t size, std::size_t alignment) {
    Impl::LocalCache& cache = Impl::registry.get(impl_->state);
    return impl_->state->allocate(cache.bins, size, alignment);
}

void ThreadCachedAllocator::deallocate(void* pointer) {
    if (pointer == nullptr) {
        return;
    }
    Impl::LocalCache& cache = Impl::registry.get(impl_->state);
    impl_->state->deallocate(cache.bins, pointer, nullptr, nullptr);
}

void ThreadCachedAllocator::deallocate(void* pointer,
                                       std::size_t size,
                                       std::size_t alignment) {
    if (pointer == nullptr) {
        return;
    }
    Impl::LocalCache& cache = Impl::registry.get(impl_->state);
    impl_->state->deallocate(cache.bins, pointer, &size, &alignment);
}

std::size_t ThreadCachedAllocator::release_current_thread_cache() {
    return Impl::registry.release(impl_->state->id());
}

bool ThreadCachedAllocator::owns(const void* pointer) const {
    return impl_->state->owns(pointer);
}

std::optional<std::size_t> ThreadCachedAllocator::owning_size_class(
    const void* pointer) const {
    return impl_->state->owning_size_class(pointer);
}

ThreadCacheStatistics ThreadCachedAllocator::statistics() const noexcept {
    return impl_->state->snapshot();
}

SegregatedAllocatorStatistics ThreadCachedAllocator::central_statistics() const {
    return impl_->state->central_snapshot();
}

}  // namespace memory_pool
