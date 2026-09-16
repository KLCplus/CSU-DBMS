/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once

#include <cstddef>
#include <string>
#include <vector>

/**
 * @file completion_types.h
 * @brief SQL 输入补全的通用数据结构（与 UI 无关）
 * @ingroup SQLAutocomplete
 *
 * 本文件定义补全模块在所有 UI/协议层之间共享的纯数据结构：候选类型枚举、
 * 候选来源枚举、单条候选 CompletionItem、请求 CompletionRequest 与响应 CompletionResponse。
 * 在整体架构中它是「确定性补全 + 可选模型补全」两条链路的公共数据契约。
 *
 * 核心原则：本文件只承载数据，不包含任何 SQL 解析、Catalog 访问或网络逻辑，
 * 从而使不同 provider 产生的候选可以用统一的结构表达并统一排序/裁剪。
 */

/**
 * @brief 候选的语义种类，供客户端分组、着色与排序使用
 */
enum class CompletionKind
{
  Keyword,   ///< SQL 关键字（来自语法期望集合）
  Table,     ///< 表名（来自 Catalog）
  Column,    ///< 列名（来自 Catalog）
  Alias,     ///< 表别名（当前 grammar 不支持，预留）
  Operator,  ///< 运算符 / 分隔符
  Type,      ///< 数据类型（INT/CHAR/FLOAT/VECTOR/DATE）
  Literal,   ///< 字面量（如 NULL、空字符串）
  Snippet,   ///< 代码片段（预留）
  Model      ///< 模型补全（预留）
};

/**
 * @brief 候选的产生来源，用于区分确定性来源与模型来源
 */
enum class CompletionSource
{
  Grammar,   ///< 来自 bison 期望符号集合 / Parser 试探
  Catalog,   ///< 来自真实 Catalog 的表与列
  Semantic,  ///< 来自语义规则（如 INSERT VALUES 的字面量）
  Model      ///< 来自 FIM 模型
};

/**
 * @brief 将候选种类转换为稳定的字符串名（用于协议输出）
 * @param kind 候选种类
 * @return 小写字符串；未知取值返回 "unknown"
 * @details 实现原理：对枚举做穷举 switch，每个分支返回字面量；末尾兜底返回 "unknown"。
 */
const char *completion_kind_name(CompletionKind kind);

/**
 * @brief 将候选来源转换为稳定的字符串名（用于协议输出）
 * @param source 候选来源
 * @return 小写字符串；未知取值返回 "unknown"
 * @details 实现原理：对枚举做穷举 switch，每个分支返回字面量；末尾兜底返回 "unknown"。
 */
const char *completion_source_name(CompletionSource source);

/**
 * @brief 单条补全候选
 * @details UI 接受该候选时，用 insert_text 替换 SQL buffer 中
 *          [replace_start, replace_end) 的半截 token。
 */
struct CompletionItem
{
  std::string      insert_text;   ///< 接受后真正写入的文本
  std::string      display_text;  ///< 下拉列表中展示的文本
  CompletionKind   kind   = CompletionKind::Keyword;         ///< 候选语义种类
  CompletionSource source = CompletionSource::Grammar;       ///< 候选来源

  // 替换用户当前半截 token 的范围（相对于整段 SQL buffer）
  size_t replace_start = 0;
  size_t replace_end   = 0;

  double      score = 0.0;  ///< 越高越靠前
  std::string detail;       ///< 例如 "student.name : VARCHAR"
};

/**
 * @brief 一次补全请求
 */
struct CompletionRequest
{
  std::string sql;                       ///< 完整 SQL buffer
  size_t      cursor_offset     = 0;     ///< 光标在 buffer 中的偏移
  size_t      max_items         = 12;    ///< 期望返回的候选上限
  bool        want_model_completion = true;  ///< 是否允许调用模型补全
};

/**
 * @brief 一次补全响应
 * @details 确定性候选放在 items；模型补全以 ghost_text 形式附加，
 *          model_used 标记本次是否真的采用了模型结果。
 */
struct CompletionResponse
{
  std::vector<CompletionItem> items;       ///< 确定性补全候选
  std::string                 ghost_text;  ///< 可为空，用于 inline ghost text
  bool                        model_used = false;  ///< 是否使用了模型补全
};
