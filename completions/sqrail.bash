# shellcheck shell=bash
# Bash completion for sqrail.
#
# The completion is static: it never runs sqrail, opens data files, or uses
# the network. Inputs, table paths, outputs, and spill roots complete as paths.

_sqrail_reply() {
  local candidate
  COMPREPLY=()
  while IFS= read -r candidate; do
    COMPREPLY+=("$candidate")
  done < <(compgen "$@")
}

# Completes $cur as a path, or as a directory with -d. bash-completion's
# _filedir handles quoting and tildes when it is loaded.
_sqrail_paths() {
  if declare -F _filedir >/dev/null 2>&1; then
    _filedir "$@"
  else
    _sqrail_reply "${1:--f}" -- "$cur"
  fi
}

_sqrail() {
  local cur=${COMP_WORDS[COMP_CWORD]}
  local prev=${COMP_WORDS[COMP_CWORD - 1]}
  local command=${COMP_WORDS[1]}
  local options index
  COMPREPLY=()

  if ((COMP_CWORD == 1)); then
    _sqrail_reply -W "schema check run --help --version" -- "$cur"
    return 0
  fi

  case $command in
    schema)
      options="--memory --threads --timeout --max-input-files --strict-schema"
      ;;
    check)
      options="-t --memory --threads --timeout --max-rows --max-input-files"
      options+=" --max-sql-bytes --strict-schema"
      ;;
    run)
      options="-t -o --memory --threads --spill --max-spill --timeout --max-rows"
      options+=" --max-output-bytes --max-input-files --max-sql-bytes --stats"
      options+=" --strict-schema"
      ;;
    *)
      return 0
      ;;
  esac

  # The default COMP_WORDBREAKS splits "-t NAME=PATH" at "=", so PATH arrives
  # as its own word, or the current word is "=" when PATH is still empty.
  if [[ $cur == "=" && ${COMP_WORDS[COMP_CWORD - 2]} == -t ]]; then
    cur=""
    _sqrail_paths
    return 0
  fi
  if [[ $prev == "=" && ${COMP_WORDS[COMP_CWORD - 3]} == -t ]]; then
    _sqrail_paths
    return 0
  fi

  case $prev in
    -o | *"<" | *">")
      _sqrail_paths
      return 0
      ;;
    --spill)
      _sqrail_paths -d
      return 0
      ;;
    -t | --memory | --threads | --max-spill | --timeout | --max-rows | \
      --max-output-bytes | --max-input-files | --max-sql-bytes)
      return 0
      ;;
  esac

  for ((index = 2; index < COMP_CWORD; index++)); do
    if [[ ${COMP_WORDS[index]} == "--" ]]; then
      if [[ $command == schema ]]; then
        _sqrail_paths
      fi
      return 0
    fi
  done

  if [[ $cur == -* ]]; then
    _sqrail_reply -W "$options --" -- "$cur"
  elif [[ $command == schema ]]; then
    _sqrail_paths
  fi
  return 0
}

complete -o filenames -F _sqrail sqrail
