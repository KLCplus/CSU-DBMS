# -*- coding: utf-8 -*-
"""语义分析相关能力：表/列存在性、名字绑定、类型一致性、INSERT 匹配、错误定位。"""

SCENARIOS = [
    {
        "name": "semantic_table_and_column",
        "category": "semantic",
        "setup": [
            "CREATE TABLE sm1(id int, name char(10), age int);",
            "INSERT INTO sm1 VALUES (1, 'a', 18);",
        ],
        "checks": [
            {
                "name": "unknown_column_with_location",
                "sql": "select nosuch from sm1;",
                "expect_error": {"type": "SemanticError", "contains": ["no such field"], "line": 1, "column": 8},
            },
            {
                "name": "unknown_table",
                "sql": "select id from no_such_table;",
                "expect_error": {"contains": ["SCHEMA_TABLE_NOT_EXIST"]},
            },
            {
                "name": "qualified_name_binding",
                "sql": "select sm1.id from sm1;",
                "expect_rows": [[1]],
            },
            {
                "name": "unqualified_name_binding",
                "sql": "select id from sm1 where age = 18;",
                "expect_rows": [[1]],
            },
        ],
    },
    {
        "name": "semantic_type_consistency",
        "category": "semantic",
        "setup": [
            "CREATE TABLE sm2(id int, name char(10));",
        ],
        "checks": [
            {
                "name": "arithmetic_type_error",
                "sql": "select id from sm2 where id + 'x' > 1;",
                "expect_error": {
                    "type": "SemanticError",
                    "contains": ["operator", "cannot be applied to", "INT", "VARCHAR"],
                    "line": 1,
                    "column": 29,
                },
            },
            {
                "name": "insert_per_column_type_mismatch",
                "sql": "insert into sm2(id, name) values ('x', 1);",
                "expect_error": {
                    "type": "TypeMismatch",
                    "contains": [
                        "sm2.id expects INT",
                        "sm2.name expects VARCHAR",
                        "VARCHAR found",
                        "INT found",
                    ],
                },
            },
            {
                "name": "insert_value_count_mismatch",
                "sql": "insert into sm2 values (1);",
                "expect_error": {"contains": ["SCHEMA_FIELD_MISSING"]},
            },
            {
                "name": "insert_unknown_column",
                "sql": "insert into sm2(nope, id) values (1, 2);",
                "expect_error": {"type": "SemanticError", "contains": ["does not exist"]},
            },
            {
                "name": "insert_single_bad_value",
                "sql": "insert into sm2 values ('bad', 'ok');",
                "expect_error": {"type": "TypeMismatch", "contains": ["sm2.id expects INT", "VARCHAR found"]},
            },
        ],
    },
]
