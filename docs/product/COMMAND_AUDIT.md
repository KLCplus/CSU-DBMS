# CSUDB 2026 Command Audit

The audit was produced from `lex_sql.l`, `yacc_sql.y`, `StmtType`, `Stmt::create_stmt`, `CommandExecutor`, executor implementations, server option parsing, and the product service/client code.

| Command | Existing Before | Implemented This Task | Supported | Notes |
| --- | --- | --- | --- | --- |
| SELECT | YES | - | YES | Projection, filter comparisons, aggregate/group paths |
| INSERT | YES | - | YES | One tuple; no target-column list |
| DELETE | YES | - | YES | WHERE supported through existing filter path |
| UPDATE | PARTIAL | - | NO | Parsed, but `Stmt::create_stmt` has no update branch |
| CREATE TABLE | YES | - | YES | Existing command executor |
| SHOW TABLES | YES | - | YES | Existing command executor |
| DESC | YES | - | YES | `DESC`, not `DESCRIBE` |
| DROP TABLE | PARTIAL | - | NO | Grammar/node exists; no executable statement path |
| CREATE INDEX | YES | - | YES | Existing statement/operator path |
| DROP INDEX | PARTIAL | - | NO | Grammar/node exists; no executable statement path |
| ANALYZE TABLE | YES | - | YES | Existing command executor |
| EXPLAIN | YES | - | YES | Existing statement/operator path |
| BEGIN/COMMIT/ROLLBACK | YES | - | YES | Existing transaction command executor |
| SET | YES | - | YES | Existing command executor |
| LOAD DATA | YES | - | YES | Existing command executor |
| HELP | YES | - | YES | Existing command executor |
| SYNC | PARTIAL | - | NO | Parsed but not converted to an executable statement |
| CREATE DATABASE | NO | YES | YES | DatabaseService management boundary |
| DROP DATABASE | NO | YES | YES | Closes and removes the selected database directory |
| SHOW DATABASES | NO | YES | YES | Persistent product catalog |
| USE | NO | YES | YES | Changes Session current database |
| CREATE USER | NO | YES | YES | Persistent salted password record |
| ALTER USER | NO | YES | YES | Password rotation |
| DROP USER | NO | YES | YES | Root cannot be dropped |
| SHOW USERS | NO | YES | YES | Requires CREATE_USER privilege |
| GRANT | NO | YES | YES | Small GLOBAL/DATABASE/TABLE privilege model |
| REVOKE | NO | YES | YES | Central catalog update |
| SHOW GRANTS | NO | YES | YES | Self or GRANT privilege |
| SELECT DATABASE() | NO | NO | NO | Use `\database` |
| JOIN keyword | NO | NO | NO | Comma-separated table sources exist; keyword does not |
| ORDER BY | NO | NO | NO | No production in the audited grammar |
| OR / unary NOT | NO | NO | NO | AND condition list exists; no OR/NOT production |

## Product command audit

| Area | Before | Now |
| --- | --- | --- |
| Separate `csudb` client and `csudbd` server | NO | YES |
| Authenticated native protocol | NO | YES |
| Multiline/product meta-command shell | NO | YES |
| Stable QueryResult DTO | NO | YES |
| Profile/environment precedence | NO | YES |
| Global CMake installation | NO | YES |
| Plain/MySQL compatibility paths | YES | Retained; not advertised as authenticated/full compatible |
