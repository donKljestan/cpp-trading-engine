cmake_minimum_required(VERSION 3.20)
project(llt LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(LLT_NATIVE_ARCH "Tune for the host CPU (-march=native)" ON)
option(LLT_BUILD_TESTS "Build unit tests" ON)
set(LLT_SANITIZER "" CACHE STRING "Sanitizer to enable: address, thread, undefined, or empty")

find_package(Threads REQUIRED)

# Shared warning and optimisation policy. Every target links this.
add_library(llt_flags INTERFACE)
target_compile_options(llt_flags INTERFACE
    -Wall -Wextra -Wpedantic -Werror
    -Wconversion -Wsign-conversion -Wshadow -Wold-style-cast
)

if(LLT_NATIVE_ARCH)
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag(-march=native LLT_HAS_MARCH_NATIVE)
    if(LLT_HAS_MARCH_NATIVE)
        target_compile_options(llt_flags INTERFACE -march=native)
    endif()
endif()

if(LLT_SANITIZER)
    target_compile_options(llt_flags INTERFACE
        -fsanitize=${LLT_SANITIZER} -fno-omit-frame-pointer -g)
    target_link_options(llt_flags INTERFACE -fsanitize=${LLT_SANITIZER})
endif()

add_library(llt_core STATIC
    src/core/clock.cpp
    src/core/cpu.cpp
)
target_include_directories(llt_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(llt_core PUBLIC llt_flags Threads::Threads)

add_executable(llt_bot src/main.cpp)
target_link_libraries(llt_bot PRIVATE llt_core)

# LTO pays off in the hot path but breaks sanitizer symbolisation, so keep them apart.
if(CMAKE_BUILD_TYPE STREQUAL "Release" AND NOT LLT_SANITIZER)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT LLT_IPO_OK OUTPUT LLT_IPO_MSG)
    if(LLT_IPO_OK)
        set_property(TARGET llt_core llt_bot PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
endif()

if(LLT_BUILD_TESTS)
    enable_testing()
    add_executable(llt_tests
        tests/test_main.cpp
        tests/test_fixed.cpp
        tests/test_spsc.cpp
        tests/test_clock.cpp
    )
    target_include_directories(llt_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_link_libraries(llt_tests PRIVATE llt_core)

    # TSan and ASan abort with "unexpected memory mapping" against the high ASLR
    # entropy of modern kernels (mmap_rnd_bits=32), so run them with ASLR disabled.
    find_program(LLT_SETARCH setarch)
    if(LLT_SANITIZER AND LLT_SETARCH)
        add_test(NAME unit COMMAND ${LLT_SETARCH} -R $<TARGET_FILE:llt_tests>)
    else()
        add_test(NAME unit COMMAND llt_tests)
    endif()
endif()