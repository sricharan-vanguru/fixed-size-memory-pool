cmake_minimum_required(VERSION 3.20)
project(fixed_size_memory_pool VERSION 1.0.0 LANGUAGES CXX)

option(MEMORY_POOL_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

add_library(memory_pool INTERFACE)
target_include_directories(memory_pool INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(memory_pool INTERFACE cxx_std_20)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(memory_pool INTERFACE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
  if(MEMORY_POOL_ENABLE_SANITIZERS)
    target_compile_options(memory_pool INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(memory_pool INTERFACE -fsanitize=address,undefined)
  endif()
endif()

add_executable(memory_pool_example examples/basic_usage.cpp)
target_link_libraries(memory_pool_example PRIVATE memory_pool)

add_executable(memory_pool_tests tests/memory_pool_tests.cpp)
target_link_libraries(memory_pool_tests PRIVATE memory_pool)

add_executable(memory_pool_benchmark benchmarks/allocation_benchmark.cpp)
target_link_libraries(memory_pool_benchmark PRIVATE memory_pool)

include(CTest)
if(BUILD_TESTING)
  add_test(NAME memory_pool_tests COMMAND memory_pool_tests)
endif()

