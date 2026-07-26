#!/usr/bin/env bash
set -euo pipefail

build_directory=${1:?CMake build directory is required}
clang_tidy=${CLANG_TIDY:-clang-tidy}
script_directory=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repository_directory=$(cd "$script_directory/.." && pwd)

run_checked() {
  local output
  local command_status

  set +e
  output=$("$@" 2>&1)
  command_status=$?
  set -e
  printf '%s\n' "$output"

  if [ "$command_status" -ne 0 ] ||
     printf '%s\n' "$output" | grep -Eq '(^|:[0-9]+:[0-9]+: )error:'; then
    return 1
  fi
}

run_checked "$clang_tidy" --verify-config
run_checked \
  "$clang_tidy" \
  --warnings-as-errors='*' \
  -p="$build_directory" \
  "$repository_directory/src/main.cpp" \
  "$repository_directory/src/json.cpp" \
  "$repository_directory/tests/json_test.cpp"
