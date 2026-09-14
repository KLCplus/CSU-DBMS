# CSU-DBMS SQL 输入补全实现方案

> 面向 OpenCode 的直接实现文档  
> 版本日期：2026-09-14  
> 目标：只负责**用户输入 SQL 命令时的自动补全**。不做自然语言转 SQL，不做聊天，不做 C++ 项目代码补全。  
> 部署硬件：单张 NVIDIA Tesla P40 24GB（Pascal / Compute Capability 6.1）

---

## 0. 最终决策

本项目采用 **“编译器/目录驱动的确定性补全 + 1.5B FIM 小模型补全”** 的混合架构。

### 选定模型

**Qwen/Qwen2.5-Coder-1.5B（Base，非 Instruct）**

推理格式使用 GGUF：

**ggml-org/Qwen2.5-Coder-1.5B-Q8_0-GGUF**

### 选定推理框架

**llama.cpp / llama-server**

### 为什么 2026 年仍选 Qwen2.5-Coder-1.5B

这不是因为没有更新的模型，而是因为任务是非常具体的 **cursor-level SQL Fill-in-the-Middle (FIM)**：

- 2026 年已有 Qwen3.5-2B，但官方模型卡主要是通用/多模态对话模型，没有把 FIM 作为其代码补全接口。
- Qwen3-Coder 的重点是 Agentic Coding，大尺寸模型不适合本项目的单 P40 低延迟补全。
- Qwen2.5-Coder Base 原生提供 `<|fim_prefix|> / <|fim_suffix|> / <|fim_middle|>`，官方也提供 repository-level FIM 格式。
- 到 2026 年当前版 llama.cpp 仍然直接提供 `--fim-qwen-1.5b-default`、`--fim-qwen-3b-default` 等专门的 Qwen2.5-Coder FIM 预设，说明这一条部署路径仍然是一等支持路径。
- SQL 本身只占本项目支持的很小语法子集，模型的作用不是“理解整个 SQL 世界”，而是给出**多 token 的自然延续**；语法正确性、表名、列名应由 DBMS 自身保证。

### 为什么不选这些模型

| 模型 | 不作为主模型的原因 |
|---|---|
| Qwen3.5-2B | 更新，但不是针对 FIM 代码补全设计；当前官方用法重点为通用/多模态 chat |
| Qwen3-1.7B | 通用模型，没有 Qwen2.5-Coder 那样成熟的 FIM 专用接口 |
| CodeGemma-2B | 有 FIM，能做 baseline，但年代较早，许可证也不是 Apache-2.0；本项目优先 Qwen |
| StarCoder2-3B | 支持 FIM，但体积更大且没有明显理由优于当前选择 |
| XiYanSQL-QwenCoder-3B | 面向 Text-to-SQL/NL2SQL，而本项目不是“自然语言生成 SQL” |
| DatA-SQL-1.5B | 同样主要是 Text-to-SQL，不是 cursor FIM |
| Qwen3-Coder 30B-A3B / 更大版本 | 即使 active 参数不大，总权重、部署复杂度和任务定位都不适合本项目 |

### 一个重要原则

**不要让 LLM 决定 SQL 是否合法。**

DBMS 已经拥有：

- Lexer
- Parser
- FIRST/FOLLOW/LL(1) 相关信息
- 语义分析
- Catalog
- 表/列类型信息

这些信息比任何 2B 模型都更准确。

因此：

1. **下拉候选列表**主要来自 Parser + Catalog。
2. **灰色 ghost text / 一整段补全**主要来自 1.5B FIM 模型。
3. 模型输出必须再次经过本地 SQL 语法/语义校验。
4. 模型服务挂掉时，SQL 补全仍必须可用。

---

# 1. 本项目允许补全的 SQL 能力边界

实现时首先读取现有项目的 Lexer/Parser 实际实现，并与 `SQL_description/requirements.md` 对齐。

## 1.1 必须覆盖

- `CREATE`
- `INSERT`
- `SELECT`
- `DELETE`
- `WHERE`
- 比较运算
- `AND`
- `OR`
- `NOT`
- 括号

## 1.2 如果项目中已经实现，则同时覆盖

- `UPDATE`
- `ORDER BY`
- `GROUP BY`
- `JOIN`
- 算术表达式
- `NULL`

## 1.3 类型系统

当前要求里明确出现：

- `INT`
- `VARCHAR`
- `BOOL`

补全器必须复用项目现有类型系统，不要自己维护第二套互相可能冲突的类型定义。

## 1.4 禁止凭模型扩展 SQL 方言

如果当前编译器没有实现以下语法，补全器不得因为大模型“知道标准 SQL”就建议它们：

- `HAVING`
- `DISTINCT`
- `UNION`
- `LIMIT`
- `OFFSET`
- `ALTER`
- `DROP`
- 窗口函数
- 子查询
- CTE
- 数据库厂商特有语法

如果未来项目真的实现这些功能，应通过 capability/grammar 自动开放，而不是修改 LLM prompt 硬开。

---

# 2. 总体架构

```text
用户每次修改 SQL
        │
        ▼
CurrentStatementExtractor
只取光标所在 SQL statement
        │
        ▼
Lexer / Partial Parser
        │
        ├─────────────► GrammarCompletionProvider
        │                   │
        │                   ├─ keyword
        │                   ├─ operator
        │                   ├─ delimiter
        │                   └─ expected token classes
        │
        ├─────────────► CatalogCompletionProvider
        │                   │
        │                   ├─ table
        │                   ├─ column
        │                   ├─ alias
        │                   └─ type-aware value/operator
        │
        ▼
Immediate Completion List
目标：立即返回，不等待 GPU
        │
        └──────────────► UI 下拉候选


用户停止输入约 120~180 ms
        │
        ▼
ModelTriggerPolicy
        │
        ├─ 明显的单一关键字补全？ ──► 不调用模型
        ├─ 位于注释/字符串？ ──────► 通常不调用模型
        └─ 需要 phrase-level continuation
                │
                ▼
SchemaContextBuilder
        │
        ▼
llama-server /infill
Qwen2.5-Coder-1.5B Base Q8_0
        │
        ▼
ModelCompletionValidator
        │
        ├─ 方言白名单
        ├─ Lexer
        ├─ Parser
        ├─ Catalog
        └─ 类型/名字检查
                │
                ▼
        Ghost Text / 长补全候选
```

---

# 3. 不要重新造一套 SQL Parser

这是实现中的强制要求。

requirements 中本来就要求 Parser 能给出：

- FIRST
- FOLLOW
- LL(1) 表
- 语法错误位置
- `unexpected token`
- `expected` 集合

自动补全与语法诊断实际上需要的是同一份信息。

例如：

```sql
SELECT name
FROM student
WHERE age > 18 AND |
```

Parser 在光标处已经应该知道下一项可以是类似：

```text
IDENTIFIER
CONST
(
NOT
```

那么补全器直接复用这个 expected set。

## 要新增的 Parser API

OpenCode 应先检查现有 Parser 的结构，如果已有类似能力就直接复用；如果没有，则新增一个很薄的接口，名称可按项目风格调整：

```cpp
struct ExpectedTokenSet {
    std::vector<TokenType> token_types;
    std::vector<std::string> literal_keywords;
};

ExpectedTokenSet expectedTokensAtCursor(
    std::string_view sql,
    size_t cursor_offset
);
```

实现原则：

1. 光标位置作为一个特殊 EOF/CURSOR 边界。
2. Lexer 允许最后一个 identifier/keyword 处于“未输入完整”的状态。
3. Parser 执行到 cursor 时，不把“不完整输入”当最终错误。
4. 返回此位置合法的 terminal / token class。
5. 不要求真正构建完整 AST。

SQL 命令通常很短，因此 V1 不需要实现复杂 incremental parser。每次按键重新 tokenize/partial parse 当前 statement 即可。

---

# 4. Completion 数据结构

增加一个与 UI 无关的后端接口。

```cpp
enum class CompletionKind {
    Keyword,
    Table,
    Column,
    Alias,
    Operator,
    Type,
    Literal,
    Snippet,
    Model
};

enum class CompletionSource {
    Grammar,
    Catalog,
    Semantic,
    Model
};

struct CompletionItem {
    std::string insert_text;
    std::string display_text;
    CompletionKind kind;
    CompletionSource source;

    // 替换用户当前半截 token，而不是一律插入
    size_t replace_start;
    size_t replace_end;

    // 越高越靠前
    double score;

    // 例如 "student.name : VARCHAR"
    std::string detail;
};

struct CompletionRequest {
    std::string sql;
    size_t cursor_offset;
    size_t max_items = 12;
    bool want_model_completion = true;
};

struct CompletionResponse {
    std::vector<CompletionItem> items;

    // 可为空。用于 inline ghost text。
    std::string ghost_text;

    bool model_used = false;
};
```

统一入口：

```cpp
CompletionResponse complete(const CompletionRequest &request);
```

不得让 UI 直接访问 Catalog、Parser 或 llama-server。

---

# 5. CurrentStatementExtractor

即使系统允许多条 SQL，模型和 Parser 补全只需要处理光标所在 statement。

实现：

```cpp
StatementSlice findStatementAtCursor(
    std::string_view buffer,
    size_t cursor
);
```

要求：

- 分号只在不位于字符串/注释时才视作 statement separator。
- 支持 `-- ...` 单行注释。
- 支持 `/* ... */` 多行注释。
- 支持单引号字符串和项目现有转义规则。
- 返回 statement 的原始 buffer range，方便 completion replacement 映射回 UI。

---

# 6. GrammarCompletionProvider

它负责“SQL 语法上现在能输入什么”。

例子：

```sql
SEL|
```

候选：

```text
SELECT
```

```sql
SELECT * FR|
```

候选：

```text
FROM
```

```sql
DELETE |
```

候选：

```text
FROM
```

```sql
ORDER |
```

只有在 ORDER BY 已被项目实际实现时：

```text
BY
```

## 关键规则

- 从 Parser expected set 生成候选。
- keyword 大小写跟随用户当前 statement 风格。
- 如果用户大部分 SQL keyword 是小写，返回小写。
- identifier 不在这里凭空猜；交给 Catalog Provider。
- 不允许建议 capability registry 中不存在的关键词。

---

# 7. CatalogCompletionProvider

requirements 已明确要求 Catalog 提供表/列查询相关能力，因此补全器必须复用现有 Catalog。

逻辑上需要：

```cpp
listTables()
findTable(name)
listColumns(table)
findColumn(table, column)
getType(table, column)
```

若实际 API 名字不同，适配现有 API，不要复制 Catalog。

## 7.1 FROM / JOIN 后

```sql
SELECT * FROM |
```

候选：

```text
student
score
course
...
```

来自真实 Catalog。

## 7.2 `alias.` 后

```sql
SELECT s.|
FROM student s
```

只返回 `student` 表真实存在的列。

## 7.3 WHERE / SELECT / GROUP BY / ORDER BY

根据当前 FROM/JOIN scope 提供：

- alias
- column
- `alias.column`

同名列存在歧义时优先显示带 alias 的形式。

## 7.4 INSERT

例如：

```sql
INSERT INTO student(|
```

候选为 `student` 的列。

已经出现的列不重复优先推荐。

```sql
INSERT INTO student(id, name) VALUES (|
```

补全上下文应知道：

- 第 1 个 value 对应 `id`
- 第 2 个 value 对应 `name`

可根据类型给出 literal/snippet 候选，但不要自动制造业务数据。

例如：

```text
0
NULL       # 仅 NULL 功能已启用时
''
```

## 7.5 CREATE TABLE

只建议当前编译器真实支持的数据类型：

```text
INT
VARCHAR
BOOL
```

不要因为模型知道 `DATE/DECIMAL/TEXT` 就建议它们。

---

# 8. Alias / Scope 解析

不用等完整 AST 成功后才能补全。

增加一个轻量 `CompletionScope`：

```cpp
struct TableBinding {
    std::string table_name;
    std::string alias;
};

struct CompletionScope {
    std::vector<TableBinding> tables;
};
```

构建顺序：

1. 如果当前 partial AST 已有 FROM/JOIN 信息，直接读取。
2. 如果 AST 尚未完成，用 token stream 对已经输入的 `FROM/JOIN ... alias` 做保守扫描。
3. 无法确认时宁可少建议，不要猜不存在的表。

---

# 9. CapabilityRegistry：阻止模型输出超出课程 SQL 子集

增加一份运行时 capability。

不要单纯维护一个脱离 Parser 的关键词大数组。

推荐结构：

```cpp
struct SqlCapabilities {
    bool create = true;
    bool insert = true;
    bool select = true;
    bool delete_stmt = true;
    bool where = true;

    bool update = false;
    bool order_by = false;
    bool group_by = false;
    bool join = false;
    bool arithmetic = false;
    bool null_value = false;

    std::unordered_set<std::string> keywords;
    std::unordered_set<std::string> operators;
    std::unordered_set<std::string> types;
};
```

初始化方式优先级：

1. 从现有 grammar/parser feature 定义生成；
2. 如果当前代码结构暂时不方便自动生成，则建立一个集中配置；
3. 测试必须验证 capability 与 Parser 实际支持范围一致。

**不要从 requirements 文档盲目开启“进阶版本”能力。只有 Parser/Executor 已实现时才开放对应补全。**

---

# 10. LLM 的职责

LLM 不负责：

- 判断表是否存在
- 判断列是否存在
- 判断 SQL 能否执行
- 决定项目支持什么 SQL
- 执行 SQL
- 自然语言问答
- Text-to-SQL

LLM 只负责：

> 根据 cursor 左右的 SQL、相关 schema 和受限 dialect，上下文预测“用户下一段最可能输入什么”。

典型价值：

```sql
SELECT department, COUNT(*)
FROM employee
|
```

模型可能补：

```sql
GROUP BY department
```

或者：

```sql
SELECT *
FROM orders o
JOIN customers c ON |
```

模型可以预测：

```sql
o.customer_id = c.id
```

然后本地 validator 再验证列是否真的存在。

---

# 11. 模型输入

使用 llama.cpp `/infill` endpoint，不使用 `/chat/completions`。

Qwen2.5-Coder Base 的原生任务正是：

```text
prefix + suffix -> middle
```

## 11.1 prefix

只包含：

- 光标所在当前 SQL statement
- 光标左侧内容

不要把整个 REPL 历史塞进去。

## 11.2 suffix

光标右侧 SQL。

如果命令行模式光标总在末尾，则 suffix 可以为空。

## 11.3 input_extra

利用 Qwen2.5-Coder repo-level FIM 的 extra context，传两个小文件：

### `dialect.sql`

动态生成，例如：

```sql
-- CSU_DBMS_SQL_DIALECT
-- Supported statements:
-- CREATE TABLE
-- INSERT INTO ... VALUES ...
-- SELECT ... FROM ... [WHERE ...]
-- DELETE FROM ... [WHERE ...]
-- UPDATE ... SET ... [WHERE ...]          -- only if enabled
-- JOIN ... ON ...                         -- only if enabled
-- GROUP BY ...                            -- only if enabled
-- ORDER BY ...                            -- only if enabled
--
-- Boolean: AND OR NOT
-- Types: INT VARCHAR BOOL
-- Only use syntax listed above.
```

必须根据 `SqlCapabilities` 生成，而不是固定写死。

### `schema.sql`

不要把全库无脑塞进去。

优先顺序：

1. 当前 statement 已引用的表；
2. 当前正在输入前缀能匹配到的表；
3. JOIN 相关表；
4. 其余表仅在 catalog 很小时加入。

示例：

```sql
CREATE TABLE student (
    id INT,
    name VARCHAR,
    age INT,
    class_id INT
);

CREATE TABLE score (
    student_id INT,
    score INT
);
```

### Context 预算

V1 将模型 context 控制在 **4096 tokens**。

建议：

- SQL prefix/suffix：通常 < 1000 tokens
- schema：上限约 1200 tokens
- dialect：< 300 tokens
- 剩余给模型内部格式与生成

不要因为模型支持 32K 就真的给 P40 32K。

---

# 12. llama.cpp `/infill` 请求

示例：

```json
{
  "input_prefix": "SELECT s.name FROM student s WHERE s.",
  "input_suffix": ";",
  "input_extra": [
    {
      "filename": "dialect.sql",
      "text": "-- dynamically generated CSU DBMS dialect"
    },
    {
      "filename": "schema.sql",
      "text": "CREATE TABLE student (id INT, name VARCHAR, age INT);"
    }
  ],
  "n_predict": 32,
  "temperature": 0.0,
  "top_k": 1,
  "cache_prompt": true,
  "n_cache_reuse": 64,
  "t_max_predict_ms": 150
}
```

客户端自身还必须设置 HTTP deadline，例如 **800 ms**。

如果超时：

- 丢弃模型结果；
- 不影响确定性补全；
- 不弹错误打断用户。

新按键出现后，旧模型请求的结果必须视为 stale，不得覆盖新输入。

可以使用 generation id：

```cpp
uint64_t completion_generation;
```

request 发出时记录 generation；响应回来时只有与当前 generation 相等才显示。

---

# 13. 模型触发策略

P40 虽然能跑这个模型，但没有必要每敲一个字符都发一次 GPU 请求。

## 13.1 每次按键立即执行

- CurrentStatementExtractor
- Lexer
- partial parser
- grammar completion
- catalog completion

## 13.2 LLM debounce

用户停止输入 **120~180 ms** 后再考虑触发模型。

默认建议 150 ms。

## 13.3 不调用模型的情况

以下情况 deterministic completion 已经足够：

```sql
SEL|
SELECT * FR|
DELETE |
ORDER |
GROUP |
INSERT |
```

如果 parser/candidate engine 能以极高置信度给出唯一下一关键词，不调用模型。

以下位置也默认不调用：

- 注释中
- 未闭合字符串内部
- SQL buffer 为空
- 当前 token 只有 1 个随机字符且无法判断上下文
- llama-server health 不正常

## 13.4 调用模型的情况

- WHERE 条件后半段
- JOIN ON 条件
- GROUP BY / ORDER BY 后的 expression
- SELECT list 多 token continuation
- INSERT VALUES 的较长 continuation
- 光标位于一条已有 SQL 中间，需要 suffix-aware FIM
- deterministic provider 有多个合理候选，希望得到 phrase-level ghost text

---

# 14. P40 推理方案

## 14.1 硬件事实

Tesla P40：

- Pascal
- Compute Capability 6.1
- 24 GB GDDR5
- INT8 吞吐能力强
- 没有现代 Tensor Core
- FP16 路径不应作为本项目主要优化方向

因此本项目使用：

**GGUF Q8_0 + llama.cpp CUDA MMQ**

而不是 BF16/vLLM。

## 14.2 为什么不用 vLLM

当前 vLLM 的 NVIDIA GPU 要求已经高于 P40 的 Compute Capability 6.1。

因此不要花时间尝试“强行在 P40 上装当前 vLLM”。

## 14.3 CUDA 版本

**P40 构建 llama.cpp 时使用 CUDA 12.x，不要使用 CUDA 13.x。**

当前 llama.cpp 的 CUDA CMake 逻辑也只会在 `CUDAToolkit_VERSION < 13` 时加入较老 CUDA architecture；Pascal 的 sm_61 应显式指定。

如果服务器当前 `nvcc --version` 显示 CUDA 13：

- 保留现有 NVIDIA driver 可以；
- 另装 CUDA 12.x toolkit 或使用 CUDA 12.x build container；
- 用 CUDA 12.x 的 `nvcc` 构建 llama.cpp。

---

# 15. llama.cpp 构建

先检查：

```bash
nvidia-smi
nvcc --version
```

构建：

```bash
git clone https://github.com/ggml-org/llama.cpp.git
cd llama.cpp

cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DGGML_CUDA=ON \
  -DCMAKE_CUDA_ARCHITECTURES=61 \
  -DGGML_CUDA_FORCE_MMQ=ON

cmake --build build -j
```

验收：

```bash
./build/bin/llama-server --version
./build/bin/llama-server --list-devices
```

输出中必须能看到 Tesla P40 / compute capability 6.1。

---

# 16. 模型启动

推荐正式配置：

```bash
./build/bin/llama-server \
  -hf ggml-org/Qwen2.5-Coder-1.5B-Q8_0-GGUF:Q8_0 \
  --host 127.0.0.1 \
  --port 8012 \
  -ngl 99 \
  -c 4096 \
  -np 1 \
  --cache-prompt \
  --cache-reuse 64 \
  -fa off
```

说明：

- `Q8_0`：模型仍很小，P40 24GB 完全没有必要为了省显存上极端 Q4。
- `-ngl 99`：全部层尽量放到 GPU。
- `-c 4096`：SQL completion 不需要长上下文。
- `-np 1`：先以单用户最低延迟为目标。
- prompt cache：用户连续打字时，大部分 prefix 重复，应利用缓存。
- `-fa off`：作为 Pascal 的保守基线。完成 benchmark 后可以 A/B 测试 `-fa on`，若无正确性/稳定性问题且确实更快再启用。

开发时也可直接验证 llama.cpp 自带的：

```bash
./build/bin/llama-server \
  --fim-qwen-1.5b-default \
  --host 127.0.0.1
```

但生产配置建议显式锁定模型和 quant，避免未来默认值变化。

---

# 17. ModelCompletionProvider

实现一个独立 provider。

```cpp
class ModelCompletionProvider {
public:
    ModelCompletionProvider(
        std::shared_ptr<LlamaCompletionClient> client,
        const SqlCapabilities &capabilities,
        Catalog &catalog
    );

    std::optional<std::string> complete(
        const CompletionContext &ctx
    );
};
```

## LlamaCompletionClient

只负责 HTTP，不知道 SQL：

```cpp
struct InfillRequest {
    std::string prefix;
    std::string suffix;
    std::vector<ExtraContextFile> extra;
    int max_tokens = 32;
};

struct InfillResult {
    std::string text;
    int64_t elapsed_ms;
};

class LlamaCompletionClient {
public:
    bool health();
    std::optional<InfillResult> infill(const InfillRequest &request);
};
```

要求：

- 连接地址默认 `127.0.0.1:8012`
- 地址可配置
- 连接失败不抛到 SQL 主流程
- deadline
- request cancellation / stale response handling
- 不做自动无限重试
- health check 有短 TTL，例如 2 秒，避免每次按键都 `/health`

---

# 18. 模型结果校验

模型给出的字符串不得直接显示。

实现：

```cpp
class ModelCompletionValidator {
public:
    ValidationResult validate(
        const CompletionContext &context,
        std::string_view model_text
    );
};
```

校验顺序：

## 18.1 清理明显非代码输出

拒绝/裁剪：

- Markdown code fence
- `Here is ...`
- 中文/英文解释性句子
- FIM special token 泄漏
- 多条不相关 SQL

Base FIM 正常情况下不应产生这些，但仍要防御。

## 18.2 长度限制

默认：

- 最大 32 tokens
- 或最大 256 characters
- 到第一个完整合理 clause / statement boundary 即可

Autocomplete 的价值是“下一段”，不是替用户写 100 行 SQL。

## 18.3 方言检查

Tokenize model output。

若出现 capability registry 不允许的 keyword/operator/type：

- 优先截断到该 token 之前；
- 若剩余为空则丢弃。

## 18.4 Parser 检查

将：

```text
prefix + model_completion + suffix
```

交给 partial/full parser。

策略：

1. 完整 SQL 能 parse：通过。
2. 因 suffix/statement 本来未完成导致 EOF 类错误，但新增片段仍保持 prefix-valid：可通过。
3. 新增片段立即造成确定的 grammar contradiction：拒绝。

## 18.5 Catalog / Semantic 检查

对模型新引入的 identifier：

- 若位置应该是 table name，则必须存在于 Catalog。
- 若位置应该是 column，则必须属于当前 scope 中可见表。
- 对 `alias.column` 校验 alias binding。
- 能做类型检查时执行类型检查。

如果模型：

```sql
SELECT * FROM student s WHERE s.salary > 10
```

但 `student.salary` 不存在，则该 ghost completion 不显示。

## 18.6 最长合法前缀

如果整个 model output 不合法，可以逐 token/语法边界缩短，保留最长合法前缀。

例如模型返回：

```sql
age > 18 ORDER BY name LIMIT 10
```

而项目支持 ORDER BY、但不支持 LIMIT，则最终只保留：

```sql
age > 18 ORDER BY name
```

---

# 19. Completion 合并与排序

确定性结果优先级高于模型。

建议：

1. 当前 partial token 的 exact-prefix keyword/catalog match
2. Grammar keyword
3. Catalog table/column
4. Semantic/type-aware candidate
5. Model phrase/snippet

例如：

```sql
SELECT * FROM stu|
```

即使模型 ghost text 猜了其他东西，下拉列表也必须把真实：

```text
student
student_score
```

排在最上。

模型最适合显示为 ghost text，不要让它污染精确的 schema 下拉候选。

---

# 20. UI 行为约定

本任务只实现“SQL 输入补全”，不引入 AI 聊天 UI。

后端应支持两类输出：

## 20.1 Completion list

例如：

```text
student
student_score
```

适合 Tab/方向键选择。

## 20.2 Ghost text

例如用户已有：

```sql
SELECT department, COUNT(*)
FROM employee
```

灰字显示：

```sql
 GROUP BY department
```

### 接受方式

由现有 UI/CLI 决定，但推荐：

- `Tab`：接受当前候选/ghost text
- `Esc`：关闭
- 新输入：立即取消旧 ghost text

若当前项目只有命令行 REPL，也要把 backend completion engine 与具体 readline/terminal UI 解耦。

---

# 21. Case style

关键词大小写跟随用户。

简单策略：

统计当前 statement 中已完成 keyword：

- uppercase 占多数 -> `SELECT`, `FROM`
- lowercase 占多数 -> `select`, `from`
- 无可判断 -> 默认 uppercase

表名、列名保持 Catalog 原名/项目现有规范。

---

# 22. 注释、字符串和非法输入

requirements 明确要求 Lexer 支持注释、字符串、转义与错误位置。

补全器必须复用 Lexer 状态。

## 注释中

```sql
SELECT * FROM student -- FR|
```

不要建议 `FROM`。

## 字符串中

```sql
WHERE name = 'Al|
```

V1 默认不调用 LLM，也不做 schema completion。

未来可考虑历史值补全，但不属于本任务。

## 非法 token 后

```sql
SELECT @|
```

不要崩溃。

可以：

- 返回空 completion；
- 或仅返回 lexer recovery 之后明确安全的候选。

---

# 23. Model Context Builder

增加：

```cpp
class SqlModelContextBuilder {
public:
    ModelContext build(const CompletionContext &ctx);
};
```

## dialect context

从 `SqlCapabilities` 动态生成。

## schema context

选择相关 schema，不要全文 dump。

推荐限制：

```text
max_schema_tables = 8
max_schema_columns = 128
max_schema_chars = 8000
```

最终仍以 token budget 为准。

当库很小，可以全部加入。

当库变大：

- referenced table：必选
- alias binding 对应表：必选
- 正在输入 table prefix 的 fuzzy match：选前若干
- 其余省略

不需要 embedding/RAG/vector database。

这个项目的 schema 是结构化数据，直接从 Catalog 拿即可。

---

# 24. 配置文件

增加例如：

`config/sql_completion.json`

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

不要在代码中散落 magic numbers。

---

# 25. 建议的代码模块

根据仓库当前目录风格选择实际路径，不要为了本功能重构整个项目。

逻辑上需要：

```text
sql/autocomplete/
    completion_types.*
    completion_engine.*
    current_statement_extractor.*
    grammar_completion_provider.*
    catalog_completion_provider.*
    completion_scope.*
    sql_capabilities.*
    model_context_builder.*
    model_completion_provider.*
    model_completion_validator.*
    llama_completion_client.*
```

如果项目已有 `sql/`, `parser/`, `semantic/`, `catalog/` 等目录，应放到最接近的现有层。

另外增加：

```text
scripts/
    build_llama_p40.sh
    run_sql_completion_model.sh
```

以及测试。

---

# 26. OpenCode 实现顺序

OpenCode 必须按这个顺序做，避免先接模型再发现基础补全不可用。

## Phase A：阅读现有代码

先定位：

- Lexer
- Token/TokenType
- Parser
- grammar / LL(1) table / FIRST/FOLLOW
- AST
- semantic analyzer
- Catalog
- type system
- SQL input/REPL/UI 入口
- 当前 test framework

输出一个很短的 integration note 后直接继续实现，不需要等人工确认。

## Phase B：纯确定性补全

完成：

- CurrentStatementExtractor
- expectedTokensAtCursor
- GrammarCompletionProvider
- CatalogCompletionProvider
- CompletionScope
- CompletionEngine
- unit tests

此时完全不依赖 GPU。

## Phase C：模型 sidecar

完成：

- config
- LlamaCompletionClient
- ModelContextBuilder
- ModelCompletionProvider
- debounce/stale handling
- model unavailable fallback

## Phase D：Validator

必须在显示模型结果前完成：

- dialect filter
- parser validation
- catalog validation
- longest-valid-prefix trimming

## Phase E：接 UI/REPL

把统一 `CompletionEngine` 接入项目现有 SQL 输入组件。

不得把模型调用逻辑直接写到 UI event handler。

## Phase F：P40 scripts + README

增加：

- CUDA/P40 检查
- llama.cpp build
- model start
- health check
- curl `/infill` smoke test
- DBMS completion smoke test

---

# 27. 单元测试要求

至少覆盖以下测试。

## 27.1 Keyword

输入：

```sql
SEL|
```

包含：

```text
SELECT
```

输入：

```sql
SELECT * FR|
```

包含：

```text
FROM
```

输入：

```sql
DELETE |
```

包含：

```text
FROM
```

## 27.2 Table

Catalog：

```text
student
score
```

输入：

```sql
SELECT * FROM |
```

必须包含：

```text
student
score
```

不得返回不存在的表。

## 27.3 Column

student：

```text
id INT
name VARCHAR
age INT
```

输入：

```sql
SELECT |
FROM student
```

包含：

```text
id
name
age
```

## 27.4 Alias

```sql
SELECT s.|
FROM student s
```

只返回 student 的列。

## 27.5 WHERE

```sql
SELECT name
FROM student
WHERE |
```

返回：

- 当前 scope 的 columns
- `NOT`
- `(` 等 Parser 合法项

具体集合以实际 grammar 为准。

## 27.6 INSERT

```sql
INSERT INTO student(|
```

返回真实 columns。

## 27.7 Advanced feature gating

如果 `GROUP BY` 未实现：

```sql
GROUP |
```

不得推荐 `BY`。

实现并启用后才可以推荐。

## 27.8 Unsupported keyword

无论模型返回什么，若当前项目不支持：

```text
HAVING
LIMIT
UNION
```

不得最终显示给用户。

## 27.9 Model hallucinated column

模型返回：

```sql
salary > 100
```

但当前 scope 没有 `salary`：

结果必须被拒绝。

## 27.10 Model server down

关闭 llama-server。

所有 Grammar/Catalog completion 仍正常工作，不能卡住 SQL 输入线程。

## 27.11 Comments

```sql
SELECT * FROM student -- FRO|
```

不得补 `FROM`。

## 27.12 String

```sql
WHERE name = 'SEL|
```

不得补 `SELECT`。

## 27.13 Multi-statement

```sql
CREATE TABLE ...;
SELECT * FR|
```

只分析第二条 statement。

## 27.14 Case

```sql
select * fr|
```

优先输出：

```text
from
```

而不是强制 `FROM`。

---

# 28. 集成测试

准备一个小 Catalog：

```sql
CREATE TABLE student (
    id INT,
    name VARCHAR,
    age INT,
    class_id INT
);

CREATE TABLE score (
    student_id INT,
    score INT
);
```

测试：

```sql
SELECT s.name
FROM student s
WHERE s.|
```

应立即得到 deterministic columns。

再测试：

```sql
SELECT s.name, sc.score
FROM student s
JOIN score sc ON |
```

如果 JOIN 已启用：

模型可产生类似：

```sql
s.id = sc.student_id
```

但只有两列都真实存在才允许显示。

---

# 29. 性能指标

不要把“模型 token/s”作为主要产品指标。

真正关注：

## Deterministic completion

- p50 < 5 ms
- p95 < 20 ms

SQL 很短，这应该可达到。

## Model path

不规定一个脱离硬件实测的虚假 TTFT 数字。

实际要求：

- 不阻塞输入线程
- debounce
- stale request 丢弃
- client timeout 默认 800 ms
- 如果模型没及时返回，UI 不等待
- 模型补全是增强项，不是正确性依赖

记录 metrics：

```text
deterministic_latency_ms
model_request_latency_ms
model_timeout_count
model_rejected_count
model_accept_count
completion_items_count
```

开发环境打印日志即可，不需要引入重型 telemetry 系统。

---

# 30. P40 Benchmark

实现完成后运行：

```bash
./build/bin/llama-bench \
  -hf ggml-org/Qwen2.5-Coder-1.5B-Q8_0-GGUF:Q8_0 \
  -ngl 99 \
  -p 512 \
  -n 32
```

并额外写一个项目自己的真实 benchmark：

输入集合至少 50 个 SQL cursor position：

- SELECT
- WHERE
- INSERT
- DELETE
- CREATE
- UPDATE（若支持）
- JOIN（若支持）
- GROUP BY（若支持）
- ORDER BY（若支持）

测：

```text
prompt build time
HTTP round trip
time to first usable completion
total model completion time
validator time
accepted/rejected
```

分别比较：

```text
-fa off
-fa on
```

只有 `-fa on` 在 P40 上经过真实测试更快且稳定时才改变默认值。

---

# 31. V1 不需要微调

先不要训练模型。

Qwen2.5-Coder Base 已经具备 SQL/code FIM 能力。

本项目又有：

- 小 SQL 方言
- Parser
- Catalog
- semantic validator
- schema context

先实现 zero-training V1。

如果最终人工测试发现：

- 经常生成项目不支持的 clause
- 对本项目 SQL grammar 学得不够稳定
- JOIN/WHERE continuation 接受率低

再做 V2 微调。

---

# 32. 可选 V2：专门的 CSU-DBMS SQL FIM 微调

**本次实现不要把 V2 当阻塞项。**

数据可以完全程序化生成，因为 grammar 和 schema 都掌握在我们手中。

## 数据格式

随机生成合法 schema：

```sql
CREATE TABLE student(...);
CREATE TABLE score(...);
```

随机生成当前 DBMS 能执行的 SQL：

```sql
SELECT s.name
FROM student s
WHERE s.age > 18;
```

随机选择 cursor：

```text
prefix:
SELECT s.name
FROM student s
WHERE s.

suffix:
 > 18;

target:
age
```

或者：

```text
prefix:
SELECT s.name
FROM student s

suffix:
;

target:
WHERE s.age > 18
```

## 训练重点

只用项目支持的 grammar。

不要给模型喂大量 MySQL/PostgreSQL 独有语法，否则会反向破坏“窄 SQL 方言”的优势。

建议先做 LoRA/FIM continued training，而不是 full fine-tuning。

训练完成后 merge/convert GGUF，仍然用相同 llama.cpp `/infill` 接口，因此 DBMS 代码无需变化。

---

# 33. 失败降级规则

这个功能绝不能影响数据库基本可用性。

```text
Parser completion 出错
  -> 返回 Catalog 基础 prefix match

Catalog completion 出错
  -> 返回 Grammar keyword

llama-server down
  -> 不显示 ghost text

llama-server timeout
  -> 丢弃

模型输出 invalid
  -> 丢弃或 longest-valid-prefix

所有 provider 都失败
  -> 返回空列表
```

任何情况下：

- 不 crash
- 不阻塞 SQL 执行
- 不修改用户 SQL
- 不自动执行 completion

---

# 34. 安全边界

虽然这是本地学生项目，也应保持基本边界：

- llama-server 只监听 `127.0.0.1`
- 不需要公网暴露 8012
- 不把 SQL/schema 上传第三方 API
- 模型生成只是文本候选
- 所有执行仍走原 DBMS Parser/Semantic/Executor
- autocomplete 本身没有执行权限

---

# 35. 最终验收标准

实现完成后必须同时满足：

- [ ] 支持光标位置而不只是行尾。
- [ ] keyword completion 来源于真实 Parser grammar。
- [ ] table/column completion 来源于真实 Catalog。
- [ ] alias-aware。
- [ ] 不建议项目未实现 SQL。
- [ ] 进阶语法按 capability 自动开关。
- [ ] deterministic completion 不依赖 GPU。
- [ ] Qwen2.5-Coder-1.5B Base 通过 llama.cpp `/infill` 工作。
- [ ] P40 使用 CUDA 12.x / sm_61 构建。
- [ ] 使用 Q8_0 作为默认 quant。
- [ ] 模型调用异步/非阻塞。
- [ ] 有 debounce 和 stale response 防护。
- [ ] 模型输出经过 Parser/Catalog validator。
- [ ] 模型挂掉时数据库和基础补全不受影响。
- [ ] 有 unit test + integration test。
- [ ] 有 `scripts/run_sql_completion_model.sh`。
- [ ] 有 README 说明如何在 P40 启动模型。
- [ ] 不实现聊天/NL2SQL/Agent 功能。

---

# 36. 给 OpenCode 的最终实现指令

请直接按本文档实现，不要把功能改造成通用 AI 助手。

第一步先阅读仓库，找出现有 Lexer、Parser、语义分析、Catalog、SQL 输入入口和测试框架。

**优先复用现有编译器状态；禁止为了 autocomplete 再引入第二套 SQL parser。**

先完成纯确定性 Grammar + Catalog 补全并测试，再接 llama.cpp。

模型固定优先使用：

```text
Qwen/Qwen2.5-Coder-1.5B
```

必须是：

```text
Base / FIM
```

不要误用：

```text
Qwen2.5-Coder-1.5B-Instruct
```

生产推理默认：

```text
ggml-org/Qwen2.5-Coder-1.5B-Q8_0-GGUF
llama.cpp
CUDA 12.x
sm_61
GGML_CUDA_FORCE_MMQ=ON
context=4096
```

实现完成后执行项目全部原测试 + 新 autocomplete tests，并给出：

1. 修改文件列表；
2. 新模块说明；
3. P40 模型启动命令；
4. 自动补全 demo 命令；
5. 测试结果；
6. llama.cpp benchmark；
7. 尚未实现/被 capability gate 关闭的 SQL 功能。

---

# 37. 研究依据（截至 2026-09-14）

以下信息用于模型与推理框架决策，具体版本升级后可重新检查：

- Qwen2.5-Coder 官方：Base 模型提供 FIM 与 repository-level FIM，1.5B/3B 等多尺寸。
  - https://github.com/QwenLM/Qwen3-Coder/blob/main/examples/Qwen2.5-Coder.md
  - https://huggingface.co/Qwen/Qwen2.5-Coder-1.5B
- llama.cpp 当前 server 提供 `/infill` endpoint，并支持 `input_prefix / input_suffix / input_extra`。
  - https://github.com/ggml-org/llama.cpp/blob/master/tools/server/README.md
- llama.cpp 当前仍提供 `--fim-qwen-1.5b-default`。
  - https://github.com/ggml-org/llama.cpp/blob/master/tools/server/README.md
- llama.cpp CUDA 构建支持显式 `CMAKE_CUDA_ARCHITECTURES=61`；当前源码对 CUDA < 13 才保留旧架构路径。
  - https://github.com/ggml-org/llama.cpp/blob/master/ggml/src/ggml-cuda/CMakeLists.txt
- NVIDIA Tesla P40 是 Pascal、24GB、Compute Capability 6.1，且具有较强 INT8 能力。
  - https://developer.nvidia.com/cuda/gpus/legacy
  - https://www.nvidia.com/content/dam/en-zz/Solutions/Data-Center/tesla-product-literature/184427-Tesla-P40-Datasheet-NV-Final-Letter-Web.pdf
- 当前 vLLM NVIDIA GPU 要求高于 P40 的 6.1，因此本项目不采用 vLLM。
  - https://docs.vllm.ai/en/stable/getting_started/installation/gpu/
- 2026 当前存在 Qwen3.5-2B，但其官方模型页面以通用/多模态 conversational 推理为主，本项目不为了“版本号更新”牺牲 FIM task fit。
  - https://huggingface.co/Qwen/Qwen3.5-2B
- SQL 专用开源模型如 XiYanSQL-QwenCoder、DatA-SQL 主要面向 NL2SQL/Text-to-SQL，可用于未来数据生成/teacher 实验，但不是本项目 V1 的 cursor completion serving model。
  - https://huggingface.co/XGenerationLab/XiYanSQL-QwenCoder-3B-2504
  - https://huggingface.co/Chinastark/DatA-SQL-1.5B
