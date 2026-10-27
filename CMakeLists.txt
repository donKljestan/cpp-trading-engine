cmake_minimum_required(VERSION 3.20)
project(minisdk LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Werror)

add_executable(minisdk_demo 
    src/main.cpp
)

target_include_directories(minisdk_demo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

find_package(Threads REQUIRED)
target_link_libraries(minisdk_demo PRIVATE Threads::Threads)