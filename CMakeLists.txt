cmake_minimum_required(VERSION 3.20)
project(netbench VERSION 1.0.0 LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "netbench requires Linux socket and clock APIs")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(NETBENCH_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)

find_package(Threads REQUIRED)

add_library(netbench
    src/common.cpp
    src/protocol.cpp
    src/metrics.cpp
)
target_include_directories(netbench PUBLIC include)
target_link_libraries(netbench PUBLIC Threads::Threads)
target_compile_options(netbench PUBLIC -Wall -Wextra -Wpedantic -Wconversion -Wshadow)

if(NETBENCH_SANITIZERS)
    target_compile_options(netbench PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(netbench PUBLIC -fsanitize=address,undefined)
endif()

add_executable(netbench_server src/server.cpp)
target_link_libraries(netbench_server PRIVATE netbench)

add_executable(netbench_client src/client.cpp)
target_link_libraries(netbench_client PRIVATE netbench)

include(CTest)
if(BUILD_TESTING)
    find_package(GTest REQUIRED)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)

    add_executable(unit_tests tests/unit.cpp)
    target_link_libraries(unit_tests PRIVATE netbench GTest::gtest_main)

    add_test(NAME unit COMMAND unit_tests)
    add_test(NAME integration
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/integration.py
                $<TARGET_FILE_DIR:netbench_client>
    )
    add_test(NAME automation
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/automation.py
    )
    set_tests_properties(unit integration automation PROPERTIES TIMEOUT 60)
endif()
