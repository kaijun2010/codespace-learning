# MyExecutable 的目標檔案
cmake_minimum_required(VERSION 3.10)
project(SnakeGame VERSION 1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

find_package(Curses REQUIRED)
include_directories(${CURSES_INCLUDE_DIRS} include)

add_executable(snake
    src/main.cpp
    src/snake.cpp)

target_link_libraries(snake ${CURSES_LIBRARIES})
