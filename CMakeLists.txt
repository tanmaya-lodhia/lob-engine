cmake_minimum_required(VERSION 3.24)
project(lob_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

# Address/UB sanitizers. Fully supported on a Linux/Clang build; on MinGW GCC
# they may not link, so this is opt-in and off by default. The CMake project is
# intentionally portable so the sanitized build can run on Linux/CI.
option(LOB_SANITIZE "Build with Address+UB sanitizers" OFF)
if(LOB_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# --- core library ------------------------------------------------------------
add_library(lob STATIC src/order_book.cpp)
target_include_directories(lob PUBLIC include)

# --- replay driver -----------------------------------------------------------
add_executable(replay apps/replay.cpp)
target_link_libraries(replay PRIVATE lob)

# --- oracle validator --------------------------------------------------------
add_executable(oracle apps/oracle.cpp)
target_link_libraries(oracle PRIVATE lob)

# --- tests -------------------------------------------------------------------
# doctest is vendored as a single header (third_party/) so the build is fully
# reproducible and needs no network access at configure time.
add_library(doctest INTERFACE)
target_include_directories(doctest INTERFACE third_party/doctest)

enable_testing()
add_executable(unit_tests tests/test_order_book.cpp)
target_link_libraries(unit_tests PRIVATE lob doctest)
add_test(NAME unit_tests COMMAND unit_tests)

# Oracle regression: a self-contained feed (no sub-depth activity) must
# reconstruct exactly at every snapshot. Exit code is 0 only on a full match.
add_test(
  NAME oracle_fixture
  COMMAND oracle
          ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/oracle_message.csv
          ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/oracle_orderbook.csv
          2
)
