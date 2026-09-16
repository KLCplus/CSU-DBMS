// ------------------------------------------------------------------------------------------------
// 本文件功能索引（VSCode / Cursor：Ctrl+Click `文件:行号` 跳转）
// ------------------------------------------------------------------------------------------------
//  model_context_builder.cpp:46 append_table_schema
//  model_context_builder.cpp:81 build


#include "sql/autocomplete/model_context_builder.h"

#include <sstream>

#include "common/type/attr_type.h"
#include "sql/autocomplete/sql_capabilities.h"
#include "storage/db/db.h"
#include "storage/table/table.h"
#include "storage/table/table_meta.h"

/**
 * @file model_context_builder.cpp
 * @ingroup SQLAutocomplete
 * @brief 模型上下文（dialect + schema）构造实现
 *
 * 本文件把项目真实支持的能力与真实表结构渲染为文本，随 /infill 请求注入模型。
 * 核心原则：只提供事实（能力表 + Catalog），不编造语法或表；数量受配置上限约束。
 */

/**
 * @brief 将一张表的列定义追加为 CREATE TABLE 文本
 * @param db 当前数据库；为空直接返回
 * @param table_name 表名；不存在直接返回
 * @param out 输出流
 * @return 无
 * @details 实现原理：通过 db 找到表与 TableMeta，输出 `CREATE TABLE 表(`，
 *          从 sys_field_num 开始遍历用户列，用逗号分隔列出 `列名 类型`（类型经
 *          attr_type_to_sql_string 转换），最后以 `);\n` 收尾。跳过系统列。
 */
void append_table_schema(Db *db, const std::string &table_name, std::ostringstream &out)
{
  if (db == nullptr) {
    return;
  }
  Table *table = db->find_table(table_name.c_str());
  if (table == nullptr) {
    return;
  }
  const TableMeta &table_meta = table->table_meta();
  out << "CREATE TABLE " << table->name() << " (";
  bool first = true;
  for (int i = table_meta.sys_field_num(); i < table_meta.field_num(); ++i) {
    const FieldMeta *field = table_meta.field(i);
    if (!first) {
      out << ", ";
    }
    first = false;
    out << field->name() << ' ' << attr_type_to_sql_string(field->type());
  }
  out << ");\n";
}

/**
 * @brief 构造模型上下文
 * @param context 补全上下文（提供 Db 与作用域表）
 * @param config 配置（max_schema_tables 限制注入表数）
 * @return 包含 dialect 与 schema 两段文本的 ModelContext
 * @details 实现原理：
 *          1. dialect 按能力表逐项拼接：基础语句、可选 UPDATE/JOIN/GROUP BY/ORDER BY、
 *             布尔运算与类型清单，并附「仅使用上述语法」的提示；
 *          2. schema 选择：先取作用域内表（受 max_schema_tables 限制），
 *             不足时用 Catalog 中未选过的表补齐，仍受上限；
 *          3. 对选中的每张表调用 append_table_schema 渲染，拼接为 result.schema。
 */
ModelContext SqlModelContextBuilder::build(const CompletionContext &context, const SqlCompletionConfig &config) const
{
  ModelContext result;

  const SqlCapabilities &caps = SqlCapabilities::instance();
  std::ostringstream     dialect;
  dialect << "-- CSU_DBMS_SQL_DIALECT\n"
          << "-- Supported statements:\n"
          << "-- CREATE TABLE / INSERT INTO ... VALUES ... / SELECT ... FROM ... [WHERE ...] / DELETE FROM ... [WHERE ...]\n";
  if (caps.update) {
    dialect << "-- UPDATE ... SET ... [WHERE ...]\n";
  }
  if (caps.join) {
    dialect << "-- JOIN ... ON ...\n";
  }
  if (caps.group_by) {
    dialect << "-- GROUP BY ...\n";
  }
  if (caps.order_by) {
    dialect << "-- ORDER BY ...\n";
  }
  dialect << "-- Boolean: AND OR NOT\n-- Types:";
  for (const std::string &type : caps.types) {
    dialect << ' ' << type;
  }
  dialect << "\n-- Only use syntax listed above.\n";
  result.dialect = dialect.str();

  // schema：优先当前 scope 的表，其次 catalog 中的表（受数量限制）
  std::ostringstream schema;
  std::vector<std::string> selected;
  for (const TableBinding &binding : context.scope.tables) {
    if (static_cast<int>(selected.size()) >= config.max_schema_tables) {
      break;
    }
    selected.push_back(binding.table_name);
  }
  if (context.db != nullptr && static_cast<int>(selected.size()) < config.max_schema_tables) {
    std::vector<std::string> all;
    context.db->all_tables(all);
    for (const std::string &table : all) {
      if (static_cast<int>(selected.size()) >= config.max_schema_tables) {
        break;
      }
      bool already = false;
      for (const std::string &name : selected) {
        if (name == table) {
          already = true;
          break;
        }
      }
      if (!already) {
        selected.push_back(table);
      }
    }
  }

  for (const std::string &table : selected) {
    append_table_schema(context.db, table, schema);
  }
  result.schema = schema.str();
  return result;
}
