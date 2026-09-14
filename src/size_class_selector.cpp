#include "memory_pool/size_class_selector.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace memory_pool {
namespace {

bool is_power_of_two(std::size_t value) noexcept {
    return value != 0 && (value & (value - 1U)) == 0U;
}

}  // namespace

SizeClassSelector::SizeClassSelector(std::vector<std::size_t> size_classes)
    : size_classes_(std::move(size_classes)) {
    if (size_classes_.empty()) {
        throw std::invalid_argument("at least one size class is required");
    }

    std::size_t previous = 0;
    for (const std::size_t class_size : size_classes_) {
        if (!is_power_of_two(class_size) || class_size < sizeof(void*)) {
            throw std::invalid_argument(
                "size classes must be power-of-two values large enough for a pointer");
        }
        if (class_size <= previous) {
            throw std::invalid_argument("size classes must be strictly increasing");
        }
        previous = class_size;
    }
}

std::vector<std::size_t> SizeClassSelector::default_size_classes() {
    return {8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096};
}

std::optional<std::size_t> SizeClassSelector::select(std::size_t size,
                                                     std::size_t alignment) const {
    if (!is_power_of_two(alignment)) {
        throw std::invalid_argument("alignment must be a non-zero power of two");
    }

    const std::size_t normalized_size = std::max<std::size_t>(size, 1);
    // With power-of-two classes, a class at least as large as the requested
    // alignment is also aligned strongly enough for the allocation.
    // Example: size=24, alignment=16 selects the 32-byte class.
    const std::size_t required_class = std::max(normalized_size, alignment);
    const auto selected =
        std::lower_bound(size_classes_.begin(), size_classes_.end(), required_class);
    if (selected == size_classes_.end()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(selected - size_classes_.begin());
}

std::size_t SizeClassSelector::class_size(std::size_t index) const {
    if (index >= size_classes_.size()) {
        throw std::out_of_range("size class index is out of range");
    }
    return size_classes_[index];
}

std::span<const std::size_t> SizeClassSelector::size_classes() const noexcept {
    return size_classes_;
}

std::size_t SizeClassSelector::class_count() const noexcept {
    return size_classes_.size();
}

std::size_t SizeClassSelector::maximum_size_class() const noexcept {
    return size_classes_.back();
}

}  // namespace memory_pool
