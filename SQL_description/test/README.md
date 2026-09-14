# CSUDB SQL 编译器 / 基础 SQL 功能测试集

本目录是面向 `SQL_description/requirements.md` 的**自动化测试集**，聚焦「必须要完成的验收能力」以及词法 / 语法 / 语义分析的关键要求。进阶能力（UPDATE / ORDER BY / GROUP BY / JOIN / 算术表达式 / NULL 等）不作为本测试集的验收目标。

## 一、覆盖范围

| 分类 | 覆盖能力 |
| --- | --- |
| `lexer` | 注释（`--`、`/* */`）、跳过空白、多字符运算符（`>= <= != <> ==`）、字符串与转义（`''`、`\`、`""`）、大小写不敏感、非法输入（非法字符 / 未闭合字符串） |
| `parser` | 优先级 `NOT > 比较 > AND > OR`、括号改变结合、语法诊断（错误位置 + 实际符号 + `expected:` 期望集合） |
| `semantic` | 表 / 列存在性、名字绑定（限定 / 非限定）、类型一致性（`INT + VARCHAR` 报错）、INSERT 逐列 `TypeMismatch`、INSERT 匹配、错误定位 |
| `core` | CREATE / INSERT（含列清单）/ SELECT / DELETE / WHERE / 比较运算 / AND / OR / NOT / 括号 |
| `boundary` | 空输入、纯空白、极长标识符、多语句、无分号、前后空白、数值边界 |

必备能力逐项对照：

- CREATE：`core_create_insert_select`
- INSERT：`core_create_insert_select`（全列 / 列清单 / 省略列为 NULL）、`semantic_type_consistency`
- SELECT：`core_create_insert_select`、`core_where_comparison`
- DELETE：`core_delete`
- WHERE：`core_where_comparison`、`core_logical_and_or_not_paren`
- 比较运算：`core_where_comparison`、`lexer_operators`
- AND / OR / NOT / 括号：`core_logical_and_or_not_paren`、`parser_precedence`

## 二、目录结构

```
SQL_description/test/
├── README.md          # 本文件
├── run_tests.py       # 测试运行器（自动拉起临时服务端，或连接已有服务端）
└── cases/
    ├── __init__.py    # 汇总所有场景
    ├── core_sql.py    # CREATE/INSERT/SELECT/DELETE/WHERE/逻辑运算
    ├── lexer.py       # 词法分析
    ├── parser.py      # 语法分析与优先级
    ├── semantic.py    # 语义分析
    └── boundary.py    # 边界与健壮性
```

## 三、运行方式

### 1. 自动拉起临时服务端（推荐，可重复运行）

```bash
# 先构建
./build.sh debug --make -j4

# 运行全部用例
python3 SQL_description/test/run_tests.py

# 查看每个通过的用例
python3 SQL_description/test/run_tests.py --verbose

# 只运行某一类
python3 SQL_description/test/run_tests.py --filter lexer
python3 SQL_description/test/run_tests.py --filter parser
```

运行器会在临时目录初始化并启动 `csudbd`，用独立的临时数据目录保证与已有数据隔离，结束后自动清理。

### 2. 连接已经启动的服务端

```bash
python3 SQL_description/test/run_tests.py --host 127.0.0.1 --port 6789 --password <密码> --database <库>
```

注意：连接外部服务端时不会自动清理数据，请确保目标库中没有与本测试集同名的表（`c1`、`lx1`、`pp1`、`sm1` 等）。

## 四、结果与指标

运行结束会输出汇总。判定失败的类别与需求中的关注指标一一对应：

| 指标 | 含义 |
| --- | --- |
| `PASS` | 符合预期 |
| `FAIL` | 错误类型 / 错误消息内容不符合预期 |
| `CRASH` | 服务端崩溃或连接断开（本测试集要求为 0） |
| `WRONG_ACCEPT` | 预期报错却执行成功（错误被放过） |
| `WRONG_REJECT` | 预期成功却报错（正确语句被拒绝） |
| `WRONG_RESULT` | 结果集与预期不一致 |
| `WRONG_LOCATION` | 错误类型正确，但**错误位置**（行/列）不一致 |
| `SETUP_ERROR` | 前置 `setup` 语句执行失败 |

全部通过时退出码为 0，否则为 1，可直接接入 CI。

## 五、扩展用例

每个场景是一个字典，结构如下（见 `cases/core_sql.py` 示例）：

```python
{
    "name": "core_create_insert_select",
    "category": "core",
    "setup": [                       # 前置语句，顺序执行，失败则跳过该场景
        "CREATE TABLE c1(id int, name char(20), age int);",
        "INSERT INTO c1 VALUES (1, 'Alice', 20);",
    ],
    "checks": [
        {                            # 期望返回结果集
            "name": "select_star",
            "sql": "select * from c1;",
            "expect_cols": ["id", "name", "age"],
            "expect_rows": [[1, "Alice", 20]],
            # "ordered": True,        # 默认按无序（多重集合）比较
        },
        {                            # 期望报错
            "name": "unknown_column",
            "sql": "select nosuch from c1;",
            "expect_error": {
                "type": "SemanticError",   # 消息中需包含
                "contains": ["no such field"],  # 需包含的子串
                "line": 1, "column": 8,         # 可选：校验错误位置
            },
        },
        {"name": "insert_row", "sql": "insert into c1 values (2, 'Bob', 18);"},  # 仅要求成功
    ],
}
```

新增类别时，在本目录下创建 `cases/<name>.py` 导出 `SCENARIOS`，并在 `cases/__init__.py` 中聚合即可。
