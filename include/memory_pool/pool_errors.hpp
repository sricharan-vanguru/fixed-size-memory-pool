#pragma once

#include <stdexcept>

namespace memory_pool {

/// The address is foreign, null where forbidden, or not at a block boundary.
class InvalidPoolPointer final : public std::invalid_argument {
public:
    using std::invalid_argument::invalid_argument;
};

/// A block that is already free was returned again.
class DoubleFreeError final : public std::logic_error {
public:
    using std::logic_error::logic_error;
};

/// Guard bytes or a free-list invariant indicate overwritten pool storage.
class MemoryCorruptionError final : public std::logic_error {
public:
    using std::logic_error::logic_error;
};

/// Sized deallocation would route differently from the original allocation.
class AllocationMismatchError final : public std::logic_error {
public:
    using std::logic_error::logic_error;
};

}  // namespace memory_pool
