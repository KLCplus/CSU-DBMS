_csudb_completion()
{
  local current="${COMP_WORDS[COMP_CWORD]}"
  COMPREPLY=($(compgen -W '--help --version --host --port --user --password --database --profile --config --execute --file --batch --silent --table --no-color --ping' -- "${current}"))
}
complete -F _csudb_completion csudb
