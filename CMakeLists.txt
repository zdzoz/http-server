cmake_minimum_required(VERSION 3.13)
project(http-server)

set(CMAKE_CXX_STANDARD_REQUIRED true)
set(CMAKE_CXX_STANDARD 20)

find_package(Threads REQUIRED)

add_executable(server src/server.cpp)
target_link_libraries(server PRIVATE Threads::Threads)
