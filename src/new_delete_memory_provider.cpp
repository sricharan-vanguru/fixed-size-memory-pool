#include "memory_pool/new_delete_memory_provider.hpp"

#include <new>
#include <stdexcept>

namespace memory_pool {
namespace {

bool is_power_of_two(std::size_t value) noexcept {
    return value != 0 && (value & (value - 1U)) == 0U;
}

bool needs_extended_alignment(std::size_t alignment) noexcept {
    return alignment > static_cast<std::size_t>(__STDCPP_DEFAULT_NEW_ALIGNMENT__);
}

}  // namespace

void* NewDeleteMemoryProvider::allocate(std::size_t bytes, std::size_t alignment) {
    if (bytes == 0) {
        throw std::invalid_argument("provider allocation size must be greater than zero");
    }
    if (!is_power_of_two(alignment)) {
        throw std::invalid_argument("provider alignment must be a non-zero power of two");
    }
    if (needs_extended_alignment(alignment)) {
        return ::operator new(bytes, std::align_val_t{alignment});
    }
    return ::operator new(bytes);
}

void NewDeleteMemoryProvider::deallocate(void* memory,
                                         std::size_t,
                                         std::size_t alignment) noexcept {
    if (memory == nullptr) {
        return;
    }
    if (needs_extended_alignment(alignment)) {
        ::operator delete(memory, std::align_val_t{alignment});
        return;
    }
    ::operator delete(memory);
}

}  // namespace memory_pool
