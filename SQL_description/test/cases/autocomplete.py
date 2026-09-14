# -*- coding: utf-8 -*-
"""SQL 输入补全（autocomplete）集成测试。

这些 check 使用 `complete` 字段，通过 native 协议的 complete 请求验证：
  - 关键字补全来自真实 Parser grammar
  - 表 / 列补全来自真实 Catalog
  - 进阶语法按 capability 开关
  - 注释 / 字符串 / 多语句 / 大小写 行为
"""

SCENARIOS = [
    {
        "name": "autocomplete_keyword_and_catalog",
        "category": "autocomplete",
        "setup": [
            "CREATE TABLE student(id int, name char(20), age int, class_id int);",
            "CREATE TABLE score(student_id int, score int);",
        ],
        "checks": [
            {
                "name": "keyword_select",
                "sql": "SELECT * FR",
                "complete": {"sql": "SELECT * FR", "contains": ["FROM"]},
            },
            {
                "name": "keyword_delete_from",
                "sql": "DELETE ",
                "complete": {"sql": "DELETE ", "contains": ["FROM"]},
            },
            {
                "name": "tables_after_from",
                "sql": "SELECT * FROM ",
                "complete": {"sql": "SELECT * FROM ", "contains": ["student", "score"]},
            },
            {
                "name": "columns_in_select",
                "sql": "SELECT  FROM student",
                "complete": {"sql": "SELECT  FROM student", "cursor": 7, "contains": ["id", "name", "age"]},
            },
            {
                "name": "columns_in_where",
                "sql": "SELECT name FROM student WHERE ",
                "complete": {"sql": "SELECT name FROM student WHERE ", "contains": ["id", "name", "NOT", "NULL"]},
            },
            {
                "name": "insert_columns",
                "sql": "INSERT INTO student(",
                "complete": {"sql": "INSERT INTO student(", "contains": ["id", "name", "age"]},
            },
            {
                "name": "insert_second_table",
                "sql": "SELECT * FROM student, ",
                "complete": {"sql": "SELECT * FROM student, ", "contains": ["score"]},
            },
            {
                "name": "join_columns",
                "sql": "SELECT * FROM student JOIN score ON ",
                "complete": {"sql": "SELECT * FROM student JOIN score ON ", "contains": ["student.id", "score.student_id"]},
            },
            {
                "name": "forbidden_keywords_absent",
                "sql": "SELECT * FROM student ",
                "complete": {"sql": "SELECT * FROM student ", "not_contains": ["LIMIT", "HAVING", "UNION", "ALTER"]},
            },
            {
                "name": "no_completion_in_comment",
                "sql": "SELECT * FROM student -- FRO",
                "complete": {"sql": "SELECT * FROM student -- FRO", "empty": True},
            },
            {
                "name": "no_completion_in_string",
                "sql": "SELECT * FROM student WHERE name = 'SEL",
                "complete": {"sql": "SELECT * FROM student WHERE name = 'SEL", "empty": True},
            },
            {
                "name": "multi_statement_second_only",
                "sql": "CREATE TABLE tmp_a(a int); SELECT * FR",
                "complete": {"sql": "CREATE TABLE tmp_a(a int); SELECT * FR", "contains": ["FROM"]},
            },
            {
                "name": "case_style_lower",
                "sql": "select * fr",
                "complete": {"sql": "select * fr", "contains": ["from"]},
            },
        ],
    },
]
