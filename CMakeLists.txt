cmake_minimum_required(VERSION 3.15)
project(SharedCacheService)
set(CMAKE_CXX_STANDARD 11)
include_directories(.)
include_directories(include)

set(CMAKE_CXX_FLAGS -pthread)
add_executable(test1 ./src/rdma_communication.cpp ./src/waitset.cpp ./test/myipc.cpp ./test/test_shared_memory.cpp)