cmake_minimum_required(VERSION 3.20)
project(LoomStore VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=native -mtune=native -O3 -flto -fno-exceptions -fno-rtti -pthread")

# Architecture-specific optimizations
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mavx2 -mbmi2 -mlzcnt -mpopcnt")
endif()

find_package(Threads REQUIRED)
find_package(protobuf REQUIRED)
find_package(uring REQUIRED)

add_executable(loomstore
    src/main.cpp
    src/io_uring_engine.cpp
    src/lockfree_map.cpp
    src/raft_node.cpp
    src/slab_allocator.cpp
)

target_include_directories(loomstore PRIVATE include)
target_link_libraries(loomstore
    Threads::Threads
    protobuf::libprotobuf
    uring
    rt
    atomic
)

# Enable link-time optimization
set_property(TARGET loomstore PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
