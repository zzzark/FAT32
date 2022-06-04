cmake_minimum_required(VERSION 3.17)
project(csTask01 C)

set(CMAKE_C_STANDARD 99)

add_executable(csTask01 main.c)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -pthread")