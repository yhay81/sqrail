cmake_minimum_required(VERSION 3.22)

project(
  sqrail
  VERSION 0.1.0
  DESCRIPTION "A minimal SQL-on-files CLI for coding agents"
  LANGUAGES CXX
)

include(FetchContent)

option(SQRAIL_BUILD_TESTS "Enable sqrail smoke tests" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# sqrail deliberately ships one engine. DuckDB is pinned so the SQL behavior of
# a release is reproducible.
set(DUCKDB_VERSION "v1.5.5" CACHE STRING "Pinned DuckDB release")
set(BUILD_UNITTESTS OFF CACHE BOOL "" FORCE)
set(BUILD_SHELL OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(BUILD_EXTENSIONS "parquet;json" CACHE STRING "" FORCE)

FetchContent_Declare(
  duckdb
  GIT_REPOSITORY "https://github.com/duckdb/duckdb.git"
  GIT_TAG "${DUCKDB_VERSION}"
  GIT_SHALLOW TRUE
  GIT_PROGRESS TRUE
)
FetchContent_MakeAvailable(duckdb)

add_executable(sqrail src/main.cpp)
target_include_directories(sqrail SYSTEM PRIVATE "${duckdb_SOURCE_DIR}/src/include")
target_link_libraries(
  sqrail
  PRIVATE
    duckdb_generated_extension_loader
    core_functions_extension
    parquet_extension
    json_extension
)
target_compile_definitions(
  sqrail
  PRIVATE
    SQRAIL_VERSION="${PROJECT_VERSION}"
    SQRAIL_DUCKDB_VERSION="${DUCKDB_VERSION}"
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(sqrail PRIVATE -Wall -Wextra -Wpedantic)
  if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(sqrail PRIVATE -ffunction-sections -fdata-sections)
    if(APPLE)
      target_link_options(sqrail PRIVATE -Wl,-dead_strip)
    else()
      target_link_options(sqrail PRIVATE -Wl,--gc-sections)
    endif()
  endif()
endif()

install(TARGETS sqrail RUNTIME DESTINATION bin)

if(SQRAIL_BUILD_TESTS)
  enable_testing()
  add_test(
    NAME sqrail-smoke
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/smoke.sh" "$<TARGET_FILE:sqrail>"
  )
endif()
