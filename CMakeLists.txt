cmake_minimum_required(VERSION 3.20)
project(fixed_size_memory_pool VERSION 1.5.0 LANGUAGES CXX)

option(MEMORY_POOL_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

function(memory_pool_enable_warnings target)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
  endif()
endfunction()

add_library(memory_pool
  src/chunk_manager.cpp
  src/fixed_size_memory_pool.cpp
  src/memory_chunk.cpp
  src/new_delete_memory_provider.cpp
  src/pool_memory_resource.cpp
  src/segregated_allocator.cpp
  src/size_class_selector.cpp
  src/detail/block_layout.cpp
  src/detail/diagnostic_state.cpp
  src/detail/memory_guard.cpp
  src/detail/statistics_tracker.cpp
)
target_include_directories(memory_pool
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(memory_pool PUBLIC cxx_std_20)
memory_pool_enable_warnings(memory_pool)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  if(MEMORY_POOL_ENABLE_SANITIZERS)
    target_compile_options(memory_pool PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(memory_pool PUBLIC -fsanitize=address,undefined)
  endif()
endif()

add_executable(memory_pool_example examples/basic_usage.cpp)
target_link_libraries(memory_pool_example PRIVATE memory_pool)
memory_pool_enable_warnings(memory_pool_example)

add_executable(object_pool_example examples/object_pool_usage.cpp)
target_link_libraries(object_pool_example PRIVATE memory_pool)
memory_pool_enable_warnings(object_pool_example)

add_executable(growing_pool_example examples/growing_pool_usage.cpp)
target_link_libraries(growing_pool_example PRIVATE memory_pool)
memory_pool_enable_warnings(growing_pool_example)

add_executable(segregated_allocator_example examples/segregated_allocator_usage.cpp)
target_link_libraries(segregated_allocator_example PRIVATE memory_pool)
memory_pool_enable_warnings(segregated_allocator_example)

add_executable(pmr_example examples/pmr_usage.cpp)
target_link_libraries(pmr_example PRIVATE memory_pool)
memory_pool_enable_warnings(pmr_example)

add_executable(memory_pool_benchmark benchmarks/allocation_benchmark.cpp)
target_link_libraries(memory_pool_benchmark PRIVATE memory_pool)
memory_pool_enable_warnings(memory_pool_benchmark)

include(CTest)
if(BUILD_TESTING)
  add_executable(memory_pool_tests
    tests/memory_pool_tests.cpp
    tests/fixed_size_memory_pool_tests.cpp
    tests/diagnostics_tests.cpp
    tests/object_pool_tests.cpp
    tests/phase3_tests.cpp
    tests/pmr_tests.cpp
    tests/segregated_allocator_tests.cpp
    tests/statistics_tests.cpp
  )
  target_link_libraries(memory_pool_tests PRIVATE memory_pool)
  memory_pool_enable_warnings(memory_pool_tests)
  add_test(NAME memory_pool_tests COMMAND memory_pool_tests)
endif()
