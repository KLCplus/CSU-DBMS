/*
 * ------------------------------------------------------------------------------------------------
 * 本文件功能索引（VSCode / Cursor：Ctrl+Click `文件:行号` 跳转）
 * ------------------------------------------------------------------------------------------------
 *  lex_sql.l:35                   Flex 词法分析器总览与实现原则
 *  lex_sql.l:61                   每次 yylex 开始重置行/列号
 *  lex_sql.l:67                   每个 token 后记录 <行,列> 位置
 *  lex_sql.l:78                   include、宏（RETURN_TOKEN 等）
 *  lex_sql.l:101                  返回 token 的宏
 *  lex_sql.l:105                  no yywrap/不区分大小写/reentrant 等
 *  lex_sql.l:116                  WHITE_SPACE/DIGIT/ID/DOT/QUOTE
 *  lex_sql.l:124                  %x STR / %x COMMENT
 *  lex_sql.l:133                  %% 之后是规则区
 *  lex_sql.l:135                  空白、换行、-- 行注释、块注释
 *  lex_sql.l:144                  NUMBER / FLOAT
 *  lex_sql.l:148                  分号、点号与全部关键字入口
 *  lex_sql.l:165                  SELECT/CALC/FROM/WHERE/AND/OR
 *  lex_sql.l:172                  INSERT/INTO/VALUES/DELETE/UPDATE/SET
 *  lex_sql.l:179                  BEGIN/COMMIT/ROLLBACK
 *  lex_sql.l:183                  NULL/IS/NOT/INT/CHAR/FLOAT/VECTOR/DATE
 *  lex_sql.l:192                  LOAD/DATA/INFILE
 *  lex_sql.l:196                  EXPLAIN/GROUP/BY/ORDER/STORAGE/FORMAT/PRIMARY/KEY
 *  lex_sql.l:205                  ANALYZE/FIELDS/TERMINATED/ENCLOSED
 *  lex_sql.l:210                  返回 ID，strdup 登记到 yyextra
 *  lex_sql.l:212                  LBRACE / RBRACE
 *  lex_sql.l:216                  EQ/NE/LE/LT/GE/GT
 *  lex_sql.l:227                  返回字符本身 + - * /
 *  lex_sql.l:232                  单/双引号返回 SSS，登记到 yyextra
 *  lex_sql.l:236                  未识别字符记日志并原样返回
 *  lex_sql.l:247                  把待解析字符串设为扫描器输入缓冲区
 * ------------------------------------------------------------------------------------------------
 */

%top{
/**
 * @file lex_sql.l
 * @brief Flex 词法分析器定义：把 SQL 文本切分为 Bison 可识别的 token
 * @ingroup SQLParser
 * @details 本文件是编译器流水线的第一环：
 *   SQL 文本 --(本文件, Flex DFA)--> token 流 --(Bison LALR)--> ParsedSqlNode AST
 *   --(ParseStage)--> --(ResolveStage)--> Stmt。
 * 核心实现原则：
 *   1. Flex 依据正则的“最长匹配优先、先定义优先”规则把输入切分；
 *   2. 每个关键字 token 通过 RETURN_TOKEN 宏返回，并把语义值写入 yylval（即 bison
 *      %union 联合体）；
 *   3. 借助 YY_USER_ACTION 记录 <行,列> 位置，供语法/语义错误定位；
 *   4. 字符串字面量的内存用 strdup 分配并登记到 yyextra 的 vector 中，由
 *      yacc_sql.y 的 sql_parse 统一释放，避免内存泄漏；
 *   5. 规则区注释必须缩进，否则会被 flex 误当作正则。
 */

/*
这里的代码会被复制到lex_sql.cpp的最开始位置
定义yy_size_t的原因是因为flex生成的代码，会使用yy_size_t与其他类型的数字
做比较，导致编译报警
*/
#define YY_TYPEDEF_YY_SIZE_T
typedef int yy_size_t;

/* 参考生成的lex_sql.cpp代码，这个宏定义会放在每次运行yylex()最开始的地方 */
#define YY_USER_INIT                                         \
  yylineno = 1;                                              \
  yycolumn = 1;

/* 参考生成的lex_sql.cpp代码，这个宏定义会放在解析一个token之后，也可以在网上找到大量的参考资料 */
/* 我们在这里设置当前解析的token的位置信息，这样在yacc中就可以使用这些信息了 */
#define YY_USER_ACTION                                       \
do {                                                         \
  yylloc->first_line   = yylloc->last_line = yylineno;       \
  yylloc->first_column = yycolumn;                           \
  yylloc->last_column  = yylloc->first_column + yyleng - 1;  \
  yycolumn += yyleng;                                        \
}                                                            \
while (0);

}

%{
#include<string.h>
#include<stdio.h>
#include <vector>

/**
 * flex 代码包含三个部分，使用 %% 分隔
 * 第一个部分的代码是C代码，flex会原样复制到目标文件中
 * 第二个部分是规则部分，使用正则表达式定义一系列规则
 * 第三个部分还是C代码，flex 会复制此段代码
 */

#include "sql/parser/parse_defs.h"
#include "common/log/log.h"
#include "yacc_sql.hpp"

#ifndef register
#define register 
#endif // register

extern int atoi();
extern double atof();

#define RETURN_TOKEN(token) LOG_DEBUG("%s", #token);return token
%}

/* Prevent the need for linking with -lfl */
%option noyywrap
%option nounput
%option noinput
%option bison-bridge
%option reentrant
/* 跟踪行号，配合 YY_USER_ACTION 输出 <行,列> */
%option yylineno
/* 不区分大小写 */
%option case-insensitive
%option bison-locations

/* 词法宏定义：空白字符、数字、标识符、点号、引号 */
WHITE_SPACE      [\ \t\b\f]
DIGIT            [0-9]+

/* ID 以字母或下划线开头，后续可跟字母/数字/下划线（关键字也走此规则，靠规则顺序优先匹配） */
ID               [A-Za-z_]+[A-Za-z0-9_]*
DOT              \.
QUOTE            [\'\"]
/* 独占起始状态：STR 用于字符串（当前未使用），COMMENT 用于块注释 */
%x STR
%x COMMENT

/* 规则匹配的优先级：*/
/* 1. 匹配的规则长的优先 */
/* 2. 写在最前面的优先 */

/* yylval 就可以认为是 yacc 中 %union 定义的结构体(union 结构) */
%%

  /* 空白与注释：忽略空格/制表等、单独处理换行以重置列号，丢弃 -- 行注释与块注释 */
{WHITE_SPACE}                                        // ignore whitespace
\n                                                   { yycolumn = 1; }
"--"[^\n]*                                           ;
"/*"                                                 { BEGIN(COMMENT); }
<COMMENT>"*/"                                        { BEGIN(INITIAL); }
<COMMENT>[^*]+                                       ;
<COMMENT>\*                                          ;

  /* 数字字面量：纯数字返回 NUMBER（语义值 int），带小数点的返回 FLOAT（语义值 float） */
{DIGIT}+                           yylval->number=atoi(yytext); RETURN_TOKEN(NUMBER);
{DIGIT}+{DOT}{DIGIT}+              yylval->floats=(float)(atof(yytext)); RETURN_TOKEN(FLOAT);

  /* 标点与保留字：分号/点号及全部 SQL 关键字，大小写不敏感地映射为同名 token */
";"                                     RETURN_TOKEN(SEMICOLON);
{DOT}                                   RETURN_TOKEN(DOT);
EXIT                                    RETURN_TOKEN(EXIT);
HELP                                    RETURN_TOKEN(HELP);
DESC                                    RETURN_TOKEN(DESC);
ASC                                     RETURN_TOKEN(ASC);
CREATE                                  RETURN_TOKEN(CREATE);
DROP                                    RETURN_TOKEN(DROP);
TABLE                                   RETURN_TOKEN(TABLE);
TABLES                                  RETURN_TOKEN(TABLES);
INDEX                                   RETURN_TOKEN(INDEX);
ON                                      RETURN_TOKEN(ON);
JOIN                                    RETURN_TOKEN(JOIN);
INNER                                   RETURN_TOKEN(INNER);
SHOW                                    RETURN_TOKEN(SHOW);
SYNC                                    RETURN_TOKEN(SYNC);
  /* 查询类关键字：SELECT / CALC / FROM / WHERE / AND / OR */
SELECT                                  RETURN_TOKEN(SELECT);
CALC                                    RETURN_TOKEN(CALC);
FROM                                    RETURN_TOKEN(FROM);
WHERE                                   RETURN_TOKEN(WHERE);
AND                                     RETURN_TOKEN(AND);
OR                                      RETURN_TOKEN(OR);
  /* DML 关键字：INSERT / INTO / VALUES / DELETE / UPDATE / SET */
INSERT                                  RETURN_TOKEN(INSERT);
INTO                                    RETURN_TOKEN(INTO);
VALUES                                  RETURN_TOKEN(VALUES);
DELETE                                  RETURN_TOKEN(DELETE);
UPDATE                                  RETURN_TOKEN(UPDATE);
SET                                     RETURN_TOKEN(SET);
  /* 事务关键字：BEGIN/COMMIT/ROLLBACK 分别映射为 TRX_BEGIN/TRX_COMMIT/TRX_ROLLBACK */
BEGIN                                   RETURN_TOKEN(TRX_BEGIN);
COMMIT                                  RETURN_TOKEN(TRX_COMMIT);
ROLLBACK                                RETURN_TOKEN(TRX_ROLLBACK);
  /* NULL 判定与类型关键字：NULL/IS/NOT 及 INT/CHAR/FLOAT/VECTOR/DATE */
NULL                                    RETURN_TOKEN(NULL_T);
IS                                      RETURN_TOKEN(IS);
NOT                                     RETURN_TOKEN(NOT);
INT                                     RETURN_TOKEN(INT_T);
CHAR                                    RETURN_TOKEN(STRING_T);
FLOAT                                   RETURN_TOKEN(FLOAT_T);
VECTOR                                  RETURN_TOKEN(VECTOR_T);
DATE                                    RETURN_TOKEN(DATE);
  /* LOAD DATA 导入关键字：LOAD/DATA/INFILE */
LOAD                                    RETURN_TOKEN(LOAD);
DATA                                    RETURN_TOKEN(DATA);
INFILE                                  RETURN_TOKEN(INFILE);
  /* EXPLAIN 及 GROUP BY / ORDER BY / 建表存储格式等子句关键字 */
EXPLAIN                                 RETURN_TOKEN(EXPLAIN);
GROUP                                   RETURN_TOKEN(GROUP);
BY                                      RETURN_TOKEN(BY);
ORDER                                   RETURN_TOKEN(ORDER);
STORAGE                                 RETURN_TOKEN(STORAGE);
FORMAT                                  RETURN_TOKEN(FORMAT);
PRIMARY                                 RETURN_TOKEN(PRIMARY);
KEY                                     RETURN_TOKEN(KEY);
  /* ANALYZE 及 LOAD 的字段分隔/包围符关键字 */
ANALYZE                                 RETURN_TOKEN(ANALYZE);
FIELDS                                  RETURN_TOKEN(FIELDS);
TERMINATED                              RETURN_TOKEN(TERMINATED);
ENCLOSED                                RETURN_TOKEN(ENCLOSED);
  /* 标识符：返回 ID；strdup 字符串登记到 yyextra，解析结束后统一 free 防止泄漏 */
{ID}                                    yylval->cstring=strdup(yytext); static_cast<std::vector<char*>*>(yyextra)->push_back(yylval->cstring); RETURN_TOKEN(ID);
  /* 括号：左/右圆括号分别返回 LBRACE / RBRACE（注意命名不同于花括号） */
"("                                     RETURN_TOKEN(LBRACE);
")"                                     RETURN_TOKEN(RBRACE);

  /* 逗号与比较运算符：== 与 = 都返回 EQ，<> 与 != 都返回 NE */
","                                     RETURN_TOKEN(COMMA);
"=="                                    RETURN_TOKEN(EQ);
"="                                     RETURN_TOKEN(EQ);
"<="                                    RETURN_TOKEN(LE);
"<>"                                    RETURN_TOKEN(NE);
"!="                                    RETURN_TOKEN(NE);
"<"                                     RETURN_TOKEN(LT);
">="                                    RETURN_TOKEN(GE);
">"                                     RETURN_TOKEN(GT);

  /* 算术运算符：直接返回字符本身，由 bison 的字符字面量终结符接收 */
"+" |
"-" |
"*" |
"/"                                     { return yytext[0]; }
  /* 字符串字面量：单/双引号包裹均返回 SSS，原始串同样登记到 yyextra；转义在语法动作里还原 */
\"([^"\\]|\\.|\"\")*\"                   yylval->cstring = strdup(yytext); static_cast<std::vector<char*>*>(yyextra)->push_back(yylval->cstring); RETURN_TOKEN(SSS);
\'([^'\\]|\\.|\'\')*\'                   yylval->cstring = strdup(yytext); static_cast<std::vector<char*>*>(yyextra)->push_back(yylval->cstring); RETURN_TOKEN(SSS);

  /* 兜底规则：未识别字符记日志并原样返回，最终由 bison 报告语法错误 */
.                                       LOG_DEBUG("Unknown character [%c]",yytext[0]); return yytext[0];
%%

/**
 * @brief 把待解析的字符串设置为词法扫描器的输入缓冲区
 * @param str     以 '\0' 结尾的 SQL 字符串
 * @param scanner 由 yylex_init_extra 创建的、可重入的词法扫描器句柄
 * @details 实现原理：yy_scan_string 为字符串新建一个内部缓冲区，yy_switch_to_buffer
 * 把 scanner 切换过去。调用方（yacc_sql.y 的 sql_parse）随后用 yylex_destroy 释放。
 */
void scan_string(const char *str, yyscan_t scanner) {
  yy_switch_to_buffer(yy_scan_string(str, scanner), scanner);
}
