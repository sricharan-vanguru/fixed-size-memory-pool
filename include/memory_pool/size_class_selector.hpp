#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace memory_pool {

/// Maps size/alignment requests to the smallest compatible power-of-two class.
class SizeClassSelector {
public:
    explicit SizeClassSelector(
        std::vector<std::size_t> size_classes = default_size_classes());

    [[nodiscard]] static std::vector<std::size_t> default_size_classes();

    [[nodiscard]] std::optional<std::size_t> select(
        std::size_t size,
        std::size_t alignment) const;
    [[nodiscard]] std::size_t class_size(std::size_t index) const;
    [[nodiscard]] std::span<const std::size_t> size_classes() const noexcept;
    [[nodiscard]] std::size_t class_count() const noexcept;
    [[nodiscard]] std::size_t maximum_size_class() const noexcept;

private:
    std::vector<std::size_t> size_classes_;
};

}  // namespace memory_pool
