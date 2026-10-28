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
find_package(OpenSSL REQUIRED)

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
    src/exec/signer.cpp
    src/md/record.cpp
    src/net/tcp.cpp
    src/net/tls.cpp
    src/net/http.cpp
    src/net/ws.cpp
    src/venue/binance/bn_env.cpp
    src/venue/binance/bn_rest.cpp
)
target_include_directories(llt_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(llt_core PUBLIC llt_flags Threads::Threads OpenSSL::SSL OpenSSL::Crypto)

add_executable(llt_bot src/main.cpp)
target_link_libraries(llt_bot PRIVATE llt_core)

add_executable(md_dump src/tools/md_dump.cpp)
target_link_libraries(md_dump PRIVATE llt_core)

add_executable(lat_probe src/tools/lat_probe.cpp)
target_link_libraries(lat_probe PRIVATE llt_core)

add_executable(sym_scan src/tools/sym_scan.cpp)
target_link_libraries(sym_scan PRIVATE llt_core)

add_executable(md_record src/tools/md_record.cpp)
target_link_libraries(md_record PRIVATE llt_core)

add_executable(md_replay src/tools/md_replay.cpp)
target_link_libraries(md_replay PRIVATE llt_core)

add_executable(backtest src/tools/backtest.cpp)
target_link_libraries(backtest PRIVATE llt_core)

add_executable(keygen src/tools/keygen.cpp)
target_link_libraries(keygen PRIVATE llt_core)

# LTO pays off in the hot path but breaks sanitizer symbolisation, so keep them apart.
if(CMAKE_BUILD_TYPE STREQUAL "Release" AND NOT LLT_SANITIZER)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT LLT_IPO_OK OUTPUT LLT_IPO_MSG)
    if(LLT_IPO_OK)
        set_property(TARGET llt_core llt_bot md_dump lat_probe sym_scan md_record md_replay
                     backtest keygen
                     PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
endif()

if(LLT_BUILD_TESTS)
    enable_testing()
    add_executable(llt_tests
        tests/test_main.cpp
        tests/test_fixed.cpp
        tests/test_spsc.cpp
        tests/test_clock.cpp
        tests/test_json_scan.cpp
        tests/test_bn_parse.cpp
        tests/test_bn_rest.cpp
        tests/test_record.cpp
        tests/test_signals.cpp
        tests/test_quoter.cpp
        tests/test_signer.cpp
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