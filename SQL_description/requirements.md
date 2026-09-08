# 必须要完成的验收能力
- CREATE
- INSERT
- SELECT
- DELETE
- WHERE
- 比较运算
- AND
- OR
- NOT
- 括号
# 进阶版本
- UPDATE
- ORDER BY
- GROUP BY
- JOIN
- 算术表达式
- NULL

# 词法分析器

## 输入：
1. SQL文件
2. 单条SQL语句
3. 多条SQL语句

## 输出：
<TokenType,Lexeme,Line,Column>

## 需要做的处理：
1. 识别关键字
2. 识别标识符
3. 识别常量
4. 识别运算符
5. 识别分隔符
6. 跳过空白
7. 跳过注释
8. 处理字符串
9. 处理转义
10. 大小写不敏感（统一处理）

## 测试要求
非法输入必须返回错误类型+位置+原因，且不能崩溃

1. 注释型输入 
```
-- single line
/* multi-line */
```
2. 多字符运行符
```
>= <= != ==
```
3. 字符串
```
'Alice' Tom''s book'
```
4. 大小写
```
select SELECT SeLeCt
```
5. 非法输入
```
@ 未闭合字符串 非法数字
```
6. 整段用例测试

输入：
```
-- query
SELECT name
FROM student
WHERE age >=18
  AND name !='Tom';
```
输出应为
```
KEYWORD      SELECT     (2,1)
IDENTIFIER   name       (2,8)
KEYWORD      FROM       (3,1)
IDENTIFIER   student    (3,6)
KEYWORD      WHERE      (4,1)
IDENTIFIER   age        (4,7)
OPERATOR     >=         (4,11)
CONST        18         (4,14)
KEYWORD      AND        (5,3)
IDENTIFIER   name       (5,7)
OPERATOR     !=         (5,12)
CONST        'Tom'      (5,15)
DELIMITER    ;          (5,20)
```

# 语法分析
输入： 词法分析产生的Token Stream

输出： AST

## 需要做的处理
1. 理解优先级：NOT>比较运算>AND>OR
2. 括号可显示改变结合结构
3. 消除左递归
4. 构造FIRST集
5. 构造FOLLOW集
6. LL(1)表
7. 梯度下降
8. LR（可选挑战）
9. 可以做语法诊断，给出错误位置+实际符号+期望集合
输入案例
```
SELECT name
FROM student
WHERE age>18 AND;
```
输出案例
```
SyntaxError at line 3,column 19

unexpected token: ';'
expected: IDENTIFIER | CONST |'(' | NOT
```

# 语义分析

## 要求
1. 验证表存在性
2. 验证列存在性
3. 名字绑定
4. 类型一致性
5. INSERT匹配
6. Catalog需要提供接口createTable / findTable / find Column / getType
7. Catalog需要服务执行引擎和持久化
8. 完成名字绑定
例
```
SELECT name
FROM student
WHERE score >60;
```
应报告
```
SemanticError: column 'score' does not exist in table 'student'.
```
9. 简化类型系统（将类型规则集中管理，而不是散落再Parser或者执行器的if/else中）
- INT+INT->INT
- INT>INT->BOOL
- VARCHAR=VARCHAR->BOOL
- BOOL AND BOOL->BOOL
- NOT BOOL->BOOL
- INT+VARCHAR->ERROR
10. 语义错误要报告类型+位置+原因
例1

输入：
```
SELECT name
FROM student
WHERE age + 'abc' > 20;
```
输出
```
SemanticError at line 3, column 11

operator '+' cannot be applied to
INT and VARCHAR
```

例2

输入
```
INSERT INTO student(id,name)
VALUES ('Alice', 1);
```

输出
```
TypeMismatch:
student.id expects INT, but VARCHAR found.
student.name expects VARCHAR, but INT found.
```

# 系统工程
## 编译器与数据库接口
1. CREATETABLE:创建表与注册元数据
2. INSERT:向目标表写入记录
3. SeqScan：顺序扫描表记录
4. Filter：执行WHERE条件
5. Project： 返回SELECT指定列
6. 输出形式： json

## AST到Logical Plan
1. FROM决定数据源
2. WHERE生成Filter
3. SELECT列表生成Project
4. Plan节点只保存执行所需信息

## 规则式优化
1. 常量折叠
```
age>10+8->age>18
```
2. 布尔化简
```
x AND TRUE->x
```
3. Projection Purning 只保留查询真正需要的列
4. Predicate Pushdown 有JOIN/子查询扩展时尽量提前过滤
5. 冗余节点消除 Project[*]/恒真Filter可直接消除

## 测试要求
1. 要能够从任意 SELECT AST解释每一个Plan节点为何产生，以及节点之间的数据流关系。
```
SELECT name
FROM student
WHERE age > 18;
```
2. 规则式优化前后必须能展示结构变化
```
SELECT name FROM student WHERE 1=1 AND age>10+8;
```
应能看出优化后Filter[age>18]

# 扩展加分项
1. 语言扩展
- UPDATE 
- ORDER BY
- GROUP BY
- JOIN
- NULL
- 更多数据类型
2. 编译器扩展
- 错误恢复
- 更复杂AST
- 优化规则框架
- Plan可视化
- 序列化
- json输出
3. 系统扩展
- JOIN执行算子
- 索引扫描
- 简单代价模型
- EXPLAIN
- Catalog持久化

# 验收
## 测试体系
1. 核心SQL正常执行：一些正常的代码
2. 词法错误：非法字符、字符串未闭合
3. 语法错误：缺分号、括号不匹配、结构错误
4. 语义错误： 表/列不存在、类型不匹配
5. 边界测试： 空输入、极长标识符、多语句、大小写
6. 自建测试集
7. 关注指标 Crash/Wrong Accept/Wrong Reject / Error Location
