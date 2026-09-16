# CSUDB 命令行客户端（csudb）的 Bash 补全脚本。
# 用法：source 本文件（或将其放入 /etc/bash_completion.d/）后，
#       在命令行输入 csudb 并按 Tab 即可补全选项。
# 说明：仅注册补全规则，不执行任何数据库操作。

# 补全函数：由 bash 的 complete 机制在按下 Tab 时调用。
# 读取当前正在输入的词 COMP_WORDS[COMP_CWORD]，用 compgen 在预定义选项列表中
# 匹配候选，结果写回 COMPREPLY 供 bash 展示。
_csudb_completion()
{
  local current="${COMP_WORDS[COMP_CWORD]}"
  COMPREPLY=($(compgen -W '--help --version --host --port --user --password --database --profile --config --execute --file --batch --silent --table --no-color --ping' -- "${current}"))
}

# 将上面的补全函数绑定到 csudb 命令。
complete -F _csudb_completion csudb
