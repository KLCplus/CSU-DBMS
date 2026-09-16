// ------------------------------------------------------------------------------------------------
// 本文件功能索引（VSCode / Cursor：Ctrl+Click `文件:行号` 跳转）
// ------------------------------------------------------------------------------------------------
//  parse_defs.h:80                描述一个属性
//  parse_defs.h:90                描述比较运算符
//  parse_defs.h:111               表示一个条件比较
//  parse_defs.h:141               描述一个显式 JOIN 子句
//  parse_defs.h:147               ORDER BY 的排序方向（ASC / DESC）
//  parse_defs.h:157               ORDER BY 的单个排序项
//  parse_defs.h:163               描述一个 select 语句
//  parse_defs.h:178               算术表达式计算的语法树
//  parse_defs.h:188               描述一个insert语句
//  parse_defs.h:199               描述一个delete语句
//  parse_defs.h:209               描述一个update语句
//  parse_defs.h:222               描述一个属性
//  parse_defs.h:235               描述一个create table语句
//  parse_defs.h:249               描述一个drop table语句
//  parse_defs.h:258               描述一个analyze table语句
//  parse_defs.h:269               描述一个create index语句
//  parse_defs.h:280               描述一个drop index语句
//  parse_defs.h:291               描述一个desc table语句
//  parse_defs.h:301               描述一个load data语句
//  parse_defs.h:314               设置变量的值
//  parse_defs.h:329               描述一个explain语句
//  parse_defs.h:339               解析SQL语句出现了错误
//  parse_defs.h:350               表示一个SQL语句的类型
//  parse_defs.h:380               表示一个SQL语句
//  parse_defs.h:410               以指定命令类型构造节点
//  parse_defs.h:417               表示语法解析后的数据
//  parse_defs.h:425               追加一条已解析的 SQL 语句

/**
 * @file parse_defs.h
 * @brief SQL 语法树（AST）节点的数据结构定义
 * @ingroup SQLParser
 * @details 本文件定义了 Bison 语法分析结果的数据载体，处于编译器流水线中
 *   SQL 文本 -> Flex token -> Bison LALR -> ParsedSqlNode AST
 *   -> ParseStage -> ResolveStage -> Stmt 的“AST”一环。
 * 核心实现原则：每个 SQL 语句（select/insert/update/delete/create 等）对应一个
 * 结构体，统一挂在 ParsedSqlNode 上，通过 SqlCommandFlag 区分类型；解析阶段只记录
 * 语法结构（表名、字段名、常量、表达式等文本/值），不访问数据库元数据，所有语义
 * 校验推迟到 ResolveStage。各节点均支持表达式（Expression）以承载 WHERE/算术/聚合等。
 */

#pragma once

#include "common/lang/string.h"
#include "common/lang/vector.h"
#include "common/lang/memory.h"
#include "common/value.h"
#include "common/lang/utility.h"

class Expression;

/**
 * @defgroup SQLParser SQL Parser
 */

/**
 * @brief 描述一个属性
 * @ingroup SQLParser
 * @details 属性，或者说字段(column, field)
 * Rel -> Relation
 * Attr -> Attribute
 */
struct RelAttrSqlNode
{
  string relation_name;   ///< relation name (may be NULL) 表名
  string attribute_name;  ///< attribute name              属性名
};

/**
 * @brief 描述比较运算符
 * @ingroup SQLParser
 */
enum CompOp
{
  EQUAL_TO,     ///< "="
  LESS_EQUAL,   ///< "<="
  NOT_EQUAL,    ///< "<>"
  LESS_THAN,    ///< "<"
  GREAT_EQUAL,  ///< ">="
  GREAT_THAN,   ///< ">"
  IS_NULL,      ///< "IS NULL"
  IS_NOT_NULL,  ///< "IS NOT NULL"
  NO_OP
};

/**
 * @brief 表示一个条件比较
 * @ingroup SQLParser
 * @details 条件比较就是SQL查询中的 where a>b 这种。
 * 一个条件比较是有两部分组成的，称为左边和右边。
 * 左边和右边理论上都可以是任意的数据，比如是字段（属性，列），也可以是数值常量。
 * 这个结构中记录的仅仅支持字段和值。
 */
struct ConditionSqlNode
{
  int left_is_attr;              ///< TRUE if left-hand side is an attribute
                                 ///< 1时，操作符左边是属性名，0时，是属性值
  Value          left_value;     ///< left-hand side value if left_is_attr = FALSE
  RelAttrSqlNode left_attr;      ///< left-hand side attribute
  CompOp         comp;           ///< comparison operator
  int            right_is_attr;  ///< TRUE if right-hand side is an attribute
                                 ///< 1时，操作符右边是属性名，0时，是属性值
  RelAttrSqlNode right_attr;     ///< right-hand side attribute if right_is_attr = TRUE 右边的属性
  Value          right_value;    ///< right-hand side value if right_is_attr = FALSE
};

/**
 * @brief 描述一个select语句
 * @ingroup SQLParser
 * @details 一个正常的select语句描述起来比这个要复杂很多，这里做了简化。
 * 一个select语句由三部分组成，分别是select, from, where。
 * select部分表示要查询的字段，from部分表示要查询的表，where部分表示查询的条件。
 * 比如 from 中可以是多个表，也可以是另一个查询语句，这里仅仅支持表，也就是 relations。
 * where 条件 conditions，这里表示使用AND串联起来多个条件。正常的SQL语句会有OR，NOT等，
 * 甚至可以包含复杂的表达式。
 */

/**
 * @brief 描述一个显式 JOIN 子句
 * @ingroup SQLParser
 * @details 支持 `JOIN table ON condition`，一个 select 语句可以包含多个 join。
 * 语义上等价于把该表加入 from 列表，并把 ON 条件与 WHERE 条件用 AND 合并。
 */
struct JoinSqlNode
{
  string                 relation_name;  ///< join 的表名
  unique_ptr<Expression> condition;      ///< ON 条件
};

enum class OrderDirection
{
  ASC,
  DESC
};

/**
 * @brief ORDER BY 的单个排序项
 * @details 每个排序表达式必须独立携带方向，才能正确表达多键混合升降序。
 */
struct OrderByUnit
{
  unique_ptr<Expression> expression;
  OrderDirection         direction = OrderDirection::ASC;
};

struct SelectSqlNode
{
  vector<unique_ptr<Expression>> expressions;  ///< 查询的表达式
  vector<string>                 relations;    ///< 查询的表
  vector<ConditionSqlNode>       conditions;   ///< 查询条件，使用AND串联起来多个条件
  unique_ptr<Expression>         where_expression;  ///< WHERE 布尔表达式（支持 AND/OR/NOT/括号/算术）
  vector<unique_ptr<Expression>> group_by;     ///< group by clause
  vector<OrderByUnit>            order_by;     ///< order by clause
  vector<JoinSqlNode>            joins;        ///< 显式 JOIN 子句
};

/**
 * @brief 算术表达式计算的语法树
 * @ingroup SQLParser
 */
struct CalcSqlNode
{
  vector<unique_ptr<Expression>> expressions;  ///< calc clause
};

/**
 * @brief 描述一个insert语句
 * @ingroup SQLParser
 * @details 于Selects类似，也做了很多简化
 */
struct InsertSqlNode
{
  string        relation_name;  ///< Relation to insert into
  vector<string> columns;       ///< 可选的列清单；为空表示按表字段顺序插入
  vector<Value> values;         ///< 要插入的值
};

/**
 * @brief 描述一个delete语句
 * @ingroup SQLParser
 */
struct DeleteSqlNode
{
  string                   relation_name;  ///< Relation to delete from
  vector<ConditionSqlNode> conditions;
};

/**
 * @brief 描述一个update语句
 * @ingroup SQLParser
 */
struct UpdateSqlNode
{
  string                   relation_name;   ///< Relation to update
  string                   attribute_name;  ///< 更新的字段，仅支持一个字段
  Value                    value;           ///< 更新的值，仅支持一个字段
  vector<ConditionSqlNode> conditions;
};

/**
 * @brief 描述一个属性
 * @ingroup SQLParser
 * @details 属性，或者说字段(column, field)
 */
struct AttrInfoSqlNode
{
  AttrType type;               ///< Type of attribute
  string   name;               ///< Attribute name
  size_t   length;             ///< Length of attribute
  bool     nullable = true;    ///< 是否允许为 NULL（默认允许）
};

/**
 * @brief 描述一个create table语句
 * @ingroup SQLParser
 * @details 这里也做了很多简化。
 */
struct CreateTableSqlNode
{
  string                  relation_name;  ///< Relation name
  vector<AttrInfoSqlNode> attr_infos;     ///< attributes
  vector<string>          primary_keys;   ///< primary keys
  // TODO: integrate to CreateTableOptions
  string storage_format;  ///< storage format
  string storage_engine;  ///< storage engine
};

/**
 * @brief 描述一个drop table语句
 * @ingroup SQLParser
 */
struct DropTableSqlNode
{
  string relation_name;  ///< 要删除的表名
};

/**
 * @brief 描述一个analyze table语句
 * @ingroup SQLParser
 */
struct AnalyzeTableSqlNode
{
  string relation_name;  ///< 要分析的表名
};

/**
 * @brief 描述一个create index语句
 * @ingroup SQLParser
 * @details 创建索引时，需要指定索引名，表名，字段名。
 * 正常的SQL语句中，一个索引可能包含了多个字段，这里仅支持一个字段。
 */
struct CreateIndexSqlNode
{
  string index_name;      ///< Index name
  string relation_name;   ///< Relation name
  string attribute_name;  ///< Attribute name
};

/**
 * @brief 描述一个drop index语句
 * @ingroup SQLParser
 */
struct DropIndexSqlNode
{
  string index_name;     ///< Index name
  string relation_name;  ///< Relation name
};

/**
 * @brief 描述一个desc table语句
 * @ingroup SQLParser
 * @details desc table 是查询表结构信息的语句
 */
struct DescTableSqlNode
{
  string relation_name;
};

/**
 * @brief 描述一个load data语句
 * @ingroup SQLParser
 * @details 从文件导入数据到表中。文件中的每一行就是一条数据，每行的数据类型、字段个数都与表保持一致
 */
struct LoadDataSqlNode
{
  string relation_name;
  string file_name;
  string terminated = ",";
  string enclosed   = "\"";
};

/**
 * @brief 设置变量的值
 * @ingroup SQLParser
 * @note 当前还没有查询变量
 */
struct SetVariableSqlNode
{
  string name;
  Value  value;
};

class ParsedSqlNode;

/**
 * @brief 描述一个explain语句
 * @ingroup SQLParser
 * @details 会创建operator的语句，才能用explain输出执行计划。
 * 一个command就是一个语句，比如select语句，insert语句等。
 * 可能改成SqlCommand更合适。
 */
struct ExplainSqlNode
{
  unique_ptr<ParsedSqlNode> sql_node;
};

/**
 * @brief 解析SQL语句出现了错误
 * @ingroup SQLParser
 * @details 当前解析时并没有处理错误的行号和列号
 */
struct ErrorSqlNode
{
  string error_msg;
  int    line;
  int    column;
};

/**
 * @brief 表示一个SQL语句的类型
 * @ingroup SQLParser
 */
enum SqlCommandFlag
{
  SCF_ERROR = 0,
  SCF_CALC,
  SCF_SELECT,
  SCF_INSERT,
  SCF_UPDATE,
  SCF_DELETE,
  SCF_CREATE_TABLE,
  SCF_DROP_TABLE,
  SCF_ANALYZE_TABLE,
  SCF_CREATE_INDEX,
  SCF_DROP_INDEX,
  SCF_SYNC,
  SCF_SHOW_TABLES,
  SCF_DESC_TABLE,
  SCF_BEGIN,  ///< 事务开始语句，可以在这里扩展只读事务
  SCF_COMMIT,
  SCF_CLOG_SYNC,
  SCF_ROLLBACK,
  SCF_LOAD_DATA,
  SCF_HELP,
  SCF_EXIT,
  SCF_EXPLAIN,
  SCF_SET_VARIABLE,  ///< 设置变量
};
/**
 * @brief 表示一个SQL语句
 * @ingroup SQLParser
 */
class ParsedSqlNode
{
public:
  enum SqlCommandFlag flag;
  ErrorSqlNode        error;
  CalcSqlNode         calc;
  SelectSqlNode       selection;
  InsertSqlNode       insertion;
  DeleteSqlNode       deletion;
  UpdateSqlNode       update;
  CreateTableSqlNode  create_table;
  DropTableSqlNode    drop_table;
  AnalyzeTableSqlNode analyze_table;
  CreateIndexSqlNode  create_index;
  DropIndexSqlNode    drop_index;
  DescTableSqlNode    desc_table;
  LoadDataSqlNode     load_data;
  ExplainSqlNode      explain;
  SetVariableSqlNode  set_variable;

public:
  /**
   * @brief 默认构造函数：语句类型置为 SCF_ERROR
   * @details 默认即错误态，未被显式赋值的节点会被视为解析失败，属于防御性设计。
   */
  ParsedSqlNode();
  /**
   * @brief 以指定命令类型构造节点
   * @param flag SQL 命令类型，用于后续阶段分派处理
   */
  explicit ParsedSqlNode(SqlCommandFlag flag);
};

/**
 * @brief 表示语法解析后的数据
 * @ingroup SQLParser
 */
class ParsedSqlResult
{
public:
  /**
   * @brief 追加一条已解析的 SQL 语句
   * @param sql_node 语句节点（独占所有权，移动语义）
   * @details 实现原理：emplace_back + move，零深拷贝地接管节点所有权。
   */
  void add_sql_node(unique_ptr<ParsedSqlNode> sql_node);

  /**
   * @brief 获取解析出的全部语句节点
   * @return 语句节点列表的可变引用
   * @details ParseStage 用它判断是否为空、是否存在 SCF_ERROR，并取出首个节点。
   */
  vector<unique_ptr<ParsedSqlNode>> &sql_nodes() { return sql_nodes_; }

private:
  vector<unique_ptr<ParsedSqlNode>> sql_nodes_;  ///< 这里记录SQL命令。虽然看起来支持多个，但是当前仅处理一个
};
