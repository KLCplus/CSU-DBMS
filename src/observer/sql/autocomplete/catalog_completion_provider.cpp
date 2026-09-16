// ------------------------------------------------------------------------------------------------
// 本文件功能索引（VSCode / Cursor：Ctrl+Click `文件:行号` 跳转）
// ------------------------------------------------------------------------------------------------
//  catalog_completion_provider.cpp:54 upper
//  catalog_completion_provider.cpp:70 starts_with_ci
//  catalog_completion_provider.cpp:85 has_symbol
//  catalog_completion_provider.cpp:91 CursorContext
//  catalog_completion_provider.cpp:103 ResolvedContext
//  catalog_completion_provider.cpp:124 resolve_context
//  catalog_completion_provider.cpp:259 add_item
//  catalog_completion_provider.cpp:289 complete_tables
//  catalog_completion_provider.cpp:319 complete_columns_of
//  catalog_completion_provider.cpp:364 complete_catalog
// ------------------------------------------------------------------------------------------------
/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/autocomplete/catalog_completion_provider.h"

#include <algorithm>
#include <cctype>

#include "common/type/attr_type.h"
#include "sql/autocomplete/sql_capabilities.h"
#include "sql/autocomplete/sql_text_scanner.h"
#include "storage/db/db.h"
#include "storage/table/table.h"
#include "storage/table/table_meta.h"

/**
 * @file catalog_completion_provider.cpp
 * @ingroup SQLAutocomplete
 * @brief 基于真实 Catalog 的表/列补全实现
 *
 * 本文件依据语句前缀与作用域推断光标意图，再从 Db/TableMeta 取出真实表名、列名与类型。
 * 核心原则：候选必须真实存在；无法确认意图时宁可不补，也不产生误导性候选。
 */

namespace {

/**
 * @brief 将字符串逐字节转大写（ASCII）
 * @param text 输入字符串
 * @return 转换后的副本
 * @details 实现原理：复制后遍历每个 char，用 std::toupper（先转 unsigned char）转换。
 */
std::string upper(const std::string &text)
{
  std::string result = text;
  for (char &c : result) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return result;
}

/**
 * @brief 大小写不敏感地判断 candidate 是否以 prefix 开头
 * @param candidate 候选完整文本
 * @param prefix 用户已输入前缀
 * @return true 表示匹配
 * @details 实现原理：prefix 更长时返回 false；否则把两者都转大写后比较前 prefix.size() 个字符。
 */
bool starts_with_ci(const std::string &candidate, const std::string &prefix)
{
  if (prefix.size() > candidate.size()) {
    return false;
  }
  return upper(candidate).compare(0, prefix.size(), upper(prefix)) == 0;
}

/**
 * @brief 判断符号集合中是否存在指定名称
 * @param symbols bison 期望 terminal 名集合
 * @param name 待查找的符号名
 * @return true 表示存在
 * @details 实现原理：对 vector 做线性查找（集合规模很小，无需哈希）。
 */
bool has_symbol(const std::vector<std::string> &symbols, const char *name)
{
  return std::find(symbols.begin(), symbols.end(), name) != symbols.end();
}

/// 光标处期望补全的语义类别
enum class CursorContext
{
  Unknown,        ///< 无法判断，交给 Grammar
  TableName,      ///< 期望表名
  ColumnName,     ///< 期望列名（含 `table.` 限定）
  InsertColumns,  ///< INSERT INTO t( 的列清单
  InsertValues,   ///< INSERT ... VALUES ( 的值
};

/**
 * @brief 光标意图解析结果
 */
struct ResolvedContext
{
  CursorContext kind = CursorContext::Unknown;  ///< 推断出的语义类别
  std::string   insert_table;                    ///< INSERT 目标表名（仅 Insert* 场景有效）
  std::string   qualifier;  ///< partial 中 "table." 的前半部分
  std::string   column_prefix;  ///< 列名前缀（可能为空）
};

/**
 * @brief 判断光标处更可能是在写表名、列名、INSERT 列清单还是 VALUES 值
 * @param context Catalog 补全上下文
 * @return 解析出的语义类别与相关表名/前缀
 * @details 实现原理：
 *          1. 若 partial 含 `.`，直接判定为 `table.` 限定列，拆出 qualifier 与 column_prefix；
 *          2. 扫描 statement_prefix，从后往前找最近的 INTO；找到后其下一个 Word 即目标表，
 *             再扫描表名之后的 token，用括号深度区分 INSERT 列清单（括号在 VALUES 之前）
 *             与 VALUES 值（括号在 VALUES 之后）；
 *          3. 否则找光标前最后一个子句关键字：VALUES->值，FROM/JOIN->表（表名后无逗号且已写表名时转为 Unknown，
 *             避免与续写子句冲突），SELECT/WHERE/ON/GROUP/ORDER/SET->列；
 *          4. 其余情况返回 Unknown，由 Grammar 尝试关键字补全。
 */
ResolvedContext resolve_context(const CatalogCompletionContext &context)
{
  ResolvedContext result;

  // "table." 形式
  const size_t dot = context.partial.find('.');
  if (dot != std::string::npos) {
    result.kind          = CursorContext::ColumnName;
    result.qualifier     = context.partial.substr(0, dot);
    result.column_prefix = context.partial.substr(dot + 1);
    return result;
  }
  result.column_prefix = context.partial;

  std::vector<SqlTextToken> tokens;
  scan_sql_text(context.statement_prefix, tokens);

  // INSERT INTO <table> ( ...  或  INSERT INTO <table> VALUES ( ...
  size_t into_index = tokens.size();
  for (size_t i = tokens.size(); i-- > 0;) {
    if (tokens[i].kind == SqlTextToken::Kind::Word && upper(tokens[i].text) == "INTO") {
      into_index = i;
      break;
    }
  }
  if (into_index < tokens.size() && into_index + 1 < tokens.size() &&
      tokens[into_index + 1].kind == SqlTextToken::Kind::Word) {
    result.insert_table = tokens[into_index + 1].text;

    bool saw_values = false;
    bool in_column_list = false;
    int  depth = 0;
    for (size_t i = into_index + 2; i < tokens.size(); ++i) {
      if (tokens[i].kind == SqlTextToken::Kind::Word && upper(tokens[i].text) == "VALUES") {
        saw_values = true;
      }
      if (tokens[i].kind == SqlTextToken::Kind::Symbol && tokens[i].text == "(") {
        if (!saw_values) {
          in_column_list = true;
        }
        ++depth;
      } else if (tokens[i].kind == SqlTextToken::Kind::Symbol && tokens[i].text == ")") {
        --depth;
        if (depth <= 0) {
          in_column_list = false;
          depth           = 0;
        }
      }
    }
    if (in_column_list) {
      result.kind = CursorContext::InsertColumns;
      return result;
    }
    if (depth > 0 && saw_values) {
      result.kind = CursorContext::InsertValues;
      return result;
    }
    result.kind = CursorContext::TableName;
    return result;
  }

  // 找到光标前最后一个子句关键字
  const char *clause_words[] = {"SELECT", "FROM", "JOIN", "WHERE", "ON", "GROUP", "ORDER", "VALUES", "SET", "UPDATE", "CREATE", "DELETE"};
  std::string last_clause;
  size_t      last_index = 0;
  for (size_t i = 0; i < tokens.size(); ++i) {
    if (tokens[i].kind == SqlTextToken::Kind::Word) {
      std::string word = upper(tokens[i].text);
      for (const char *clause : clause_words) {
        if (word == clause) {
          last_clause = clause;
          last_index  = i;
          break;
        }
      }
    }
  }

  if (last_clause == "VALUES") {
    result.kind = CursorContext::InsertValues;
    return result;
  }

  if (last_clause == "FROM" || last_clause == "JOIN") {
    // FROM 后若已经写了表名（且没有逗号结尾），则进入列上下文
    bool has_table = false;
    bool trailing_comma = false;
    for (size_t i = last_index + 1; i < tokens.size(); ++i) {
      if (tokens[i].kind == SqlTextToken::Kind::Word) {
        std::string word = upper(tokens[i].text);
        if (word == "WHERE" || word == "GROUP" || word == "ORDER" || word == "JOIN" || word == "INNER" || word == "ON") {
          break;
        }
        has_table = true;
        trailing_comma = false;
      } else if (tokens[i].kind == SqlTextToken::Kind::Symbol && tokens[i].text == ",") {
        trailing_comma = true;
      }
    }
    if (!has_table || trailing_comma) {
      result.kind = CursorContext::TableName;
      return result;
    }
    // 已经写完表名：此处更可能是子句边界（WHERE/JOIN/...）或新语句，
    // 不主动给列候选，交给 Grammar + 试探关键字。
    result.kind = CursorContext::Unknown;
    return result;
  }

  if (last_clause == "SELECT" || last_clause == "WHERE" || last_clause == "ON" || last_clause == "GROUP" ||
      last_clause == "ORDER" || last_clause == "SET") {
    result.kind = CursorContext::ColumnName;
    return result;
  }

  // 默认：如果只在输入 SELECT，则可能是语句起始关键字，由 Grammar 负责
  result.kind = CursorContext::Unknown;
  return result;
}

/**
 * @brief 生成一条候选并去重追加
 * @param out 输出候选列表
 * @param insert_text 接受后写入的文本
 * @param display 展示文本
 * @param kind 候选种类
 * @param source 候选来源
 * @param detail 详情文本（如 "student.name : VARCHAR"）
 * @param score 排序分值
 * @param replace_begin 替换区间起点
 * @param replace_end 替换区间终点
 * @return 无
 * @details 实现原理：先线性检查 out 中是否已存在相同 (insert_text, kind) 的候选，
 *          存在则直接返回；否则组装 CompletionItem 后追加。
 */
void add_item(std::vector<CompletionItem> &out, const std::string &insert_text, const std::string &display,
    CompletionKind kind, CompletionSource source, const std::string &detail, double score, size_t replace_begin,
    size_t replace_end)
{
  for (const CompletionItem &existing : out) {
    if (existing.insert_text == insert_text && existing.kind == kind) {
      return;
    }
  }
  CompletionItem item;
  item.insert_text   = insert_text;
  item.display_text  = display;
  item.kind          = kind;
  item.source        = source;
  item.detail        = detail;
  item.score         = score;
  item.replace_start = replace_begin;
  item.replace_end   = replace_end;
  out.push_back(std::move(item));
}

/**
 * @brief 补齐表名候选
 * @param context Catalog 补全上下文（提供 Db 与替换区间）
 * @param prefix 用户已输入的表名前缀（空表示不过滤）
 * @param out 输出候选列表
 * @return 无
 * @details 实现原理：db 为空直接返回；用 db->all_tables 取出全部表，
 *          对每个表做大小写不敏感前缀过滤后，以 score=20.0 的 Catalog 候选追加。
 */
void complete_tables(const CatalogCompletionContext &context, const std::string &prefix, std::vector<CompletionItem> &out)
{
  if (context.db == nullptr) {
    return;
  }
  std::vector<std::string> tables;
  context.db->all_tables(tables);
  for (const std::string &table : tables) {
    if (!prefix.empty() && !starts_with_ci(table, prefix)) {
      continue;
    }
    add_item(out, table, table, CompletionKind::Table, CompletionSource::Catalog, "table", 20.0, context.replace_begin,
        context.replace_end);
  }
}

/**
 * @brief 补齐指定表的列名候选
 * @param db 当前数据库；为空直接返回
 * @param table_name 目标表名；表不存在直接返回
 * @param prefix 列名前缀（空表示不过滤）
 * @param qualify true 时 insert_text 使用 `table.column` 限定形式
 * @param out 输出候选列表
 * @param replace_begin 替换区间起点
 * @param replace_end 替换区间终点
 * @return 无
 * @details 实现原理：从 table_meta 的 sys_field_num 开始遍历用户列（跳过系统列），
 *          对每列做大小写不敏感前缀过滤；detail 记录 `表.列 : 类型`；
 *          以 score=22.0 的 Catalog 候选追加，display 始终为裸列名。
 */
void complete_columns_of(
    Db *db, const std::string &table_name, const std::string &prefix, bool qualify, std::vector<CompletionItem> &out,
    size_t replace_begin, size_t replace_end)
{
  if (db == nullptr) {
    return;
  }
  Table *table = db->find_table(table_name.c_str());
  if (table == nullptr) {
    return;
  }
  const TableMeta &table_meta = table->table_meta();
  // 跳过 sys_field_num 之前的系统列，只补全用户定义列
  for (int i = table_meta.sys_field_num(); i < table_meta.field_num(); ++i) {
    const FieldMeta *field = table_meta.field(i);
    if (!prefix.empty() && !starts_with_ci(field->name(), prefix)) {
      continue;
    }
    std::string insert_text = field->name();
    if (qualify) {
      insert_text = table->name() + std::string(".") + field->name();
    }
    std::string detail = std::string(table->name()) + "." + field->name() + " : " + attr_type_to_sql_string(field->type());
    add_item(out, insert_text, field->name(), CompletionKind::Column, CompletionSource::Catalog, detail, 22.0, replace_begin,
        replace_end);
  }
}

}  // namespace

/**
 * @brief 依据 Catalog 与光标意图生成表/列候选
 * @param context Catalog 补全上下文
 * @param out 输出参数；候选追加到该 vector 末尾
 * @return 无
 * @details 实现原理：
 *          1. 期望集合含 "ID" 或意图为 `table.` 列名时才进入；
 *          2. 按 resolve_context 的结果分派：
 *             TableName -> complete_tables；
 *             InsertColumns -> 按 INSERT 目标表补列（不加限定）；
 *             InsertValues -> 只给 NULL / 空字符串等保守字面量；
 *             ColumnName -> 有 qualifier 时补该限定表，否则对作用域内所有表补列，
 *                           多表时自动使用限定名；
 *             Unknown -> 不做 Catalog 补全。
 */
void complete_catalog(const CatalogCompletionContext &context, std::vector<CompletionItem> &out)
{
  const bool expects_identifier = has_symbol(context.expected_symbols, "ID");
  ResolvedContext resolved     = resolve_context(context);

  // 语法上不需要 identifier，且不是 "table." 形式时，不做 Catalog 补全
  if (!expects_identifier && resolved.kind != CursorContext::ColumnName) {
    return;
  }

  switch (resolved.kind) {
    case CursorContext::TableName: {
      complete_tables(context, context.partial, out);
    } break;

    case CursorContext::InsertColumns: {
      complete_columns_of(context.db, resolved.insert_table, resolved.column_prefix, false, out, context.replace_begin,
          context.replace_end);
    } break;

    case CursorContext::InsertValues: {
      // 只给最保守的字面量候选，不制造业务数据
      const SqlCapabilities &caps = SqlCapabilities::instance();
      if (caps.null_value) {
        add_item(out, "NULL", "NULL", CompletionKind::Literal, CompletionSource::Semantic, "null value", 15.0,
            context.replace_begin, context.replace_end);
      }
      add_item(out, "''", "''", CompletionKind::Literal, CompletionSource::Semantic, "empty string", 14.0,
          context.replace_begin, context.replace_end);
    } break;

    case CursorContext::ColumnName: {
      // `table.` 形式：qualifier 已给出表名，按裸列名前缀补齐
      if (!resolved.qualifier.empty()) {
        complete_columns_of(context.db, resolved.qualifier, resolved.column_prefix, false, out, context.replace_begin,
            context.replace_end);
        break;
      }
      // 否则在当前作用域的所有表中查列；多表时插入限定名以避免歧义
      std::vector<std::string> tables = context.scope.table_names();
      const bool               qualify = tables.size() > 1;
      for (const std::string &table : tables) {
        complete_columns_of(context.db, table, resolved.column_prefix, qualify, out, context.replace_begin,
            context.replace_end);
      }
    } break;

    case CursorContext::Unknown:
    default:
      break;
  }
}
