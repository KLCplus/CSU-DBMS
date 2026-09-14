# -*- coding: utf-8 -*-
"""语法分析相关能力：优先级、括号、语法诊断（位置 + 实际符号 + 期望集合）。"""

SCENARIOS = [
    {
        "name": "parser_precedence",
        "category": "parser",
        "setup": [
            "CREATE TABLE pp1(a int, b int, c int);",
            "INSERT INTO pp1 VALUES (1, 1, 0);",
            "INSERT INTO pp1 VALUES (1, 0, 0);",
            "INSERT INTO pp1 VALUES (0, 1, 1);",
            "INSERT INTO pp1 VALUES (0, 0, 0);",
        ],
        "checks": [
            # AND 优先级高于 OR：a=1 OR (b=1 AND c=1)
            {
                "name": "and_over_or_implicit",
                "sql": "select a, b from pp1 where a = 1 or b = 1 and c = 1;",
                "expect_rows": [[1, 1], [1, 0], [0, 1]],
            },
            {
                "name": "and_over_or_explicit",
                "sql": "select a, b from pp1 where a = 1 or (b = 1 and c = 1);",
                "expect_rows": [[1, 1], [1, 0], [0, 1]],
            },
            # 括号改变结合： (a=1 OR b=1) AND c=1
            {
                "name": "paren_changes_grouping",
                "sql": "select a, b from pp1 where (a = 1 or b = 1) and c = 1;",
                "expect_rows": [[0, 1]],
            },
            # NOT 优先级高于 AND：(NOT a=1) AND b=1
            {
                "name": "not_over_and",
                "sql": "select a, b from pp1 where not a = 1 and b = 1;",
                "expect_rows": [[0, 1]],
            },
            {
                "name": "not_parenthesized",
                "sql": "select a, b from pp1 where not (a = 1 and b = 1);",
                "expect_rows": [[1, 0], [0, 1], [0, 0]],
            },
            # AND 优先级高于 OR 的另一侧
            {
                "name": "and_before_or_left",
                "sql": "select a, b from pp1 where a = 1 and b = 1 or c = 1;",
                "expect_rows": [[1, 1], [0, 1]],
            },
            {
                "name": "three_way_or",
                "sql": "select a, b from pp1 where a = 1 or b = 1 or c = 1;",
                "expect_rows": [[1, 1], [1, 0], [0, 1]],
            },
        ],
    },
    {
        "name": "parser_syntax_diagnostics",
        "category": "parser",
        "setup": [
            "CREATE TABLE pd1(a int);",
        ],
        "checks": [
            {
                "name": "missing_operand_reports_expected",
                "sql": "select a from pd1 where a = 1 and;",
                "expect_error": {
                    "type": "SyntaxError",
                    "contains": ["unexpected token: SEMICOLON", "expected:"],
                    "line": 1,
                    "column": 34,
                },
            },
            {
                "name": "multiline_error_location",
                "sql": "select a\nfrom pd1\nwhere a = 1 and;",
                "expect_error": {
                    "type": "SyntaxError",
                    "contains": ["unexpected token: SEMICOLON", "expected:"],
                    "line": 3,
                    "column": 16,
                },
            },
            {
                "name": "unbalanced_left_paren",
                "sql": "select a from pd1 where (a = 1;",
                "expect_error": {
                    "type": "SyntaxError",
                    "contains": ["expected:"],
                    "line": 1,
                    "column": 31,
                },
            },
            {
                "name": "extra_right_paren",
                "sql": "select a from pd1 where a = 1);",
                "expect_error": {
                    "type": "SyntaxError",
                    "contains": ["unexpected token: RBRACE"],
                    "line": 1,
                    "column": 30,
                },
            },
            {
                "name": "missing_expression_after_select",
                "sql": "select from pd1;",
                "expect_error": {
                    "type": "SyntaxError",
                    "contains": ["unexpected token: FROM", "expected:"],
                    "line": 1,
                    "column": 8,
                },
            },
        ],
    },
]
