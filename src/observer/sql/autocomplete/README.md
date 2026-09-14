# CSUDB SQL 输入补全（Autocomplete）

本模块实现 **用户输入 SQL 命令时的自动补全**，采用「编译器/目录驱动的确定性补全 + 可选 Qwen2.5-Coder FIM 小模型」的混合架构。它不是自然语言转 SQL，也不是聊天，更不是 C++ 代码补全。

设计原则：

1. **不让 LLM 决定 SQL 是否合法。** 语法合法性、表名、列名全部由现有 Lexer/Parser/Catalog 保证。
2. **不引入第二套 SQL Parser。** 关键字补全直接复用 bison 的期望符号集合（`collect_expected_tokens`）。
3. **模型是增强项。** 模型不可用 / 超时 / 校验失败时，确定性补全照常工作，不阻塞、不报错。

## 目录结构

```
sql/autocomplete/
├── completion_types.{h,cpp}          # 补全请求/响应/候选数据结构
├── current_statement_extractor.{h,cpp}  # 取光标所在 statement（分号/注释/字符串安全）
├── sql_text_scanner.{h,cpp}          # 轻量文本扫描（不是语法分析器）
├── sql_capabilities.{h,cpp}          # 运行时 SQL 能力表（方言门禁）
├── grammar_completion_provider.{h,cpp}  # 基于 Parser 期望集合的关键字补全
├── catalog_completion_provider.{h,cpp}  # 基于 Catalog 的表/列补全
├── completion_scope.{h,cpp}          # FROM/JOIN/INSERT INTO 作用域扫描
├── completion_engine.{h,cpp}         # 统一入口 complete()
├── sql_completion_config.{h,cpp}     # 配置加载（config/sql_completion.json）
├── llama_completion_client.{h,cpp}   # llama.cpp /infill HTTP 客户端（带 timeout）
├── model_context_builder.{h,cpp}     # dialect + 相关 schema 上下文
├── model_completion_provider.{h,cpp} # 模型 provider（健康检查 + 调用 + 校验）
└── model_completion_validator.{h,cpp} # 方言/Parser/Catalog 校验 + 最长合法前缀
```

Parser 侧新增（复用同一份语法）：

- `sql/parser/expected_tokens.h` 与 `yacc_sql.y` 中的 `collect_expected_tokens`：
  在 SQL 前缀末尾追加哨兵字符，让 Parser 在光标处产生语法错误，
  再通过 `%define parse.error custom` 的 `yypcontext_expected_tokens` 取出期望 terminal 集合。

## 确定性补全覆盖

- 关键字：`SELECT/FROM/WHERE/AND/OR/NOT/...`，来自 Parser 期望集合，并用同一 Parser 试探确认续写关键字。
- 表名：`FROM` / `JOIN` / `INSERT INTO` 之后来自真实 Catalog。
- 列名：`SELECT` / `WHERE` / `ON` / `GROUP BY` / `ORDER BY` 中来自当前 scope 的表；
  `INSERT INTO t(` 中来自表 `t` 的列；`t.` 形式按限定名补全。
- 类型：`CREATE TABLE` 列定义处只推荐项目真实支持的类型（`INT/CHAR/FLOAT/VECTOR/DATE`）。
- 大小写：跟随用户当前语句风格（`select * fr` → `from`）。
- 不补全：注释内、未闭合字符串内、非法字符处、SQL buffer 为空。

> 说明：当前 grammar 不支持 `FROM t alias`，因此 `table_alias = false`，不做别名补全；
> `HAVING/LIMIT/UNION/DISTINCT/ALTER/...` 不在能力表中，任何来源（含模型）都不会推荐。

## 协议接口

native 协议新增 `complete` 请求：

```json
{ "type": "complete", "sql": "SELECT * FR", "cursor": 11, "max_items": 12, "want_model": false }
```

响应复用通用结果结构：`rows` 每行是
`[insert_text, display_text, kind, source, replace_start, replace_end, score, detail]`，
`attributes.ghost_text` 为模型 ghost text，`attributes.model_used` 表示是否使用模型。

## 客户端

- 交互式 `csudb`：Tab 触发确定性补全；ghost text 在模型可用时显示。
- 一次性演示：

```bash
csudb -h 127.0.0.1 -P 6789 -u root --complete "SELECT * FR"
```

（非交互场景可用环境变量 `CSUDB_PASSWORD` 提供密码。）

- 交互内：`/complete SELECT * FROM student W`

## 配置

`etc/sql_completion.json`（也可用环境变量 `CSUDB_SQL_COMPLETION_CONFIG` 指定）：

```json
{
  "enabled": true,
  "model_enabled": true,
  "llama_base_url": "http://127.0.0.1:8012",
  "model_debounce_ms": 150,
  "model_http_timeout_ms": 800,
  "model_max_tokens": 32,
  "model_temperature": 0.0,
  "max_completion_items": 12,
  "max_schema_tables": 8,
  "max_schema_columns": 128,
  "max_context_tokens": 4096
}
```

`model_enabled=false` 时完全不启动模型路径，只保留确定性补全。

## P40 模型 sidecar

模型：`Qwen/Qwen2.5-Coder-1.5B`（Base，非 Instruct），GGUF `ggml-org/Qwen2.5-Coder-1.5B-Q8_0-GGUF`。

```bash
# 1) 构建 llama.cpp（CUDA 12.x / sm_61 / MMQ）
scripts/build_llama_p40.sh

# 2) 启动模型（仅监听 127.0.0.1:8012）
scripts/run_sql_completion_model.sh

# 3) 冒烟测试 /infill
curl -s http://127.0.0.1:8012/infill \
  -H 'Content-Type: application/json' \
  -d '{"input_prefix":"SELECT * FROM student WHERE ","input_suffix":";","n_predict":16}'
```

注意：P40 为 Pascal / CC 6.1，不要使用 CUDA 13.x。生产配置使用 `-ngl 99 -c 4096 -np 1 --cache-prompt --cache-reuse 64 -fa off`。

## 测试

- 单元测试：`unittest/observer/autocomplete_test.cpp`（扫描器、statement 提取、能力表、Parser 期望集合、
  grammar 补全、engine 关键字/大小写/门禁/注释字符串/多语句、模型输出校验）。
- 集成测试：`SQL_description/test/cases/autocomplete.py`，通过 native 协议对真实 Catalog 验证表/列/INSERT 补全、
  方言门禁与边界行为。运行：

```bash
python3 SQL_description/test/run_tests.py --filter autocomplete
```

## 尚未实现 / 受 capability 关闭

- 表别名 `FROM t a` 与别名补全（grammar 未支持）。
- 异步/debounce/stale generation 的完整 UI 管线：当前确定性补全由客户端同步请求（本地 socket，通常在 10ms 内）；
  模型 ghost text 走同一请求且带 HTTP deadline，超时即丢弃。
- 未采用任何 NL2SQL / 聊天 / Agent 能力。
