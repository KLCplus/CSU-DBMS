# -*- coding: utf-8 -*-
"""边界测试：空输入、极长标识符、多语句、空白与分号、数值边界。"""

SCENARIOS = [
    {
        "name": "boundary_inputs",
        "category": "boundary",
        "setup": [
            "CREATE TABLE bd1(id int, name char(10));",
            "INSERT INTO bd1 VALUES (1, 'a');",
            "INSERT INTO bd1 VALUES (2, 'b');",
        ],
        "checks": [
            {
                "name": "empty_input_no_crash",
                "sql": "",
                "expect_error": {"contains": ["INTERNAL"]},
            },
            {
                "name": "whitespace_only_no_crash",
                "sql": "   \t  ",
                "expect_error": {},
            },
            {
                "name": "very_long_identifier_no_crash",
                "sql": "select " + ("a" * 300) + " from bd1;",
                "expect_error": {"contains": ["no such field"]},
            },
            {
                "name": "multiple_statements_first_executed",
                "sql": "select id from bd1; select name from bd1;",
                "expect_rows": [[1], [2]],
            },
            {
                "name": "statement_without_semicolon",
                "sql": "select id from bd1",
                "expect_rows": [[1], [2]],
            },
            {
                "name": "leading_trailing_whitespace_and_newlines",
                "sql": "\n\tselect id\nfrom bd1\nwhere id = 1;\n",
                "expect_rows": [[1]],
            },
            {
                "name": "space_before_semicolon",
                "sql": "select id from bd1 where id = 1 ;",
                "expect_rows": [[1]],
            },
            {
                "name": "no_matching_rows",
                "sql": "select id from bd1 where id = 0;",
                "expect_rows": [],
            },
        ],
    },
    {
        "name": "boundary_numeric",
        "category": "boundary",
        "setup": [
            "CREATE TABLE bn1(v int);",
            "INSERT INTO bn1 VALUES (0);",
        ],
        "checks": [
            {"name": "zero", "sql": "select v from bn1 where v = 0;", "expect_rows": [[0]]},
            {"name": "insert_max_int", "sql": "insert into bn1 values (2147483647);"},
            {
                "name": "select_max_int",
                "sql": "select v from bn1 where v = 2147483647;",
                "expect_rows": [[2147483647]],
            },
            {
                "name": "select_gt_zero",
                "sql": "select v from bn1 where v > 0;",
                "expect_rows": [[2147483647]],
            },
        ],
    },
]
