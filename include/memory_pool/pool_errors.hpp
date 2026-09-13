#pragma once

#include <stdexcept>

namespace memory_pool {

class InvalidPoolPointer final : public std::invalid_argument {
public:
    using std::invalid_argument::invalid_argument;
};

class DoubleFreeError final : public std::logic_error {
public:
    using std::logic_error::logic_error;
};

class MemoryCorruptionError final : public std::logic_error {
public:
    using std::logic_error::logic_error;
};

class AllocationMismatchError final : public std::logic_error {
public:
    using std::logic_error::logic_error;
};

}  // namespace memory_pool
