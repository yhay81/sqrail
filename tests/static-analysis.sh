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

compile_database="$build_directory/compile_commands.json"
if [ ! -f "$compile_database" ]; then
  printf 'error: missing compilation database: %s\n' "$compile_database" >&2
  exit 1
fi

run_checked "$clang_tidy" --verify-config

candidate_sources=(
  "$repository_directory/src/main.cpp"
  "$repository_directory/src/commands.cpp"
  "$repository_directory/src/cli.cpp"
  "$repository_directory/src/control.cpp"
  "$repository_directory/src/engine.cpp"
  "$repository_directory/src/json.cpp"
  "$repository_directory/src/platform.cpp"
  "$repository_directory/src/result.cpp"
  "$repository_directory/tests/cli_test.cpp"
  "$repository_directory/tests/control_test.cpp"
  "$repository_directory/tests/json_test.cpp"
  "$repository_directory/fuzz/cli_fuzz.cpp"
  "$repository_directory/fuzz/strict_json_fuzz.cpp"
)

sources=()
for candidate_source in "${candidate_sources[@]}"; do
  if grep -Fq "$candidate_source" "$compile_database"; then
    sources+=("$candidate_source")
  fi
done

if [ "${#sources[@]}" -eq 0 ]; then
  printf 'error: no sqrail translation units found in %s\n' "$compile_database" >&2
  exit 1
fi

jobs=${CLANG_TIDY_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '4')}
case $jobs in
  ''|*[!0-9]*|0)
    jobs=4
    ;;
esac

log_directory=$(mktemp -d)
trap 'rm -rf "$log_directory"' EXIT

pids=()
index=0
for source_file in "${sources[@]}"; do
  while [ "${#pids[@]}" -ge "$jobs" ]; do
    next_pids=()
    for pid in "${pids[@]}"; do
      if kill -0 "$pid" 2>/dev/null; then
        next_pids+=("$pid")
      fi
    done
    if [ "${#next_pids[@]}" -ge "$jobs" ]; then
      wait "${next_pids[0]}" || true
      pids=("${next_pids[@]:1}")
    else
      pids=("${next_pids[@]}")
    fi
  done

  log_file="$log_directory/$index.log"
  status_file="$log_directory/$index.status"
  (
    set +e
    "$clang_tidy" --warnings-as-errors='*' -p="$build_directory" "$source_file" >"$log_file" 2>&1
    printf '%s\n' "$?" >"$status_file"
  ) &
  pids+=("$!")
  index=$((index + 1))
done

for pid in "${pids[@]}"; do
  wait "$pid" || true
done

failed=0
for ((i = 0; i < index; ++i)); do
  log_file="$log_directory/$i.log"
  status_file="$log_directory/$i.status"
  unit_status=$(cat "$status_file")
  if [ -s "$log_file" ]; then
    cat "$log_file"
  fi
  if [ "$unit_status" -ne 0 ] || grep -Eq '(^|:[0-9]+:[0-9]+: )error:' "$log_file"; then
    failed=1
  fi
done

exit "$failed"
