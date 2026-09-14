#include "test_support.hpp"

#include <cstddef>
#include <cstring>
#include <sstream>
#include <string>

namespace diagnostics_tests {
namespace {

// Opt-in safety behavior is tested independently from the default fast path.
struct Tracked {
    static inline int alive = 0;

    Tracked() { ++alive; }
    ~Tracked() { --alive; }
};

std::size_t reported_leaks = 0;

void record_leaks(std::size_t outstanding) noexcept { reported_leaks = outstanding; }

void double_free_and_state_queries(TestContext& test) {
    memory_pool::FixedSizeMemoryPool pool(
        32, 2, alignof(std::max_align_t), diagnostic_options());
    void* block = pool.allocate();
    test.expect(pool.block_index(block) == 0,
                "the first allocation should use block zero");
    test.expect(pool.is_allocated(block), "an allocated block should be tracked");
    pool.validate_integrity();

    pool.deallocate(block);
    test.expect(!pool.is_allocated(block),
                "a returned block should be tracked as free");
    pool.validate_integrity();
    test.expect_throws<memory_pool::DoubleFreeError>(
        [&] { pool.deallocate(block); }, "diagnostic mode should detect a double-free");
    pool.validate_integrity();
}

void free_list_corruption(TestContext& test) {
    memory_pool::FixedSizeMemoryPool pool(
        32, 2, alignof(std::max_align_t), diagnostic_options());
    void* block = pool.allocate();
    pool.deallocate(block);

    // A released unguarded block begins with FreeNode::next. Point it to itself to
    // simulate corruption without depending on the private FreeNode type.
    std::memcpy(block, &block, sizeof(block));
    test.expect_throws<memory_pool::MemoryCorruptionError>(
        [&] { pool.validate_integrity(); },
        "integrity validation should detect a free-list cycle");
}

void state_requires_diagnostics(TestContext& test) {
    memory_pool::FixedSizeMemoryPool pool(16, 1);
    void* block = pool.allocate();
    test.expect_throws<std::logic_error>(
        [&] { static_cast<void>(pool.is_allocated(block)); },
        "allocation state should not be guessed without diagnostics");
    pool.deallocate(block);
}

void invalid_destroy_is_safe(TestContext& test) {
    memory_pool::FixedSizeMemoryPool pool(
        sizeof(Tracked), 1, alignof(Tracked), diagnostic_options());
    Tracked external;
    const int alive_before = Tracked::alive;
    test.expect_throws<memory_pool::InvalidPoolPointer>(
        [&] { pool.destroy(&external); },
        "destroy should reject a foreign pointer before its destructor runs");
    test.expect(Tracked::alive == alive_before,
                "rejecting a foreign object should not invoke its destructor");
}

void memory_poisoning(TestContext& test) {
    memory_pool::FixedSizeMemoryPool pool(
        32, 1, alignof(std::max_align_t), diagnostic_options(false, true));
    auto* block = static_cast<std::byte*>(pool.allocate());
    bool allocated_pattern_present = true;
    for (std::size_t index = 0; index < pool.block_size(); ++index) {
        allocated_pattern_present =
            allocated_pattern_present && block[index] == std::byte{0xCD};
    }
    test.expect(allocated_pattern_present,
                "allocated memory should use the debug pattern");

    pool.deallocate(block);
    test.expect(block[sizeof(void*)] == std::byte{0xDD},
                "released bytes outside FreeNode should use the free pattern");
}

void guard_corruption(TestContext& test) {
    {
        memory_pool::FixedSizeMemoryPool pool(
            16, 1, alignof(std::max_align_t), diagnostic_options(false, false, true));
        auto* block = static_cast<std::byte*>(pool.allocate());
        block[pool.block_size()] = std::byte{0};
        test.expect_throws<memory_pool::MemoryCorruptionError>(
            [&] { pool.deallocate(block); }, "a back-canary change should be detected");
    }
    {
        memory_pool::FixedSizeMemoryPool pool(
            16, 1, alignof(std::max_align_t), diagnostic_options(false, false, true));
        auto* block = static_cast<std::byte*>(pool.allocate());
        block[-1] = std::byte{0};
        test.expect_throws<memory_pool::MemoryCorruptionError>(
            [&] { pool.deallocate(block); },
            "a front-canary change should be detected");
    }
}

void diagnostic_dump(TestContext& test) {
    memory_pool::FixedSizeMemoryPool pool(
        16, 2, alignof(std::max_align_t), diagnostic_options());
    void* block = pool.allocate();
    std::ostringstream output;
    pool.debug_dump(output);
    test.expect(output.str().find("block[0]=allocated") != std::string::npos,
                "debug dump should show allocated state");
    test.expect(output.str().find("block[1]=free") != std::string::npos,
                "debug dump should show free state");
    test.expect(output.str().find("free_list=1") != std::string::npos,
                "debug dump should show free-list order");
    pool.deallocate(block);
}

void leak_callback(TestContext& test) {
    reported_leaks = 0;
    {
        auto options = diagnostic_options();
        options.leak_handler = record_leaks;
        memory_pool::FixedSizeMemoryPool pool(
            16, 2, alignof(std::max_align_t), options);
        static_cast<void>(pool.allocate());
    }
    test.expect(reported_leaks == 1,
                "destruction should report outstanding blocks through the callback");
}

}  // namespace

void run(TestContext& test) {
    double_free_and_state_queries(test);
    free_list_corruption(test);
    state_requires_diagnostics(test);
    invalid_destroy_is_safe(test);
    memory_poisoning(test);
    guard_corruption(test);
    diagnostic_dump(test);
    leak_callback(test);
}

}  // namespace diagnostics_tests
