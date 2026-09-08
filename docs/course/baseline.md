# CSU-DBMS Baseline v0.1 Record

> 本文前半部分保留基座整理时的历史验收快照；当前 CSU-DBMS CLI、OS Page/Buffer Pool 实验与最新结果分别见第 12 节、`cli.md` 和 `os_storage.md`。

## 1. Revision

- Upstream repository: `https://github.com/oceanbase/miniob`
- Upstream base commit: `3259c37` (`fix: propagate LOAD DATA error status instead of returning false SUCCESS (#646)`)
- Working branch: `main`
- Baseline identifier: Git tag `minidb-baseline-v0.1`（发布提交后创建）
- Verification date: 2026-09-07 (Asia/Shanghai)

## 2. Environment

| Component | Verified value |
| --- | --- |
| OS | Ubuntu 24.04.4 LTS, Linux 7.0.0-28-generic x86_64 |
| GCC / G++ | 13.3.0 |
| CMake | 3.28.3 |
| Flex | 2.6.4 |
| Bison | 3.8.2 |
| GDB | 15.1 |
| C++ standard | C++20 (`CMAKE_CXX_STANDARD 20`) |
| Build type | Debug, ASAN enabled by upstream default |

Flex/Bison 最初不在系统 PATH 中，第一次 CMake 配置在 `find_package(FLEX REQUIRED)` 失败；安装 Flex 2.6.4 和 Bison 3.8.2 后成功。这是环境依赖，不是源码问题。

## 3. Dependency initialization and build

实际执行：

```bash
./build.sh init
./build.sh clean
./build.sh debug --make -j4
```

结果：成功。从空的 `build_debug` 重新配置和编译后生成：

- `build_debug/bin/observer`
- `build_debug/bin/obclient`
- `build_debug/bin/clog_dump`
- `build_debug/lib/libobserver.a`
- 默认启用的 46 个 CTest 测试目标

构建期间只有 `src/observer/storage/common/column.cpp` 的 signed/unsigned comparison warning；顶层 CMake 已设置 `-Wno-error=sign-compare`，不影响产物。本次按“问题记录、不改核心实现”原则未处理。

## 4. Observer startup

为避免测试数据进入仓库，实际使用隔离目录：

```bash
cd /tmp/minidb-baseline-final.lAod8n
/home/konglingchen/code/DBMS/miniob/build_debug/bin/observer \
  -f /home/konglingchen/code/DBMS/miniob/etc/observer.ini \
  -P cli
```

历史结果：成功加载配置并进入交互提示符，退出后进程返回 0。当前入口与提示符见第 12 节。

可移植写法见项目 README：将仓库绝对路径替换为本机路径即可。observer 会在**启动时的当前工作目录**下创建 `miniob/db/sys`。

## 5. SQL demo and persistence

实际执行：

```sql
CREATE TABLE student (id INT, name CHAR(20));
INSERT INTO student VALUES (1, 'Alice');
INSERT INTO student VALUES (2, 'Bob');
SELECT * FROM student;
SELECT name FROM student WHERE id = 1;
DELETE FROM student WHERE id = 1;
SELECT * FROM student;
```

实际结果摘要：

```text
CREATE/INSERT/INSERT: SUCCESS

SELECT *:
id | name
1 | Alice
2 | Bob

SELECT name WHERE id = 1:
name
Alice

DELETE: SUCCESS

SELECT * after DELETE:
id | name
2 | Bob
```

随后退出 observer，在同一个数据目录用相同命令重启，再执行：

```sql
SELECT * FROM student;
```

仍返回 `2 | Bob`，因此表元数据、记录删除和剩余记录的磁盘持久化验证成功。测试目录中实际生成 `student.table`、`student.data`、`sys.db`、`dblwr.db`、`clog/` 和 `lsm/` 等存储内容。

## 6. Tests

执行命令：

```bash
cd build_debug
ctest --output-on-failure -j4
```

最终结果：46 个测试中 44 个通过，2 个失败（96%）。失败项也分别串行复现，不是 `ctest -j4` 的测试间资源竞争：

1. `bplus_tree_log_test`
   - `BplusTreeLog.base` 通过。
   - `BplusTreeLog.concurrency` 在 `storage/index/latch_memo.cpp` 断言 `frame->pin_count() == 1` 失败。
2. `mvcc_trx_log_test`
   - 在 `MvccTrxLog.wal` 的并发路径出现 `RECORD_NOMEM`/无效 slot，ASAN 报告 heap-use-after-free，随后在 `mvcc_trx.cpp` 触发提交断言；单独复现时也曾在 `frame.cpp` 触发 latch owner 断言。

这两个测试在任何项目文件修改前的上游基线构建中已经失败；裁剪后结果一致。由于用户要求“记录问题，先不修改”，且它们属于 Advanced/Reserved 的并发 B+Tree/MVCC/WAL 范围，本次没有改 B+Tree、Frame latch、MVCC 或测试逻辑。基础 SQL、Parser、Catalog、Buffer、Record 和多数日志测试均通过。

## 7. Preserved modules

### A. Core

- `src/observer/sql/parser`, `stmt`, `expr`, `optimizer`, `operator`, `executor`
- `src/observer/catalog`
- `src/observer/storage/db`, `table`, `record`, `field`, `buffer`, `common`, `persist`
- `src/observer/main.cpp`, `session`, `event`, `net`, `common`
- `src/obclient`, `test`, `unittest` 的核心/上游测试体系

### B. Advanced / Reserved

- `storage/index`（B+Tree、IndexScan、index WAL）
- `storage/trx`（Vacuous/MVCC/LSM）
- `storage/clog`（WAL/Redo/Recovery）
- Double Write Buffer
- Cascade Optimizer、统计信息、向量化、PAX、LSM
- `benchmark` 中与 Record、B+Tree、执行、并发有关的基准
- `tools/clog_dump`

### C. Infrastructure

- Session/Event/Net/Common
- Docker/Dev Container
- 上游文档和构建脚本

## 8. Removed / excluded

### Removed: `src/cpplings`

原因：这是独立 C++ 语言练习集合。依赖扫描只发现顶层 `WITH_CPPLINGS` 和 `src/CMakeLists.txt` 的条件子目录；observer、obclient、oblsm、核心库和核心 tests 均不 include 或链接它。同步移除了 CMake 选项/子目录入口和上游文档中的失效链接。

可能影响：不能再通过本仓库构建 `build_debug/bin/cpplings/*` 教学练习程序。不会影响 MiniDB 请求链。

### Removed: `src/memtracer`

原因：这是通过 `LD_PRELOAD` 使用的独立内存监控共享库，`WITH_MEMTRACER` 默认 OFF，且与 ASAN 不兼容。依赖扫描显示核心 observer 不链接/包含它，只有专属 unittest、benchmark 和文档引用。

同步删除：

- `unittest/memtracer/`
- `benchmark/memtracer_performance_test.cpp`
- `docs/docs/game/miniob-memtracer.md` 及 MkDocs 导航入口
- `WITH_MEMTRACER` 和相关 CMake 子目录

可能影响：不能再构建/预加载 `libmemtracer.so`，也不能运行其专属单测与性能测试。ASAN Debug 构建仍保留，后续 OS I/O/Page Trace 计划不依赖 MemTracer。

### Removed: repository governance text files

按课程仓库维护方要求删除根目录 `CODE_OF_CONDUCT.md` 和 `CONTRIBUTING.md`，它们不参与编译或运行。

`License` 与 `NOTICE` 已在 CSU-DBMS CLI 整理阶段恢复。公开分发派生代码时必须提供许可证副本并保留原有版权、专利、商标及免责声明；产品界面无需使用上游品牌。

### Not removed

- `src/oblsm`：`observer_static` 直接链接 `oblsm`，`Db::init` 会创建 LSM 实例；删除会破坏构建/启动。
- `benchmark` 其余文件：与后续 B+Tree、Buffer/Record 和执行性能实验相关。
- `docker`, `.devcontainer`：不参与 observer 链接，但有助于课程开发环境复现。
- B+Tree、Transaction、CLog、Recovery、tests：未来扩展价值高。

## 9. Source changes

- Core implementation changes: none.
- Build changes: 只移除 Cpplings/MemTracer 的选项和子目录入口。
- Documentation changes: 重写 README；新增 grammar、architecture、source map 和本记录；清理已删除模块的旧文档引用。
- File format/protocol changes: none.

## 10. Known issues and gaps

1. 当前 Flex/Bison 语法只支持 WHERE 中以 `AND` 串联比较条件，没有 `OR`/`NOT` token 与产生式；详见 `grammar.md`。
2. 默认 Debug 构建下 `bplus_tree_log_test` 和 `mvcc_trx_log_test` 存在上游并发断言失败，详见 Tests。
3. `./build.sh init` 会将 libevent/jsoncpp 子模块 checkout 到脚本指定的兼容 revision，因此初始化后 superproject 可能显示子模块工作树 revision 变化；这属于上游脚本行为。
4. 默认 `ENABLE_ASAN=ON`；适合 Debug，但运行结果和性能不应作为 Release benchmark。
5. `Catalog` 当前主要持有内存中的 `TableStats`；schema metadata 主要由 `Db/TableMeta/FieldMeta/IndexMeta` 提供，不应把 `Catalog` 单例误认为完整持久化 Data Dictionary。
6. README 的运行命令使用 `/tmp` 隔离数据；如果从仓库根启动，会产生仓库内运行数据，应避免提交。
7. 根目录 `License`/`NOTICE` 已恢复并保留；不得再以“去品牌”为理由删除源码版权头或开源许可文本。

## 11. Next development notes

- 第一阶段观测优先接入 `SqlTaskHandler`/各 Stage 边界，以只读方式输出 Token、Parsed SQL、Stmt 和 Plan。
- AST/Plan 可视化先实现 dump/visitor，不改变 `unique_ptr` 所有权。
- Buffer/Page Trace 从 `DiskBufferPool::get_this_page`, `allocate_page`, `unpin_page`, `load_page`, `write_page` 开始。
- 修复 Advanced 并发测试前，应先用 `CONCURRENCY` 配置矩阵复现并理解 Frame pin/latch ownership；不要通过删除断言掩盖问题。
- 每个阶段都维持 `Debug build + unit tests + CLI SQL + restart persistence` 回归门槛。


## 12. CSU-DBMS CLI packaging verification (2026-09-08)

本次将用户入口包装为 CSU-DBMS 自有 CLI，核心数据库架构和文件格式未重写。

实际构建命令：

```bash
./build.sh debug --make -j4
```

构建成功，新增产物 `build_debug/bin/csudb` 与 `build_debug/bin/csudb-client`；`observer`、`obclient` 作为旧测试兼容入口保留。`./csudb --version` 返回 `CSU-DBMS 0.1.0`，`./csudb --help` 可正常显示长选项。

在 `/tmp/csudb_cli_verify_20260908` 中实际执行：

```sql
help;
CREATE TABLE student (id INT, name CHAR(20));
INSERT INTO student VALUES (1, 'Alice');
INSERT INTO student VALUES (2, 'Bob');
SELECT * FROM student;
SELECT name FROM student WHERE id = 1;
DELETE FROM student WHERE id = 1;
SELECT * FROM student;
```

欢迎页、`csudb >` 提示符、内置帮助和全部 SQL 均成功；删除后只剩 `2 | Bob`。退出并用 `./csudb` 在相同工作目录重启后再次查询，仍返回 Bob，持久化通过。数据目录为 `csudb_data/db/sys`，历史文件为 `.csudb_history`。

完整测试命令：

```bash
ctest --test-dir build_debug --output-on-failure
```

结果为 47 项中 45 项通过（96%）。失败项仍是历史基线已记录的 `bplus_tree_log_test` 并发 pin-count 断言和 `mvcc_trx_log_test` ASAN use-after-free/提交断言；`buffer_pool_os_test` 通过。本次没有修改对应 Advanced/Reserved 核心实现。

品牌边界：所有正式 SQL、网络和 LSM CLI 的欢迎语、提示符、历史文件和默认数据/日志路径均已切换为 CSU-DBMS。源码中的 `MiniobLineReader`、`observer` 静态库名、`oceanbase` namespace 等内部兼容标识暂不进行大规模重命名，以免破坏 ABI 和成熟核心；上游版权头、`License`、`NOTICE` 因开源许可要求必须保留。
