# -*- coding: utf-8 -*-
"""CSUDB SQL 测试集用例（按类别组织）。

每个场景（scenario）的字段：
  name     场景名
  category 分类
  setup    前置 SQL 语句列表（顺序执行，失败则跳过该场景）
  checks   检查项列表：
             {name, sql, expect_rows, [expect_cols], [ordered]}
             或 {name, sql, expect_error: {type, contains, line, column}}
             或 {name, sql}  # 仅要求成功执行
"""

from . import core_sql
from . import lexer
from . import parser
from . import semantic
from . import boundary
from . import autocomplete

ALL_SCENARIOS = (
    lexer.SCENARIOS
    + parser.SCENARIOS
    + semantic.SCENARIOS
    + core_sql.SCENARIOS
    + boundary.SCENARIOS
    + autocomplete.SCENARIOS
)
