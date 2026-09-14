/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/autocomplete/completion_types.h"

const char *completion_kind_name(CompletionKind kind)
{
  switch (kind) {
    case CompletionKind::Keyword: return "keyword";
    case CompletionKind::Table: return "table";
    case CompletionKind::Column: return "column";
    case CompletionKind::Alias: return "alias";
    case CompletionKind::Operator: return "operator";
    case CompletionKind::Type: return "type";
    case CompletionKind::Literal: return "literal";
    case CompletionKind::Snippet: return "snippet";
    case CompletionKind::Model: return "model";
  }
  return "unknown";
}

const char *completion_source_name(CompletionSource source)
{
  switch (source) {
    case CompletionSource::Grammar: return "grammar";
    case CompletionSource::Catalog: return "catalog";
    case CompletionSource::Semantic: return "semantic";
    case CompletionSource::Model: return "model";
  }
  return "unknown";
}
