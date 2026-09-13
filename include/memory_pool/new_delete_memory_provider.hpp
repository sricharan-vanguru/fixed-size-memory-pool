#pragma once

#include "memory_pool/memory_provider.hpp"

namespace memory_pool {

class NewDeleteMemoryProvider final : public IMemoryProvider {
public:
    [[nodiscard]] void* allocate(std::size_t bytes,
                                 std::size_t alignment) override;
    void deallocate(void* memory,
                    std::size_t bytes,
                    std::size_t alignment) noexcept override;
};

}  // namespace memory_pool
