/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include <algorithm>
#include <string>
#include <vector>

#include "gtest/gtest.h"

#include "sql/autocomplete/completion_engine.h"
#include "sql/autocomplete/current_statement_extractor.h"
#include "sql/autocomplete/grammar_completion_provider.h"
#include "sql/autocomplete/model_completion_validator.h"
#include "sql/autocomplete/sql_capabilities.h"
#include "sql/autocomplete/sql_text_scanner.h"
#include "sql/parser/expected_tokens.h"

namespace {

bool contains(const std::vector<CompletionItem> &items, const std::string &text)
{
  return std::any_of(items.begin(), items.end(), [&](const CompletionItem &item) { return item.insert_text == text; });
}

CompletionResponse complete_text(const std::string &sql, size_t cursor = std::string::npos)
{
  CompletionEngine   engine;
  CompletionRequest  request;
  request.sql                   = sql;
  request.cursor_offset         = cursor == std::string::npos ? sql.size() : cursor;
  request.want_model_completion = false;
  return engine.complete(nullptr, request);
}

}  // namespace

TEST(AutocompleteScannerTest, comments_and_strings)
{
  EXPECT_FALSE(cursor_in_string_or_comment("SELECT * FROM t", 14));
  EXPECT_TRUE(cursor_in_string_or_comment("SELECT * FROM t -- FRO", 21));
  EXPECT_TRUE(cursor_in_string_or_comment("SELECT 'abc", 11));
  EXPECT_FALSE(cursor_in_string_or_comment("SELECT 'abc' ", 13));
  EXPECT_TRUE(cursor_in_string_or_comment("/* c */ SELECT", 4));
}

TEST(AutocompleteExtractorTest, statement_at_cursor)
{
  const std::string sql = "CREATE TABLE t(a int); SELECT * FR";
  StatementSlice    slice = find_statement_at_cursor(sql, sql.size());
  EXPECT_TRUE(slice.found);
  EXPECT_EQ(sql.substr(slice.begin), " SELECT * FR");
}

TEST(AutocompleteExtractorTest, semicolon_in_string_is_not_separator)
{
  const std::string sql = "SELECT ';' FROM t";
  StatementSlice    slice = find_statement_at_cursor(sql, sql.size());
  EXPECT_EQ(slice.begin, 0u);
  EXPECT_EQ(slice.end, sql.size());
}

TEST(AutocompleteCapabilitiesTest, forbidden_and_supported)
{
  const SqlCapabilities &caps = SqlCapabilities::instance();
  EXPECT_TRUE(caps.is_keyword("SELECT"));
  EXPECT_TRUE(caps.is_keyword("OR"));
  EXPECT_TRUE(caps.is_type("INT"));
  EXPECT_TRUE(caps.is_type("CHAR"));
  EXPECT_FALSE(caps.is_forbidden("WHERE"));
  EXPECT_TRUE(caps.is_forbidden("HAVING"));
  EXPECT_TRUE(caps.is_forbidden("LIMIT"));
  EXPECT_TRUE(caps.is_forbidden("UNION"));
  EXPECT_TRUE(caps.is_forbidden("VARCHAR"));  // 项目用 CHAR，不推荐 VARCHAR
}

TEST(AutocompleteParserTest, expected_tokens_reused_from_parser)
{
  std::vector<std::string> tokens;
  int                      line = 0;
  int                      col  = 0;
  int                      n    = collect_expected_tokens("SELECT * \x01", tokens, line, col);
  EXPECT_GT(n, 0);
  EXPECT_NE(std::find(tokens.begin(), tokens.end(), "FROM"), tokens.end());
}

TEST(AutocompleteGrammarTest, keyword_from_expected_symbols)
{
  GrammarCompletionContext context;
  context.expected_symbols = {"SELECT"};
  std::vector<CompletionItem> items;
  complete_grammar(context, items);
  EXPECT_TRUE(contains(items, "SELECT"));

  context.expected_symbols = {"STRING_T"};
  items.clear();
  complete_grammar(context, items);
  EXPECT_TRUE(contains(items, "CHAR"));
}

TEST(AutocompleteEngineTest, keyword_completion)
{
  EXPECT_TRUE(contains(complete_text("SEL").items, "SELECT"));
  EXPECT_TRUE(contains(complete_text("SELECT * FR").items, "FROM"));
  EXPECT_TRUE(contains(complete_text("DELET").items, "DELETE"));
  // WHERE 之后应能补 NOT / NULL
  auto where_items = complete_text("SELECT name FROM student WHERE ").items;
  EXPECT_TRUE(contains(where_items, "NOT"));
  EXPECT_TRUE(contains(where_items, "NULL"));
}

TEST(AutocompleteEngineTest, case_style_follows_user)
{
  auto lower_items = complete_text("select * fr").items;
  EXPECT_TRUE(contains(lower_items, "from"));
}

TEST(AutocompleteEngineTest, group_by_capability_gate)
{
  // 本项目已实现 GROUP BY，因此在 GROUP 后推荐 BY（须位于合法语句前缀中）
  EXPECT_TRUE(contains(complete_text("SELECT a FROM t GROUP ").items, "BY"));
}

TEST(AutocompleteEngineTest, no_completion_in_comment_or_string)
{
  EXPECT_TRUE(complete_text("SELECT * FROM t -- FRO").items.empty());
  EXPECT_TRUE(complete_text("WHERE name = 'SEL").items.empty());
}

TEST(AutocompleteEngineTest, multi_statement_only_second)
{
  auto items = complete_text("CREATE TABLE t(a int); SELECT * FR").items;
  EXPECT_TRUE(contains(items, "FROM"));
}

TEST(AutocompleteModelValidatorTest, cleans_markdown_and_blocks_dialect)
{
  ModelCompletionValidator validator;
  CompletionContext        context;
  context.statement_prefix = "SELECT * FROM student ";

  ValidationResult result = validator.validate(context, "```sql\n WHERE \n```");
  EXPECT_TRUE(result.accepted);
  EXPECT_EQ(result.text, "WHERE");

  // LIMIT 不被支持，应被截断
  result = validator.validate(context, " ORDER BY name LIMIT 10");
  EXPECT_TRUE(result.accepted);
  EXPECT_EQ(result.text, "ORDER BY name");

  // 纯禁用关键字 -> 拒绝
  result = validator.validate(context, "LIMIT 10");
  EXPECT_FALSE(result.accepted);
}
