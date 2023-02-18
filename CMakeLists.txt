project(SharedCacheService)
set(CMAKE_CXX_STANDARD 11)
include_directories(.)
include_directories(include)

set(CMAKE_CXX_FLAGS "-pthread -libverbs -g -O0")
set(DEBUG false CACHE BOOL "debug")

if (DEBUG) 
    add_definitions(-DDEBUG)
endif()

# add_executable(test1 ./src/rdma_communication.cpp ./src/waitset.cpp ./test/myipc.cpp ./test/test_shared_memory.cpp)
add_executable(test2 ./test/test_simple_server.cpp ./src/rdma_communication.cpp ./src/waitset.cpp  ./test/test_worker_threadpool.cpp ./test/myipc.cpp)