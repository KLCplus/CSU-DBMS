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

namespace {

std::string upper(const std::string &text)
{
  std::string result = text;
  for (char &c : result) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return result;
}

bool starts_with_ci(const std::string &candidate, const std::string &prefix)
{
  if (prefix.size() > candidate.size()) {
    return false;
  }
  return upper(candidate).compare(0, prefix.size(), upper(prefix)) == 0;
}

bool has_symbol(const std::vector<std::string> &symbols, const char *name)
{
  return std::find(symbols.begin(), symbols.end(), name) != symbols.end();
}

enum class CursorContext
{
  Unknown,
  TableName,
  ColumnName,
  InsertColumns,
  InsertValues,
};

struct ResolvedContext
{
  CursorContext kind = CursorContext::Unknown;
  std::string   insert_table;
  std::string   qualifier;  ///< partial 中 "table." 的前半部分
  std::string   column_prefix;
};

// 判断光标处更可能是在写表名、列名、INSERT 列清单还是 VALUES 值
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
      if (!resolved.qualifier.empty()) {
        complete_columns_of(context.db, resolved.qualifier, resolved.column_prefix, false, out, context.replace_begin,
            context.replace_end);
        break;
      }
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
