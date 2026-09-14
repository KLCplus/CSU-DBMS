/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

//
// Created for UPDATE support.
//

#include "sql/operator/update_physical_operator.h"
#include "common/log/log.h"
#include "common/type/attr_type.h"
#include "storage/table/table.h"
#include "storage/field/field_meta.h"
#include "storage/trx/trx.h"

#include <algorithm>
#include <cstring>

using namespace std;
using namespace common;

UpdatePhysicalOperator::UpdatePhysicalOperator(Table *table, const FieldMeta *field_meta, Value value)
    : table_(table), field_meta_(field_meta), value_(value)
{}

RC UpdatePhysicalOperator::open(Trx *trx)
{
  if (children_.empty()) {
    return RC::SUCCESS;
  }

  unique_ptr<PhysicalOperator> &child = children_[0];

  RC rc = child->open(trx);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to open child operator: %s", strrc(rc));
    return rc;
  }

  trx_ = trx;
  records_.clear();

  while (OB_SUCC(rc = child->next())) {
    Tuple *tuple = child->current_tuple();
    if (nullptr == tuple) {
      child->close();
      LOG_WARN("failed to get current record from update child");
      return RC::INTERNAL;
    }

    RowTuple *row_tuple = static_cast<RowTuple *>(tuple);
    Record   &record    = row_tuple->record();
    Record    copied_record;
    rc = copied_record.copy_data(record.data(), record.len());
    if (OB_FAIL(rc)) {
      child->close();
      return rc;
    }
    // child 提供真实记录及 RID；复制后再更新，避免扫描期间改页导致迭代器失效。
    copied_record.set_rid(record.rid());
    records_.emplace_back(std::move(copied_record));
  }

  RC close_rc = child->close();
  if (rc == RC::RECORD_EOF) {
    rc = RC::SUCCESS;
  }
  if (OB_FAIL(rc)) {
    return rc;
  }
  if (OB_FAIL(close_rc)) {
    return close_rc;
  }

  // 持续消费 child 并先收集全部匹配记录，保证多行 UPDATE 且避免扫描器被页内更新破坏。
  const TableMeta &table_meta = table_->table_meta();
  int              field_index = -1;
  for (int i = 0; i < table_meta.field_num(); i++) {
    if (table_meta.field(i) == field_meta_) {
      field_index = i;
      break;
    }
  }

  for (Record &old_record : records_) {
    Record new_record;
    rc = new_record.copy_data(old_record.data(), old_record.len());
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to copy record data. rc=%s", strrc(rc));
      return rc;
    }
    new_record.set_rid(old_record.rid());

    // 同步维护 NULL 位图，否则原值为 NULL 的字段更新后仍会被读成 NULL
    if (field_index >= 0) {
      char *bitmap = new_record.data() + table_meta.null_bitmap_offset();
      if (value_.is_null()) {
        bitmap[field_index / 8] |= static_cast<char>(1 << (field_index % 8));
        memset(new_record.data() + field_meta_->offset(), 0, field_meta_->len());
      } else {
        bitmap[field_index / 8] &= static_cast<char>(~(1 << (field_index % 8)));
      }
    }

    if (!value_.is_null()) {
      // 将字段的新值写入新记录对应位置
      size_t copy_len = field_meta_->len();
      if (field_meta_->type() == AttrType::CHARS) {
        memset(new_record.data() + field_meta_->offset(), 0, field_meta_->len());
        copy_len = std::min(static_cast<size_t>(field_meta_->len()), static_cast<size_t>(value_.length()));
      }
      memcpy(new_record.data() + field_meta_->offset(), value_.data(), copy_len);
    }

    rc = trx_->update_record(table_, old_record, new_record);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to update record: %s", strrc(rc));
      return rc;
    }
  }

  return RC::SUCCESS;
}

RC UpdatePhysicalOperator::next()
{
  return RC::RECORD_EOF;
}

RC UpdatePhysicalOperator::close()
{
  return RC::SUCCESS;
}
