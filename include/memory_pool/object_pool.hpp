#pragma once

#include "memory_pool/fixed_size_memory_pool.hpp"
#include "memory_pool/pool_ptr.hpp"

#include <cstddef>
#include <type_traits>
#include <utility>

namespace memory_pool {

/// Type-safe object-lifetime facade over FixedSizeMemoryPool.
/// The pool must outlive every raw pointer and PoolPtr created from it.
template <typename T>
class ObjectPool {
    static_assert(std::is_object_v<T> && !std::is_array_v<T> &&
                      !std::is_const_v<T> && !std::is_volatile_v<T> &&
                      std::is_destructible_v<T>,
                  "ObjectPool requires a destructible, non-cv object type");

public:
    using value_type = T;

    explicit ObjectPool(std::size_t capacity, PoolOptions options = {})
        : pool_(sizeof(T), capacity, alignof(T), options) {}

    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;
    ObjectPool(ObjectPool&&) = delete;
    ObjectPool& operator=(ObjectPool&&) = delete;

    template <typename... Args>
    [[nodiscard]] T* create(Args&&... args) & {
        return pool_.create<T>(std::forward<Args>(args)...);
    }

    template <typename... Args>
    [[nodiscard]] T* create(Args&&...) && = delete;

    void destroy(T* object) { pool_.destroy(object); }

    template <typename... Args>
    [[nodiscard]] PoolPtr<T> make_unique(Args&&... args) & {
        // The custom deleter remembers the originating pool; it does not own it.
        T* const object = create(std::forward<Args>(args)...);
        return PoolPtr<T>{object, PoolDeleter<T>{pool_}};
    }

    template <typename... Args>
    [[nodiscard]] PoolPtr<T> make_unique(Args&&...) && = delete;

    [[nodiscard]] bool owns(const T* object) const noexcept { return pool_.owns(object); }
    [[nodiscard]] std::size_t capacity() const noexcept { return pool_.capacity(); }
    [[nodiscard]] std::size_t available() const noexcept { return pool_.available(); }
    [[nodiscard]] std::size_t in_use() const noexcept { return pool_.in_use(); }
    [[nodiscard]] bool diagnostics_enabled() const noexcept {
        return pool_.diagnostics_enabled();
    }
    [[nodiscard]] const PoolStatistics& statistics() const noexcept {
        return pool_.statistics();
    }

private:
    FixedSizeMemoryPool pool_;
};

}  // namespace memory_pool
