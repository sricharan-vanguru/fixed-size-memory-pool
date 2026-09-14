cmake_minimum_required(VERSION 3.20)
project(memory_pool_package_consumer LANGUAGES CXX)

find_package(fixed_size_memory_pool 1.7 REQUIRED)

add_executable(memory_pool_package_consumer main.cpp)
target_link_libraries(memory_pool_package_consumer
  PRIVATE memory_pool::memory_pool
)
