#pragma once

#include "memory_pool/fixed_size_memory_pool.hpp"

#include <exception>
#include <memory>

namespace memory_pool {

/// Returns an object to its originating pool. The pool is non-owning and must
/// remain alive until every pointer using this deleter has been destroyed.
template <typename T> class PoolDeleter {
  public:
    PoolDeleter() noexcept = default;
    explicit PoolDeleter(FixedSizeMemoryPool& pool) noexcept : pool_(&pool) {}

    void operator()(T* object) const noexcept {
        if (object == nullptr) {
            return;
        }
        if (pool_ == nullptr) {
            std::terminate();
        }
        pool_->destroy(object);
    }

  private:
    FixedSizeMemoryPool* pool_{nullptr};
};

template <typename T> using PoolPtr = std::unique_ptr<T, PoolDeleter<T>>;

}  // namespace memory_pool
