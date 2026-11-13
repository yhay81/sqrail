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
  local command=${COMP_WORDS[1]}
  local options index word value_for=""
  local positional_only=false redirect_target=false
  COMPREPLY=()

  if ((COMP_CWORD == 1)); then
    _sqrail_reply -W "schema check run --help --version" -- "$cur"
    return 0
  fi

  case $command in
    schema)
      options="--memory --threads --timeout --max-input-files --strict-schema --all-varchar"
      ;;
    check)
      options="-t --memory --threads --timeout --max-rows --max-input-files"
      options+=" --max-sql-bytes --strict-schema --all-varchar"
      ;;
    run)
      options="-t -o --memory --threads --spill --max-spill --timeout --max-rows"
      options+=" --max-output-bytes --max-input-files --max-sql-bytes --stats"
      options+=" --strict-schema --all-varchar"
      ;;
    *)
      return 0
      ;;
  esac

  for ((index = 2; index < COMP_CWORD; index++)); do
    word=${COMP_WORDS[index]}
    if [[ $redirect_target == true ]]; then
      redirect_target=false
    elif [[ $word == *"<" || $word == *">" ]]; then
      redirect_target=true
    elif [[ -n $value_for ]]; then
      # Bash splits NAME=PATH into three words with its default COMP_WORDBREAKS.
      if [[ ($value_for == -t || $value_for == --table) && ${COMP_WORDS[index + 1]} == "=" ]]; then
        value_for=table_path
        ((index += 1))
      else
        value_for=""
      fi
    elif [[ $positional_only == false ]]; then
      case $word in
        --) positional_only=true ;;
        -o | --output | --spill | -t | --table | --memory | --threads | --max-spill | \
          --timeout | --max-rows | --max-output-bytes | --max-input-files | --max-sql-bytes)
          value_for=$word
          ;;
      esac
    fi
  done

  if [[ $redirect_target == true ]]; then
    _sqrail_paths
    return 0
  fi
  if [[ $positional_only == true ]]; then
    if [[ $command == schema ]]; then
      _sqrail_paths
    fi
    return 0
  fi

  case $value_for in
    table_path)
      if [[ $cur == "=" ]]; then cur=""; fi
      _sqrail_paths
      return 0
      ;;
    -o | --output)
      _sqrail_paths
      return 0
      ;;
    --spill)
      _sqrail_paths -d
      return 0
      ;;
    ?*)
      return 0
      ;;
  esac

  if [[ $cur == -* ]]; then
    _sqrail_reply -W "$options --" -- "$cur"
  elif [[ $command == schema ]]; then
    _sqrail_paths
  fi
  return 0
}

complete -o filenames -F _sqrail sqrail
