cmake_minimum_required(VERSION 3.16)
project(OrderMatchingEngine VERSION 1.0.0 LANGUAGES CXX)

# ─────────────────────────────────────────────────────────────────────────────
# C++ Standard
# ─────────────────────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ─────────────────────────────────────────────────────────────────────────────
# Compiler Flags
# ─────────────────────────────────────────────────────────────────────────────
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    # Strict warnings (added per-target, not globally for dependencies)
    set(PROJECT_WARNING_FLAGS
        -Wall
        -Wextra
        -Wpedantic
        -Werror
        -Wconversion
        -Wshadow
        -Wno-unused-parameter
    )
    
    # Release optimizations
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -march=native -flto")
    
    # Debug flags
    set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g -fsanitize=address,undefined")
    set(CMAKE_EXE_LINKER_FLAGS_DEBUG "-fsanitize=address,undefined")
endif()

# ─────────────────────────────────────────────────────────────────────────────
# Include Directories
# ─────────────────────────────────────────────────────────────────────────────
include_directories(${CMAKE_SOURCE_DIR}/include)

# ─────────────────────────────────────────────────────────────────────────────
# Core Library
# ─────────────────────────────────────────────────────────────────────────────
set(LIB_SOURCES
    src/order_pool.cpp
    src/price_level.cpp
)

add_library(order_matching_engine STATIC ${LIB_SOURCES})
target_include_directories(order_matching_engine PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_compile_options(order_matching_engine PRIVATE ${PROJECT_WARNING_FLAGS})

# ─────────────────────────────────────────────────────────────────────────────
# Main Executable (disabled until order_book is implemented)
# ─────────────────────────────────────────────────────────────────────────────
# add_executable(order_matching src/main.cpp)
# target_link_libraries(order_matching PRIVATE order_matching_engine)

# ─────────────────────────────────────────────────────────────────────────────
# Testing (Google Test)
# ─────────────────────────────────────────────────────────────────────────────
option(BUILD_TESTS "Build unit tests" ON)

if(BUILD_TESTS)
    include(FetchContent)
    
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.14.0
    )
    FetchContent_MakeAvailable(googletest)
    
    enable_testing()
    
    # OrderPool tests
    add_executable(order_pool_test tests/order_pool_test.cpp)
    target_link_libraries(order_pool_test
        PRIVATE
            order_matching_engine
            GTest::gtest_main
    )
    
    # PriceLevel tests
    add_executable(price_level_test tests/price_level_test.cpp)
    target_link_libraries(price_level_test
        PRIVATE
            order_matching_engine
            GTest::gtest_main
    )
    
    # OrderBook matching tests
    add_executable(order_book_matching_test tests/order_book_matching_tests.cpp)
    target_link_libraries(order_book_matching_test
        PRIVATE
            order_matching_engine
            GTest::gtest_main
    )
    
    # Self-Match Prevention tests
    add_executable(order_book_smp_test tests/order_book_smp_tests.cpp)
    target_link_libraries(order_book_smp_test
        PRIVATE
            order_matching_engine
            GTest::gtest_main
    )
    
    # Cancel tests
    add_executable(order_book_cancel_test tests/order_book_cancel_test.cpp)
    target_link_libraries(order_book_cancel_test
        PRIVATE
            order_matching_engine
            GTest::gtest_main
    )
    
    # Allocation tracking tests
    add_executable(allocation_test tests/allocation_test.cpp)
    target_link_libraries(allocation_test
        PRIVATE
            order_matching_engine
            GTest::gtest_main
    )
    
    include(GoogleTest)
    gtest_discover_tests(order_pool_test)
    gtest_discover_tests(price_level_test)
    gtest_discover_tests(order_book_matching_test)
    gtest_discover_tests(order_book_smp_test)
    gtest_discover_tests(order_book_cancel_test)
    gtest_discover_tests(allocation_test)
    
    # TODO: uncomment when implemented
    # add_executable(order_book_test tests/order_book_test.cpp)
    # target_link_libraries(order_book_test PRIVATE order_matching_engine GTest::gtest_main)
    # gtest_discover_tests(order_book_test)
    
    # add_executable(invariants_test tests/invariants_test.cpp)
    # target_link_libraries(invariants_test PRIVATE order_matching_engine GTest::gtest_main)
    # gtest_discover_tests(invariants_test)
endif()

# ─────────────────────────────────────────────────────────────────────────────
# Benchmarking (Google Benchmark)
# ─────────────────────────────────────────────────────────────────────────────
option(BUILD_BENCHMARKS "Build benchmarks" OFF)

if(BUILD_BENCHMARKS)
    include(FetchContent)
    
    FetchContent_Declare(
        benchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG v1.8.3
    )
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(benchmark)
    
    # Google Benchmark suite
    add_executable(order_book_bench benchmarks/order_book_bench.cpp)
    target_link_libraries(order_book_bench
        PRIVATE
            order_matching_engine
            benchmark::benchmark
            benchmark::benchmark_main
    )
    target_compile_options(order_book_bench PRIVATE -O3 -DNDEBUG -march=native)
    target_compile_definitions(order_book_bench PRIVATE NDEBUG)
    
    # Latency percentile tool (standalone, no Google Benchmark dependency)
    add_executable(latency_percentiles benchmarks/latency_percentiles.cpp)
    target_link_libraries(latency_percentiles
        PRIVATE
            order_matching_engine
    )
    target_compile_options(latency_percentiles PRIVATE -O3 -DNDEBUG -march=native)
    target_compile_definitions(latency_percentiles PRIVATE NDEBUG)
endif()

# ─────────────────────────────────────────────────────────────────────────────
# Summary
# ─────────────────────────────────────────────────────────────────────────────
message(STATUS "")
message(STATUS "Order Matching Engine - Build Config")
message(STATUS "  Build Type:    ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ Standard:  C++${CMAKE_CXX_STANDARD}")
message(STATUS "  Compiler:      ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Tests:         ${BUILD_TESTS}")
message(STATUS "  Benchmarks:    ${BUILD_BENCHMARKS}")
message(STATUS "")
