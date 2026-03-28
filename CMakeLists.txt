cmake_minimum_required(VERSION 3.14)

# on Ubuntu 24.04 clang++ (18.1.3) produces much faster executables than g++ (13.3.0)
set(CMAKE_CXX_COMPILER_NAMES clang++ g++ icpc c++ cxx)
project(eccpp VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Enable LTO via CMake when supported
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)

find_package(Threads REQUIRED)

add_executable(rtsp_server
    src/main.cpp
    src/rtsp_server.cpp
)

target_include_directories(rtsp_server PRIVATE src)
target_link_libraries(rtsp_server PRIVATE Threads::Threads)
