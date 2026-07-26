cmake_minimum_required(VERSION 3.22)

project(
  sqrail
  VERSION 0.1.0
  DESCRIPTION "A minimal SQL-on-files CLI for coding agents"
  LANGUAGES CXX
)

include(FetchContent)
include(GNUInstallDirs)

option(SQRAIL_BUILD_TESTS "Enable sqrail smoke tests" ON)
option(SQRAIL_BUILD_DUCKDB_SHELL "Build the pinned DuckDB CLI for benchmarks" OFF)
option(SQRAIL_WARNINGS_AS_ERRORS "Treat sqrail compiler warnings as errors" OFF)
option(SQRAIL_STATIC_CPP_RUNTIME "Statically link the C++ runtime on Linux" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# sqrail deliberately ships one engine. DuckDB is pinned so the SQL behavior of
# a release is reproducible. The human-readable version is kept separately from
# the immutable source revision.
set(DUCKDB_VERSION "v1.5.5" CACHE STRING "Pinned DuckDB release")
set(
  DUCKDB_REVISION
  "d8cdaa33fda8df955cc76ef58a280f68f4cd43fa"
  CACHE STRING
  "Pinned DuckDB git revision"
)
set(BUILD_UNITTESTS OFF CACHE BOOL "" FORCE)
set(BUILD_SHELL "${SQRAIL_BUILD_DUCKDB_SHELL}" CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(BUILD_EXTENSIONS "parquet;json" CACHE STRING "" FORCE)

FetchContent_Declare(
  duckdb
  GIT_REPOSITORY "https://github.com/duckdb/duckdb.git"
  GIT_TAG "${DUCKDB_REVISION}"
  GIT_SHALLOW TRUE
  GIT_PROGRESS TRUE
)
FetchContent_MakeAvailable(duckdb)

add_executable(sqrail src/main.cpp)
target_include_directories(sqrail SYSTEM PRIVATE "${duckdb_SOURCE_DIR}/src/include")
if(APPLE)
  target_link_libraries(
    sqrail
    PRIVATE
      duckdb_generated_extension_loader
      core_functions_extension
      parquet_extension
      json_extension
  )
else()
  # GNU linkers resolve static archives from left to right. Put DuckDB before
  # the generated extension loader; the extension targets add it again
  # transitively to resolve symbols introduced by the extensions.
  target_link_libraries(
    sqrail
    PRIVATE
      duckdb_static
      duckdb_generated_extension_loader
      core_functions_extension
      parquet_extension
      json_extension
  )
endif()
target_compile_definitions(
  sqrail
  PRIVATE
    SQRAIL_VERSION="${PROJECT_VERSION}"
    SQRAIL_DUCKDB_VERSION="${DUCKDB_VERSION}"
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(sqrail PRIVATE -Wall -Wextra -Wpedantic)
  if(SQRAIL_WARNINGS_AS_ERRORS)
    target_compile_options(sqrail PRIVATE -Werror)
  endif()
  if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(sqrail PRIVATE -ffunction-sections -fdata-sections)
    if(APPLE)
      target_link_options(sqrail PRIVATE -Wl,-dead_strip)
    else()
      target_link_options(sqrail PRIVATE -Wl,--gc-sections)
    endif()
  endif()
endif()

if(
  SQRAIL_STATIC_CPP_RUNTIME
  AND UNIX
  AND NOT APPLE
  AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU"
)
  target_link_options(sqrail PRIVATE -static-libgcc -static-libstdc++)
endif()

install(
  TARGETS sqrail
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  COMPONENT sqrail
)
install(
  FILES README.md LICENSE THIRD_PARTY_NOTICES.md CHANGELOG.md SECURITY.md
  DESTINATION "${CMAKE_INSTALL_DOCDIR}"
  COMPONENT sqrail
)
install(
  FILES
    docs/CONCEPT.md
    docs/CONCEPT.ja.md
    docs/CONTRACT.md
    docs/BENCHMARKS.md
    docs/BASELINE.md
  DESTINATION "${CMAKE_INSTALL_DOCDIR}/docs"
  COMPONENT sqrail
)
install(
  FILES benchmarks/README.md
  DESTINATION "${CMAKE_INSTALL_DOCDIR}/benchmarks"
  COMPONENT sqrail
)
install(
  DIRECTORY "${duckdb_SOURCE_DIR}/third_party/"
  DESTINATION "${CMAKE_INSTALL_DOCDIR}/licenses/duckdb-third-party"
  COMPONENT sqrail
  FILES_MATCHING
    PATTERN "LICENSE*"
    PATTERN "COPYING*"
    PATTERN "NOTICE*"
)

if(SQRAIL_BUILD_TESTS)
  enable_testing()
  add_test(
    NAME sqrail-smoke
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/smoke.sh" "$<TARGET_FILE:sqrail>"
  )
endif()
