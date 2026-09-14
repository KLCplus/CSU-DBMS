/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/autocomplete/completion_scope.h"

#include <algorithm>
#include <cctype>

#include "sql/autocomplete/sql_text_scanner.h"

namespace {

std::string upper(const std::string &text)
{
  std::string result = text;
  for (char &c : result) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return result;
}

bool is_clause_boundary(const std::string &upper_word)
{
  static const char *boundaries[] = {"WHERE", "GROUP", "ORDER", "JOIN", "INNER", "ON", "SET", "VALUES", "LIMIT"};
  for (const char *b : boundaries) {
    if (upper_word == b) {
      return true;
    }
  }
  return false;
}

}  // namespace

bool CompletionScope::contains(const std::string &table_name) const
{
  return std::any_of(tables.begin(), tables.end(), [&](const TableBinding &binding) {
    return binding.table_name == table_name;
  });
}

std::vector<std::string> CompletionScope::table_names() const
{
  std::vector<std::string> names;
  names.reserve(tables.size());
  for (const TableBinding &binding : tables) {
    names.push_back(binding.table_name);
  }
  return names;
}

void build_completion_scope(std::string_view statement_prefix, CompletionScope &scope)
{
  scope.tables.clear();

  std::vector<SqlTextToken> tokens;
  scan_sql_text(statement_prefix, tokens);

  for (size_t i = 0; i < tokens.size(); ++i) {
    if (tokens[i].kind != SqlTextToken::Kind::Word) {
      continue;
    }
    const std::string word = upper(tokens[i].text);

    // INSERT INTO <table>
    if (word == "INTO") {
      if (i + 1 < tokens.size() && tokens[i + 1].kind == SqlTextToken::Kind::Word) {
        scope.tables.push_back({tokens[i + 1].text, ""});
      }
      continue;
    }

    if (word == "FROM" || word == "JOIN") {
      // FROM 后可能跟逗号分隔的多个表；JOIN 后只跟一个表
      for (size_t j = i + 1; j < tokens.size(); ++j) {
        if (tokens[j].kind == SqlTextToken::Kind::Word) {
          const std::string word_j = upper(tokens[j].text);
          if (is_clause_boundary(word_j)) {
            break;
          }
          scope.tables.push_back({tokens[j].text, ""});
          if (word == "JOIN") {
            break;
          }
        } else if (tokens[j].kind == SqlTextToken::Kind::Symbol) {
          if (tokens[j].text == ",") {
            continue;
          }
          break;
        }
      }
    }
  }

  // 去重并保持首次出现顺序
  std::vector<TableBinding> unique;
  for (const TableBinding &binding : scope.tables) {
    if (!std::any_of(unique.begin(), unique.end(), [&](const TableBinding &existing) {
          return existing.table_name == binding.table_name;
        })) {
      unique.push_back(binding);
    }
  }
  scope.tables.swap(unique);
}
