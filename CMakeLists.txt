cmake_minimum_required(VERSION 3.20)
project(netbench VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(NETBENCH_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)
find_package(Threads REQUIRED)

add_library(netbench src/common.cpp src/metrics.cpp src/protocol.cpp)
target_include_directories(netbench PUBLIC include)
target_link_libraries(netbench PUBLIC Threads::Threads)
target_compile_options(netbench PUBLIC -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
if(NETBENCH_SANITIZERS)
    target_compile_options(netbench PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(netbench PUBLIC -fsanitize=address,undefined)
endif()

add_executable(netbench_client src/client.cpp)
target_link_libraries(netbench_client PRIVATE netbench)

add_executable(netbench_server src/server.cpp)
target_link_libraries(netbench_server PRIVATE netbench)
