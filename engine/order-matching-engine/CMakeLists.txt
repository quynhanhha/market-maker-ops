cmake_minimum_required(VERSION 3.16)
project(OrderMatchingEngine VERSION 1.0.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    set(PROJECT_WARNING_FLAGS
        -Wall
        -Wextra
        -Wpedantic
        -Werror
        -Wconversion
        -Wshadow
        -Wno-unused-parameter
    )
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -march=native -flto")
    set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g -fsanitize=address,undefined")
    set(CMAKE_EXE_LINKER_FLAGS_DEBUG "-fsanitize=address,undefined")
endif()
include_directories(${CMAKE_SOURCE_DIR}/include)
set(LIB_SOURCES
    src/order_pool.cpp
    src/price_level.cpp
)

add_library(order_matching_engine STATIC ${LIB_SOURCES})
target_include_directories(order_matching_engine PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_compile_options(order_matching_engine PRIVATE ${PROJECT_WARNING_FLAGS})
add_library(venue_core STATIC
    venue/protocol.cpp
    venue/framing.cpp
    venue/venue.cpp
)
target_link_libraries(venue_core PUBLIC order_matching_engine)
target_include_directories(venue_core PUBLIC ${CMAKE_SOURCE_DIR}/venue)
target_compile_options(venue_core PRIVATE ${PROJECT_WARNING_FLAGS})

add_executable(venue_server
    venue/main.cpp
    venue/server.cpp
)
target_link_libraries(venue_server PRIVATE venue_core)
target_compile_options(venue_server PRIVATE ${PROJECT_WARNING_FLAGS})
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
    add_executable(order_pool_test tests/order_pool_test.cpp)
    target_link_libraries(order_pool_test
        PRIVATE
            order_matching_engine
            GTest::gtest_main
    )
    add_executable(price_level_test tests/price_level_test.cpp)
    target_link_libraries(price_level_test
        PRIVATE
            order_matching_engine
            GTest::gtest_main
    )
    add_executable(order_book_matching_test tests/order_book_matching_tests.cpp)
    target_link_libraries(order_book_matching_test
        PRIVATE
            order_matching_engine
            GTest::gtest_main
    )
    add_executable(order_book_smp_test tests/order_book_smp_tests.cpp)
    target_link_libraries(order_book_smp_test
        PRIVATE
            order_matching_engine
            GTest::gtest_main
    )
    add_executable(order_book_cancel_test tests/order_book_cancel_test.cpp)
    target_link_libraries(order_book_cancel_test
        PRIVATE
            order_matching_engine
            GTest::gtest_main
    )
    add_executable(allocation_test tests/allocation_test.cpp)
    target_link_libraries(allocation_test
        PRIVATE
            order_matching_engine
            GTest::gtest_main
    )
    add_executable(venue_protocol_test tests/venue_protocol_test.cpp)
    target_link_libraries(venue_protocol_test
        PRIVATE
            venue_core
            GTest::gtest_main
    )

    include(GoogleTest)
    gtest_discover_tests(order_pool_test)
    gtest_discover_tests(price_level_test)
    gtest_discover_tests(order_book_matching_test)
    gtest_discover_tests(order_book_smp_test)
    gtest_discover_tests(order_book_cancel_test)
    gtest_discover_tests(allocation_test)
    gtest_discover_tests(venue_protocol_test)
endif()
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
    add_executable(order_book_bench benchmarks/order_book_bench.cpp)
    target_link_libraries(order_book_bench
        PRIVATE
            order_matching_engine
            benchmark::benchmark
            benchmark::benchmark_main
    )
    target_compile_options(order_book_bench PRIVATE -O3 -DNDEBUG -march=native)
    target_compile_definitions(order_book_bench PRIVATE NDEBUG)
    add_executable(latency_percentiles benchmarks/latency_percentiles.cpp)
    target_link_libraries(latency_percentiles
        PRIVATE
            order_matching_engine
    )
    target_compile_options(latency_percentiles PRIVATE -O3 -DNDEBUG -march=native)
    target_compile_definitions(latency_percentiles PRIVATE NDEBUG)
endif()
message(STATUS "")
message(STATUS "Order Matching Engine - Build Config")
message(STATUS "  Build Type:    ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ Standard:  C++${CMAKE_CXX_STANDARD}")
message(STATUS "  Compiler:      ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Tests:         ${BUILD_TESTS}")
message(STATUS "  Benchmarks:    ${BUILD_BENCHMARKS}")
message(STATUS "")
