cmake_minimum_required(VERSION 3.25)
cmake_policy(SET CMP0091 NEW)

project(
  sqrail
  VERSION 0.3.0
  DESCRIPTION "A minimal SQL-on-files CLI for coding agents"
  LANGUAGES CXX
)

include(GNUInstallDirs)
include(CheckCXXCompilerFlag)
include(CheckIPOSupported)

option(SQRAIL_BUILD_TESTS "Enable sqrail smoke tests" ON)
option(SQRAIL_BUILD_FUZZERS "Build coverage-guided fuzz targets" OFF)
option(SQRAIL_BUILD_DUCKDB_SHELL "Build the pinned DuckDB CLI for benchmarks" OFF)
option(SQRAIL_WARNINGS_AS_ERRORS "Treat sqrail compiler warnings as errors" OFF)
option(SQRAIL_STATIC_CPP_RUNTIME "Statically link the C++ runtime on Linux" ON)
option(SQRAIL_ENABLE_CLANG_TIDY "Run clang-tidy while compiling sqrail sources" OFF)
option(SQRAIL_ENABLE_HARDENING "Enable production compiler and linker hardening" ON)
option(SQRAIL_ENABLE_IPO "Enable whole-program interprocedural optimization" OFF)
option(SQRAIL_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(SQRAIL_ENABLE_THREAD_SANITIZER "Enable ThreadSanitizer" OFF)
option(SQRAIL_NATIVE_ARCH "Optimize for the build host CPU; never use for portable releases" OFF)
set(
  SQRAIL_DUCKDB_PROVIDER
  "BUNDLED"
  CACHE STRING
  "DuckDB provider: BUNDLED fetches the pinned source; SYSTEM uses an installed DuckDB package"
)
set_property(CACHE SQRAIL_DUCKDB_PROVIDER PROPERTY STRINGS BUNDLED SYSTEM)
set(SQRAIL_PGO_MODE "OFF" CACHE STRING "Clang PGO mode: OFF, GENERATE, or USE")
set_property(CACHE SQRAIL_PGO_MODE PROPERTY STRINGS OFF GENERATE USE)
set(
  SQRAIL_PGO_PROFILE
  "${CMAKE_BINARY_DIR}/pgo/sqrail.profdata"
  CACHE FILEPATH
  "Merged Clang instrumentation profile used by SQRAIL_PGO_MODE=USE"
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "Generate the clang tooling compilation database" FORCE)
if(MSVC)
  set(
    CMAKE_MSVC_RUNTIME_LIBRARY
    "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    CACHE STRING
    "Use the self-contained static MSVC runtime"
    FORCE
  )
  # MSVC otherwise reports __cplusplus as C++98 even in /std:c++20 mode.
  # DuckDB uses that macro to select std::uncaught_exceptions(), which is the
  # only available overload in the current Windows Arm64 standard library.
  add_compile_options(/utf-8 /Zc:__cplusplus)
endif()

string(TOUPPER "${SQRAIL_PGO_MODE}" SQRAIL_PGO_MODE)
if(NOT SQRAIL_PGO_MODE MATCHES "^(OFF|GENERATE|USE)$")
  message(FATAL_ERROR "SQRAIL_PGO_MODE must be OFF, GENERATE, or USE")
endif()
if(NOT SQRAIL_PGO_MODE STREQUAL "OFF")
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "sqrail instrumentation PGO currently requires Clang")
  endif()
  if(SQRAIL_ENABLE_SANITIZERS OR SQRAIL_ENABLE_THREAD_SANITIZER)
    message(FATAL_ERROR "PGO and sanitizers must use separate build trees")
  endif()
  if(SQRAIL_PGO_MODE STREQUAL "GENERATE")
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/pgo/raw")
    set(SQRAIL_PGO_FLAGS "-fprofile-instr-generate=${CMAKE_BINARY_DIR}/pgo/raw/sqrail-%m.profraw")
  else()
    if(NOT EXISTS "${SQRAIL_PGO_PROFILE}")
      message(FATAL_ERROR "PGO profile does not exist: ${SQRAIL_PGO_PROFILE}")
    endif()
    set(
      SQRAIL_PGO_FLAGS
      "-fprofile-instr-use=${SQRAIL_PGO_PROFILE}"
      "-Werror=profile-instr-out-of-date"
    )
  endif()
  add_compile_options(${SQRAIL_PGO_FLAGS})
  add_link_options(${SQRAIL_PGO_FLAGS})
endif()

if(SQRAIL_ENABLE_IPO)
  check_ipo_supported(RESULT SQRAIL_IPO_SUPPORTED OUTPUT SQRAIL_IPO_ERROR LANGUAGES CXX)
  if(NOT SQRAIL_IPO_SUPPORTED)
    message(FATAL_ERROR "IPO is unavailable: ${SQRAIL_IPO_ERROR}")
  endif()
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
endif()

if(SQRAIL_NATIVE_ARCH)
  check_cxx_compiler_flag("-march=native" SQRAIL_HAS_MARCH_NATIVE)
  if(SQRAIL_HAS_MARCH_NATIVE)
    add_compile_options(-march=native)
  else()
    check_cxx_compiler_flag("-mcpu=native" SQRAIL_HAS_MCPU_NATIVE)
    if(NOT SQRAIL_HAS_MCPU_NATIVE)
      message(FATAL_ERROR "The compiler has no supported native-CPU optimization flag")
    endif()
    add_compile_options(-mcpu=native)
  endif()
endif()

if(SQRAIL_ENABLE_HARDENING AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  add_compile_options(-fstack-protector-strong)
  if(UNIX AND NOT APPLE)
    # Hosted and distribution toolchains may inject their own fortify level.
    # Normalize it before applying sqrail's policy so strict builds remain
    # deterministic without weakening the requested protection.
    add_compile_options(
      $<$<NOT:$<CONFIG:Debug>>:-U_FORTIFY_SOURCE>
      $<$<NOT:$<CONFIG:Debug>>:-D_FORTIFY_SOURCE=3>
    )
  endif()
endif()
if(SQRAIL_ENABLE_SANITIZERS)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    message(FATAL_ERROR "SQRAIL_ENABLE_SANITIZERS requires Clang or GCC")
  endif()
  set(SQRAIL_SANITIZER_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer)
  # Instrument the embedded engine as well as the frontend. Sanitizing only the
  # thin CLI layer would miss almost all parser and file-format memory access.
  add_compile_options(${SQRAIL_SANITIZER_FLAGS})
  add_link_options(${SQRAIL_SANITIZER_FLAGS})
endif()
if(SQRAIL_ENABLE_THREAD_SANITIZER)
  if(SQRAIL_ENABLE_SANITIZERS)
    message(FATAL_ERROR "ThreadSanitizer and AddressSanitizer must use separate build trees")
  endif()
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    message(FATAL_ERROR "SQRAIL_ENABLE_THREAD_SANITIZER requires Clang or GCC")
  endif()
  set(SQRAIL_THREAD_SANITIZER_FLAGS -fsanitize=thread -fno-omit-frame-pointer)
  add_compile_options(${SQRAIL_THREAD_SANITIZER_FLAGS})
  add_link_options(${SQRAIL_THREAD_SANITIZER_FLAGS})
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  add_compile_options(
    $<$<CONFIG:Release>:-ffunction-sections>
    $<$<CONFIG:Release>:-fdata-sections>
  )
endif()

string(TOUPPER "${SQRAIL_DUCKDB_PROVIDER}" SQRAIL_DUCKDB_PROVIDER)
if(NOT SQRAIL_DUCKDB_PROVIDER MATCHES "^(BUNDLED|SYSTEM)$")
  message(FATAL_ERROR "SQRAIL_DUCKDB_PROVIDER must be BUNDLED or SYSTEM")
endif()

if(SQRAIL_DUCKDB_PROVIDER STREQUAL "BUNDLED")
  include(FetchContent)

  # Official sqrail releases deliberately pin one engine revision so their SQL
  # behavior is reproducible. Distribution packages can instead select SYSTEM
  # and track their packaged DuckDB dependency.
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
  # DuckDB enables ASan and UBSan by default for Debug builds, but those flags
  # are scoped to its subdirectory and do not reach sqrail's final link step.
  # Keep the embedded engine's switches off and let SQRAIL_ENABLE_SANITIZERS
  # instrument and link the entire process consistently.
  set(ENABLE_SANITIZER OFF CACHE BOOL "DuckDB sanitizer policy is managed by sqrail" FORCE)
  set(ENABLE_UBSAN OFF CACHE BOOL "DuckDB sanitizer policy is managed by sqrail" FORCE)
  set(ENABLE_THREAD_SANITIZER OFF CACHE BOOL "DuckDB sanitizer policy is managed by sqrail" FORCE)
  set(RELEASE_SANITIZER OFF CACHE BOOL "DuckDB sanitizer policy is managed by sqrail" FORCE)

  FetchContent_Declare(
    duckdb
    GIT_REPOSITORY "https://github.com/duckdb/duckdb.git"
    GIT_TAG "${DUCKDB_REVISION}"
    GIT_SHALLOW TRUE
    GIT_PROGRESS TRUE
  )
  FetchContent_MakeAvailable(duckdb)

  set(SQRAIL_DUCKDB_INCLUDE_DIRS "${duckdb_SOURCE_DIR}/src/include")
  if(APPLE)
    set(
      SQRAIL_DUCKDB_LIBRARIES
      duckdb_generated_extension_loader
      core_functions_extension
      parquet_extension
      json_extension
    )
  else()
    # GNU linkers resolve static archives from left to right. Put DuckDB before
    # the generated extension loader; extension targets add it again
    # transitively for symbols introduced by the extensions.
    set(
      SQRAIL_DUCKDB_LIBRARIES
      duckdb_static
      duckdb_generated_extension_loader
      core_functions_extension
      parquet_extension
      json_extension
    )
  endif()
else()
  if(SQRAIL_BUILD_DUCKDB_SHELL)
    message(FATAL_ERROR "SQRAIL_BUILD_DUCKDB_SHELL requires SQRAIL_DUCKDB_PROVIDER=BUNDLED")
  endif()

  find_package(DuckDB CONFIG REQUIRED)
  foreach(SQRAIL_REQUIRED_EXTENSION IN ITEMS core_functions parquet json)
    if(NOT SQRAIL_REQUIRED_EXTENSION IN_LIST DuckDB_EXTENSIONS)
      message(
        FATAL_ERROR
        "System DuckDB is missing required extension: ${SQRAIL_REQUIRED_EXTENSION}"
      )
    endif()
  endforeach()
  if(NOT TARGET duckdb)
    message(FATAL_ERROR "System DuckDB package did not export the shared duckdb target")
  endif()

  set(SQRAIL_DUCKDB_INCLUDE_DIRS "${DuckDB_INCLUDE_DIRS}")
  set(SQRAIL_DUCKDB_LIBRARIES duckdb)
endif()

add_library(sqrail_json STATIC src/json.cpp)
target_include_directories(sqrail_json PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

add_library(sqrail_cli STATIC src/cli.cpp)
target_include_directories(sqrail_cli PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

add_executable(
  sqrail
  src/main.cpp
  src/commands.cpp
  src/control.cpp
  src/engine.cpp
  src/platform.cpp
  src/result.cpp
)
target_include_directories(sqrail SYSTEM PRIVATE "${SQRAIL_DUCKDB_INCLUDE_DIRS}")
target_link_libraries(sqrail PRIVATE sqrail_cli sqrail_json ${SQRAIL_DUCKDB_LIBRARIES})
if(WIN32)
  target_link_libraries(sqrail PRIVATE advapi32)
  if(MSVC)
    target_link_options(
      sqrail
      PRIVATE "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/src/sqrail.manifest"
    )
  endif()
endif()
if(SQRAIL_DUCKDB_PROVIDER STREQUAL "SYSTEM")
  # Preserve non-default distribution prefixes (for example Linuxbrew) after
  # `cmake --install` removes the build-tree RPATH.
  set_property(TARGET sqrail PROPERTY INSTALL_RPATH_USE_LINK_PATH TRUE)
endif()
target_compile_definitions(
  sqrail
  PRIVATE
    SQRAIL_VERSION="${PROJECT_VERSION}"
)

function(sqrail_enable_project_warnings target)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(
      "${target}"
      PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wconversion
        -Wsign-conversion
        -Wformat=2
    )
  endif()
  if(MSVC)
    target_compile_options("${target}" PRIVATE /W4 /permissive- /Zc:preprocessor)
  endif()
  if(SQRAIL_WARNINGS_AS_ERRORS)
    if(MSVC)
      target_compile_options("${target}" PRIVATE /WX)
    else()
      target_compile_options("${target}" PRIVATE -Werror)
    endif()
  endif()
endfunction()

function(sqrail_enable_project_hardening target)
  if(SQRAIL_ENABLE_HARDENING AND MSVC)
    # Keep /sdl scoped to sqrail. Applying it directory-wide also promotes
    # warnings inside the pinned DuckDB dependency to errors; MSVC ARM64 emits
    # C4146 for valid unsigned arithmetic in DuckDB.
    target_compile_options("${target}" PRIVATE /sdl /guard:cf)
  endif()
endfunction()

sqrail_enable_project_warnings(sqrail_json)
sqrail_enable_project_warnings(sqrail_cli)
sqrail_enable_project_warnings(sqrail)
sqrail_enable_project_hardening(sqrail_json)
sqrail_enable_project_hardening(sqrail_cli)
sqrail_enable_project_hardening(sqrail)

if(SQRAIL_ENABLE_HARDENING AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  if(UNIX AND NOT APPLE)
    target_link_options(sqrail PRIVATE -Wl,-z,relro,-z,now)
  endif()
endif()
if(SQRAIL_ENABLE_HARDENING AND MSVC)
  target_link_options(sqrail PRIVATE /DYNAMICBASE /NXCOMPAT /guard:cf)
endif()

if(SQRAIL_ENABLE_CLANG_TIDY)
  find_program(SQRAIL_CLANG_TIDY NAMES clang-tidy REQUIRED)
  add_custom_target(
    sqrail-static-analysis
    COMMAND
      "${CMAKE_COMMAND}" -E env
      "CLANG_TIDY=${SQRAIL_CLANG_TIDY}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/static-analysis.sh"
      "${CMAKE_BINARY_DIR}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Running Clang-Tidy over every non-fuzz sqrail translation unit"
    VERBATIM
  )
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  if(CMAKE_BUILD_TYPE STREQUAL "Release")
    if(APPLE)
      target_link_options(sqrail PRIVATE -Wl,-dead_strip)
    else()
      target_link_options(sqrail PRIVATE -Wl,--gc-sections)
    endif()
  endif()
endif()

if(
  SQRAIL_STATIC_CPP_RUNTIME
  AND NOT SQRAIL_ENABLE_SANITIZERS
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
    docs/PLATFORMS.md
    docs/V0.3_RELEASE.md
  DESTINATION "${CMAKE_INSTALL_DOCDIR}/docs"
  COMPONENT sqrail
)
install(
  FILES
    benchmarks/README.md
    benchmarks/AGENT_EVALUATION.md
    benchmarks/agent-tasks.json
    benchmarks/agent-tasks-v0.3.json
    benchmarks/agent_oracle.py
  DESTINATION "${CMAKE_INSTALL_DOCDIR}/benchmarks"
  COMPONENT sqrail
)
install(
  PROGRAMS benchmarks/agent-eval.py benchmarks/agent-run.py
  DESTINATION "${CMAKE_INSTALL_DOCDIR}/benchmarks"
  COMPONENT sqrail
)
install(
  PROGRAMS benchmarks/agent-eval/run.py
  DESTINATION "${CMAKE_INSTALL_DOCDIR}/benchmarks/agent-eval"
  COMPONENT sqrail
)
install(
  FILES benchmarks/agent-eval/launcher.c
  DESTINATION "${CMAKE_INSTALL_DOCDIR}/benchmarks/agent-eval"
  COMPONENT sqrail
)
if(SQRAIL_DUCKDB_PROVIDER STREQUAL "BUNDLED")
  install(
    DIRECTORY "${duckdb_SOURCE_DIR}/third_party/"
    DESTINATION "${CMAKE_INSTALL_DOCDIR}/licenses/duckdb-third-party"
    COMPONENT sqrail
    FILES_MATCHING
      PATTERN "LICENSE*"
      PATTERN "COPYING*"
      PATTERN "NOTICE*"
  )
endif()

if(SQRAIL_BUILD_TESTS)
  enable_testing()
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_executable(sqrail-json-test tests/json_test.cpp)
  target_link_libraries(sqrail-json-test PRIVATE sqrail_json)
  sqrail_enable_project_warnings(sqrail-json-test)
  add_executable(sqrail-cli-test tests/cli_test.cpp)
  target_link_libraries(sqrail-cli-test PRIVATE sqrail_cli)
  sqrail_enable_project_warnings(sqrail-cli-test)
  add_test(NAME sqrail-json-unit COMMAND sqrail-json-test)
  add_test(NAME sqrail-cli-unit COMMAND sqrail-cli-test)
  add_test(
    NAME sqrail-agent-eval-unit
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/agent_eval_test.py"
  )
  add_test(
    NAME sqrail-agent-oracle-unit
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/agent_oracle_test.py"
  )
  add_test(
    NAME sqrail-agent-runner-unit
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/agent_runner_test.py"
  )
  add_test(
    NAME sqrail-package-tools-unit
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/package_tools_test.py"
  )
  add_test(
    NAME sqrail-benchmark-environment-unit
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/benchmark_environment_test.py"
  )
  add_test(
    NAME sqrail-cross-platform
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/cross_platform_test.py"
      "$<TARGET_FILE:sqrail>"
  )
  if(UNIX)
    add_test(
      NAME sqrail-smoke
      COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/smoke.sh" "$<TARGET_FILE:sqrail>"
    )
  endif()
  set_tests_properties(
    sqrail-json-unit
    sqrail-cli-unit
    sqrail-agent-eval-unit
    sqrail-agent-oracle-unit
    sqrail-agent-runner-unit
    sqrail-package-tools-unit
    sqrail-benchmark-environment-unit
    sqrail-cross-platform
    PROPERTIES TIMEOUT 120
  )
  if(UNIX)
    set_tests_properties(sqrail-smoke PROPERTIES TIMEOUT 120)
  endif()
endif()

if(SQRAIL_BUILD_FUZZERS)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "SQRAIL_BUILD_FUZZERS requires Clang")
  endif()
  add_executable(sqrail-strict-json-fuzz fuzz/strict_json_fuzz.cpp src/json.cpp)
  target_include_directories(sqrail-strict-json-fuzz PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  sqrail_enable_project_warnings(sqrail-strict-json-fuzz)
  add_executable(sqrail-cli-fuzz fuzz/cli_fuzz.cpp src/cli.cpp)
  target_include_directories(sqrail-cli-fuzz PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  sqrail_enable_project_warnings(sqrail-cli-fuzz)
  target_compile_options(
    sqrail-strict-json-fuzz
    PRIVATE
      -fsanitize=fuzzer,address,undefined
      -fno-omit-frame-pointer
  )
  target_link_options(sqrail-strict-json-fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
  target_compile_options(
    sqrail-cli-fuzz
    PRIVATE
      -fsanitize=fuzzer,address,undefined
      -fno-omit-frame-pointer
  )
  target_link_options(sqrail-cli-fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
endif()
