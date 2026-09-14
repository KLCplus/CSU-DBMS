# -*- coding: utf-8 -*-
"""词法分析相关能力：注释、空白、多字符运算符、字符串与转义、大小写、非法输入。"""

SCENARIOS = [
    {
        "name": "lexer_comments_and_whitespace",
        "category": "lexer",
        "setup": [
            "CREATE TABLE lx1(id int, age int);",
            "INSERT INTO lx1 VALUES (1, 18);",
            "INSERT INTO lx1 VALUES (2, 20);",
        ],
        "checks": [
            {
                "name": "single_line_comment",
                "sql": "-- single line\nselect id from lx1 where age >= 18;",
                "expect_rows": [[1], [2]],
            },
            {
                "name": "block_comment",
                "sql": "/* multi\nline */ select id from lx1 where age >= 18;",
                "expect_rows": [[1], [2]],
            },
            {
                "name": "inline_comment",
                "sql": "select id /* inline */ from lx1 where age >= 18;",
                "expect_rows": [[1], [2]],
            },
            {
                "name": "extra_whitespace",
                "sql": "  \t select   id\n from   lx1 \t where  age>=18 ;  ",
                "expect_rows": [[1], [2]],
            },
        ],
    },
    {
        "name": "lexer_operators",
        "category": "lexer",
        "setup": [
            "CREATE TABLE lx2(v int);",
            "INSERT INTO lx2 VALUES (1);",
            "INSERT INTO lx2 VALUES (2);",
            "INSERT INTO lx2 VALUES (3);",
        ],
        "checks": [
            {"name": "ge", "sql": "select v from lx2 where v >= 2;", "expect_rows": [[2], [3]]},
            {"name": "le", "sql": "select v from lx2 where v <= 2;", "expect_rows": [[1], [2]]},
            {"name": "ne", "sql": "select v from lx2 where v != 2;", "expect_rows": [[1], [3]]},
            {"name": "ne_alt", "sql": "select v from lx2 where v <> 2;", "expect_rows": [[1], [3]]},
            {"name": "eq", "sql": "select v from lx2 where v = 2;", "expect_rows": [[2]]},
            {"name": "eq_eq", "sql": "select v from lx2 where v == 2;", "expect_rows": [[2]]},
        ],
    },
    {
        "name": "lexer_strings_and_escapes",
        "category": "lexer",
        "setup": [
            "CREATE TABLE lx3(id int, s char(20));",
        ],
        "checks": [
            {"name": "insert_doubled_quote", "sql": "insert into lx3 values (1, 'O''Brien');"},
            {"name": "select_doubled_quote", "sql": "select s from lx3 where id = 1;", "expect_rows": [["O'Brien"]]},
            {"name": "insert_empty_string", "sql": "insert into lx3 values (2, '');"},
            {"name": "select_empty_string", "sql": "select s from lx3 where id = 2;", "expect_rows": [[""]]},
            {"name": "insert_backslash_escape", "sql": "insert into lx3 values (3, 'a\\\\b');"},
            {"name": "select_backslash_escape", "sql": "select s from lx3 where id = 3;", "expect_rows": [["a\\b"]]},
            {"name": "insert_double_quoted", "sql": 'insert into lx3 values (4, "dq");'},
            {"name": "select_double_quoted", "sql": "select s from lx3 where id = 4;", "expect_rows": [["dq"]]},
            {
                "name": "string_is_case_sensitive",
                "sql": "select id from lx3 where s = 'o''brien';",
                "expect_rows": [],
            },
        ],
    },
    {
        "name": "lexer_case_insensitive",
        "category": "lexer",
        "setup": [
            "CREATE TABLE lx4(id int, age int);",
            "INSERT INTO lx4 VALUES (1, 18);",
        ],
        "checks": [
            {
                "name": "mixed_case_keywords_and_identifiers",
                "sql": "SeLeCt ID FrOm LX4 WhErE AgE = 18;",
                "expect_rows": [[1]],
            },
            {
                "name": "upper_case",
                "sql": "SELECT ID FROM LX4 WHERE AGE = 18;",
                "expect_rows": [[1]],
            },
        ],
    },
    {
        "name": "lexer_illegal_input",
        "category": "lexer",
        "setup": [
            "CREATE TABLE lx5(id int, name char(10));",
            "INSERT INTO lx5 VALUES (1, 'x');",
        ],
        "checks": [
            {
                "name": "illegal_char",
                "sql": "select id from lx5 where id = 1 and @;",
                "expect_error": {"type": "SyntaxError", "contains": ["unexpected token", "expected"]},
            },
            {
                "name": "unterminated_string",
                "sql": "select id from lx5 where name = 'abc;",
                "expect_error": {"type": "SyntaxError", "contains": ["unterminated string"]},
            },
            {
                "name": "unknown_symbol",
                "sql": "select id from lx5 where id = ?;",
                "expect_error": {"type": "SyntaxError"},
            },
            {
                "name": "illegal_number",
                "sql": "select id from lx5 where id = 12abc;",
                "expect_error": {"type": "SyntaxError"},
            },
        ],
    },
    {
        # requirements.md 词法分析器「整段用例测试」的等价用例
        "name": "lexer_requirement_example",
        "category": "lexer",
        "setup": [
            "CREATE TABLE zh1(name char(10), age int);",
            "INSERT INTO zh1 VALUES ('Alice', 18);",
            "INSERT INTO zh1 VALUES ('Tom', 19);",
            "INSERT INTO zh1 VALUES ('Bob', 20);",
        ],
        "checks": [
            {
                "name": "full_snippet",
                "sql": "-- query\nSELECT name\nFROM zh1\nWHERE age >=18\n  AND name !='Tom';",
                "expect_rows": [["Alice"], ["Bob"]],
            },
        ],
    },
]
