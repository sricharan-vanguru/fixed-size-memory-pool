cmake_minimum_required(VERSION 3.20)
project(fixed_size_memory_pool VERSION 1.7.0 LANGUAGES CXX)

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

find_package(Threads REQUIRED)

set(MEMORY_POOL_BUILD_TOOLS_DEFAULT OFF)
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
  set(MEMORY_POOL_BUILD_TOOLS_DEFAULT ON)
endif()

option(MEMORY_POOL_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(MEMORY_POOL_ENABLE_THREAD_SANITIZER "Enable ThreadSanitizer" OFF)
option(MEMORY_POOL_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(MEMORY_POOL_ENABLE_CLANG_TIDY "Run clang-tidy while compiling" OFF)
option(MEMORY_POOL_BUILD_EXAMPLES "Build usage examples" ${MEMORY_POOL_BUILD_TOOLS_DEFAULT})
option(MEMORY_POOL_BUILD_BENCHMARKS "Build microbenchmarks" ${MEMORY_POOL_BUILD_TOOLS_DEFAULT})

if(MEMORY_POOL_ENABLE_SANITIZERS AND MEMORY_POOL_ENABLE_THREAD_SANITIZER)
  message(FATAL_ERROR "Address/Undefined sanitizers and ThreadSanitizer cannot be enabled together")
endif()

function(memory_pool_enable_warnings target)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    if(MEMORY_POOL_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  elseif(MSVC)
    target_compile_options(${target} PRIVATE /W4)
    if(MEMORY_POOL_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  endif()
endfunction()

add_library(memory_pool
  src/chunk_manager.cpp
  src/fixed_size_memory_pool.cpp
  src/memory_chunk.cpp
  src/monotonic_arena.cpp
  src/new_delete_memory_provider.cpp
  src/pool_memory_resource.cpp
  src/segregated_allocator.cpp
  src/size_class_selector.cpp
  src/thread_cached_allocator.cpp
  src/detail/block_layout.cpp
  src/detail/arena_chunk.cpp
  src/detail/diagnostic_state.cpp
  src/detail/memory_guard.cpp
  src/detail/statistics_tracker.cpp
  src/detail/thread_cache_state.cpp
)
target_include_directories(memory_pool
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(memory_pool PUBLIC Threads::Threads)
target_compile_features(memory_pool PUBLIC cxx_std_20)
memory_pool_enable_warnings(memory_pool)
add_library(memory_pool::memory_pool ALIAS memory_pool)

if(MEMORY_POOL_ENABLE_CLANG_TIDY)
  find_program(MEMORY_POOL_CLANG_TIDY_EXECUTABLE NAMES clang-tidy REQUIRED)
  set_target_properties(memory_pool PROPERTIES
    CXX_CLANG_TIDY "${MEMORY_POOL_CLANG_TIDY_EXECUTABLE}"
  )
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  if(MEMORY_POOL_ENABLE_SANITIZERS)
    target_compile_options(memory_pool PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(memory_pool PUBLIC -fsanitize=address,undefined)
  endif()
  if(MEMORY_POOL_ENABLE_THREAD_SANITIZER)
    target_compile_options(memory_pool PUBLIC -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(memory_pool PUBLIC -fsanitize=thread)
  endif()
endif()

function(memory_pool_add_tool target source)
  add_executable(${target} ${source})
  target_link_libraries(${target} PRIVATE memory_pool::memory_pool)
  memory_pool_enable_warnings(${target})
endfunction()

if(MEMORY_POOL_BUILD_EXAMPLES)
  memory_pool_add_tool(memory_pool_example examples/basic_usage.cpp)
  memory_pool_add_tool(object_pool_example examples/object_pool_usage.cpp)
  memory_pool_add_tool(growing_pool_example examples/growing_pool_usage.cpp)
  memory_pool_add_tool(segregated_allocator_example examples/segregated_allocator_usage.cpp)
  memory_pool_add_tool(pmr_example examples/pmr_usage.cpp)
  memory_pool_add_tool(concurrency_example examples/concurrency_usage.cpp)
  memory_pool_add_tool(arena_example examples/arena_usage.cpp)
endif()

if(MEMORY_POOL_BUILD_BENCHMARKS)
  memory_pool_add_tool(memory_pool_benchmark benchmarks/allocation_benchmark.cpp)
  memory_pool_add_tool(concurrency_benchmark benchmarks/concurrency_benchmark.cpp)
  memory_pool_add_tool(arena_benchmark benchmarks/arena_benchmark.cpp)
endif()

include(CTest)
if(BUILD_TESTING)
  add_executable(memory_pool_tests
    tests/memory_pool_tests.cpp
    tests/arena_tests.cpp
    tests/concurrency_tests.cpp
    tests/concurrency_edge_tests.cpp
    tests/fixed_size_memory_pool_tests.cpp
    tests/diagnostics_tests.cpp
    tests/object_pool_tests.cpp
    tests/phase3_tests.cpp
    tests/pmr_tests.cpp
    tests/segregated_allocator_tests.cpp
    tests/statistics_tests.cpp
  )
  target_link_libraries(memory_pool_tests PRIVATE memory_pool::memory_pool)
  memory_pool_enable_warnings(memory_pool_tests)
  add_test(NAME memory_pool_tests COMMAND memory_pool_tests)
endif()

configure_package_config_file(
  cmake/fixed_size_memory_poolConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/fixed_size_memory_poolConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/fixed_size_memory_pool"
)
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/fixed_size_memory_poolConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}"
  COMPATIBILITY SameMajorVersion
)

install(TARGETS memory_pool
  EXPORT fixed_size_memory_poolTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)
install(DIRECTORY include/
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
  FILES_MATCHING PATTERN "*.hpp"
)
install(EXPORT fixed_size_memory_poolTargets
  FILE fixed_size_memory_poolTargets.cmake
  NAMESPACE memory_pool::
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/fixed_size_memory_pool"
)
install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/fixed_size_memory_poolConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/fixed_size_memory_poolConfigVersion.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/fixed_size_memory_pool"
)
install(FILES LICENSE README.md CHANGELOG.md
  DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/fixed_size_memory_pool"
)
install(DIRECTORY docs/
  DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/fixed_size_memory_pool"
  FILES_MATCHING PATTERN "*.md"
)
