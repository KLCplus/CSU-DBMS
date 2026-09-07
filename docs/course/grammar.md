# MiniDB SQL Grammar

本文依据当前仓库的 `src/observer/sql/parser/lex_sql.l` 与 `yacc_sql.y` 整理。它描述当前代码，而不是重新设计 SQL。Flex/Bison 生成的 `lex_sql.cpp/.h` 与 `yacc_sql.cpp/.hpp` 被 `.gitignore` 忽略，不应手工编辑。

## 1. 词法规则

- 关键字大小写不敏感（Flex 使用 `%option case-insensitive`）。
- 标识符：`[A-Za-z_]+[A-Za-z0-9_]*`。
- 整数：十进制数字；浮点数：`digits.digits`。
- 字符串：单引号或双引号包围，当前规则不处理转义引号。
- 比较符：`=`, `<`, `>`, `<=`, `>=`, `<>`, `!=`。
- 算术符：`+`, `-`, `*`, `/`。
- 分号可选，但 CLI 示例统一使用分号。

## 2. Core Grammar（当前实现）

以下 EBNF 是对真实 Bison 产生式的等价摘要；标识符、值等细节以上游文件为准。

```ebnf
command       ::= create_table | insert | select | delete [ ";" ]

create_table  ::= "CREATE" "TABLE" identifier "("
                  attr_def { "," attr_def }
                  [ "," "PRIMARY" "KEY" "(" identifier { "," identifier } ")" ]
                  ")"
                  [ "STORAGE" "FORMAT" "=" identifier ]

attr_def      ::= identifier type [ "(" integer ")" ]
type          ::= "INT" | "CHAR" | "FLOAT" | "VECTOR"

insert        ::= "INSERT" "INTO" identifier "VALUES" "("
                  value { "," value } ")"
value         ::= integer | float | quoted_string

select        ::= "SELECT" expression { "," expression }
                  "FROM" identifier { "," identifier }
                  [ where ] [ group_by ]

delete        ::= "DELETE" "FROM" identifier [ where ]

where         ::= "WHERE" condition { "AND" condition }
condition     ::= operand comparison operand
operand       ::= attribute | value
attribute     ::= identifier | identifier "." identifier
comparison    ::= "=" | "<" | ">" | "<=" | ">=" | "<>" | "!="

expression    ::= expression "+" expression
                | expression "-" expression
                | expression "*" expression
                | expression "/" expression
                | "(" expression ")"
                | "-" expression
                | "*"
                | value
                | attribute
                | identifier "(" expression ")"

group_by      ::= "GROUP" "BY" expression { "," expression }
```

算术优先级由 Bison 声明确定：`*`/`/` 高于 `+`/`-`，一元负号使用 `UMINUS` 且优先级更高。

## 3. WHERE、AND、OR、NOT 的真实状态

当前版本的 Lexer 和 Parser：

- 支持 `WHERE`。
- 支持由 `AND` 串联的一个或多个比较条件。
- 支持字段-值、值-值、字段-字段和值-字段比较。
- **不包含 `OR` token/产生式。**
- **不包含 `NOT` token/产生式。**
- WHERE 条件本身不是通用 `expression`，不能在条件中使用任意括号布尔表达式。

因此，`OR`/`NOT` 是课程希望补齐的语法能力，但不是 MiniDB Baseline v0.1 已实现能力。本次基座整理没有为它们改写 Parser。后续若实现，应同步修改 `lex_sql.l`、`yacc_sql.y`、Parsed SQL 表示、Binder/Filter、表达式树和优化规则，并增加 parser/integration tests。

## 4. 可运行示例

```sql
CREATE TABLE student (id INT, name CHAR(20));
INSERT INTO student VALUES (1, 'Alice');
INSERT INTO student VALUES (2, 'Bob');
SELECT * FROM student;
SELECT name FROM student WHERE id = 1;
DELETE FROM student WHERE id = 1;
SELECT * FROM student;
```

多条件示例（当前 AND 能力）：

```sql
SELECT name FROM student WHERE id >= 1 AND id < 10;
```

## 5. Existing Extended Grammar

`command_wrapper` 还接受下列已有命令。它们属于上游稳定能力或 Advanced/Reserved，不因课程 Core 子集而删除：

- `CALC expression_list`
- `UPDATE table SET field = value [WHERE ...]`
- `DROP TABLE table`
- `CREATE INDEX index ON table(field)` / `DROP INDEX index ON table`
- `SHOW TABLES` / `DESC table` / `ANALYZE TABLE table`
- `BEGIN` / `COMMIT` / `ROLLBACK`
- `SYNC`
- `LOAD DATA INFILE ... INTO TABLE ...`
- `EXPLAIN command`
- `SET variable = value`
- `HELP` / `EXIT`

> 注意：上面的能力是否适合某次课程实验，应由测试范围决定；“Parser 能接受”也不自动意味着所有边界语义均完整。应以 `Stmt::create_stmt`、对应 Executor/Operator 和集成测试共同确认。

## 6. 从语法到语义

1. `lex_sql.l` 输出 token。
2. `yacc_sql.y` 构造 `ParsedSqlNode`、`ConditionSqlNode` 和 `Expression` 派生对象。
3. `ParseStage` 将首个 Parsed SQL 放入 `SQLStageEvent`。
4. `ResolveStage` 调用 `Stmt::create_stmt`。
5. `SelectStmt::create`、`FilterStmt::create` 与 `ExpressionBinder` 查询 `Db/TableMeta/FieldMeta`，完成名称绑定和基础语义检查。

若增加语法可观测，最安全的第一步是在 token 生成后和 `ParsedSqlResult` 生成后增加只读事件/序列化，不改变现有节点所有权。
