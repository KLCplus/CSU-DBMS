# -*- coding: utf-8 -*-
"""必备能力：CREATE / INSERT / SELECT / DELETE / WHERE / 比较 / AND / OR / NOT / 括号。"""

SCENARIOS = [
    {
        "name": "core_create_insert_select",
        "category": "core",
        "setup": [
            "CREATE TABLE c1(id int, name char(20), age int);",
            "INSERT INTO c1 VALUES (1, 'Alice', 20);",
            "INSERT INTO c1 VALUES (2, 'Bob', 18);",
            "INSERT INTO c1 VALUES (3, 'Cindy', 22);",
            "INSERT INTO c1 VALUES (4, 'Dave', 18);",
        ],
        "checks": [
            {
                "name": "select_star",
                "sql": "select * from c1;",
                "expect_cols": ["id", "name", "age"],
                "expect_rows": [[1, "Alice", 20], [2, "Bob", 18], [3, "Cindy", 22], [4, "Dave", 18]],
            },
            {
                "name": "project_and_where",
                "sql": "select id, name from c1 where age = 18;",
                "expect_cols": ["id", "name"],
                "expect_rows": [[2, "Bob"], [4, "Dave"]],
            },
            {
                "name": "project_single_column",
                "sql": "select name from c1;",
                "expect_rows": [["Alice"], ["Bob"], ["Cindy"], ["Dave"]],
            },
            {"name": "empty_result", "sql": "select id from c1 where id = 99;", "expect_rows": []},
            {"name": "insert_full_row", "sql": "insert into c1 values (5, 'Eve', 25);"},
            {
                "name": "select_inserted_row",
                "sql": "select id, age from c1 where id = 5;",
                "expect_rows": [[5, 25]],
            },
            {"name": "insert_column_list", "sql": "insert into c1(name, id) values ('Zoe', 6);"},
            {
                "name": "omitted_column_is_null",
                "sql": "select name, age from c1 where id = 6;",
                "expect_cols": ["name", "age"],
                "expect_rows": [["Zoe", "null"]],
            },
        ],
    },
    {
        "name": "core_where_comparison",
        "category": "core",
        "setup": [
            "CREATE TABLE c2(id int, v int);",
            "INSERT INTO c2 VALUES (1, 10);",
            "INSERT INTO c2 VALUES (2, 20);",
            "INSERT INTO c2 VALUES (3, 30);",
            "INSERT INTO c2 VALUES (4, 20);",
        ],
        "checks": [
            {"name": "eq", "sql": "select id from c2 where v = 20;", "expect_rows": [[2], [4]]},
            {"name": "eq_eq", "sql": "select id from c2 where v == 20;", "expect_rows": [[2], [4]]},
            {"name": "ne", "sql": "select id from c2 where v != 20;", "expect_rows": [[1], [3]]},
            {"name": "ne_alt", "sql": "select id from c2 where v <> 20;", "expect_rows": [[1], [3]]},
            {"name": "lt", "sql": "select id from c2 where v < 20;", "expect_rows": [[1]]},
            {"name": "le", "sql": "select id from c2 where v <= 20;", "expect_rows": [[1], [2], [4]]},
            {"name": "gt", "sql": "select id from c2 where v > 20;", "expect_rows": [[3]]},
            {"name": "ge", "sql": "select id from c2 where v >= 20;", "expect_rows": [[2], [3], [4]]},
            {"name": "string_compare", "sql": "select id from c2 where v > 10 and v < 30;", "expect_rows": [[2], [4]]},
        ],
    },
    {
        "name": "core_logical_and_or_not_paren",
        "category": "core",
        "setup": [
            "CREATE TABLE c3(a int, b int, name char(10));",
            "INSERT INTO c3 VALUES (1, 1, 'x');",
            "INSERT INTO c3 VALUES (1, 0, 'y');",
            "INSERT INTO c3 VALUES (0, 1, 'x');",
            "INSERT INTO c3 VALUES (0, 0, 'y');",
        ],
        "checks": [
            {"name": "and", "sql": "select a, b from c3 where a = 1 and b = 1;", "expect_rows": [[1, 1]]},
            {
                "name": "and_three",
                "sql": "select a, b from c3 where a = 1 and b = 0 and name = 'y';",
                "expect_rows": [[1, 0]],
            },
            {
                "name": "or",
                "sql": "select a, b from c3 where a = 1 or b = 1;",
                "expect_rows": [[1, 1], [1, 0], [0, 1]],
            },
            {
                "name": "not",
                "sql": "select a, b from c3 where not a = 1;",
                "expect_rows": [[0, 1], [0, 0]],
            },
            {
                "name": "not_parenthesized",
                "sql": "select a, b from c3 where not (a = 1);",
                "expect_rows": [[0, 1], [0, 0]],
            },
            {
                "name": "paren_grouping",
                "sql": "select a, b from c3 where (a = 1 or b = 1) and name = 'x';",
                "expect_rows": [[1, 1], [0, 1]],
            },
            {
                "name": "precedence_and_over_or",
                "sql": "select a, b from c3 where a = 1 or b = 1 and name = 'y';",
                "expect_rows": [[1, 1], [1, 0]],
            },
            {
                "name": "precedence_not_over_and",
                "sql": "select a, b from c3 where not a = 1 and b = 1;",
                "expect_rows": [[0, 1]],
            },
            {
                "name": "nested_paren",
                "sql": "select a, b from c3 where ((a = 1) and (b = 1)) or (a = 0 and b = 0);",
                "expect_rows": [[1, 1], [0, 0]],
            },
        ],
    },
    {
        "name": "core_delete",
        "category": "core",
        "setup": [
            "CREATE TABLE c4(id int, name char(10));",
            "INSERT INTO c4 VALUES (1, 'a');",
            "INSERT INTO c4 VALUES (2, 'b');",
            "INSERT INTO c4 VALUES (3, 'c');",
        ],
        "checks": [
            {"name": "delete_one", "sql": "delete from c4 where id = 2;"},
            {"name": "verify_delete_one", "sql": "select id from c4;", "expect_rows": [[1], [3]]},
            {"name": "delete_no_match", "sql": "delete from c4 where id = 99;"},
            {"name": "verify_no_match", "sql": "select id from c4;", "expect_rows": [[1], [3]]},
            {"name": "delete_all", "sql": "delete from c4;"},
            {"name": "verify_delete_all", "sql": "select id from c4;", "expect_rows": []},
            {"name": "delete_empty_table", "sql": "delete from c4 where id = 1;"},
        ],
    },
]
