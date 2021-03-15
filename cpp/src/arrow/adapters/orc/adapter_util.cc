// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "arrow/adapters/orc/adapter_util.h"

#include <cmath>
#include <string>
#include <vector>

#include "arrow/array/builder_base.h"
#include "arrow/builder.h"
#include "arrow/chunked_array.h"
#include "arrow/scalar.h"
#include "arrow/status.h"
#include "arrow/util/checked_cast.h"
#include "arrow/util/decimal.h"
#include "arrow/util/range.h"
#include "orc/Exceptions.hh"
#include "orc/MemoryPool.hh"
#include "orc/OrcFile.hh"

// The number of milliseconds, microseconds and nanoseconds in a second
constexpr int64_t kOneSecondMillis = 1000LL;
constexpr int64_t kOneMicroNanos = 1000LL;
constexpr int64_t kOneSecondMicros = 1000000LL;
constexpr int64_t kOneMilliNanos = 1000000LL;
constexpr int64_t kOneSecondNanos = 1000000000LL;
// alias to not interfere with nested orc namespace
namespace liborc = orc;

namespace {

using arrow::internal::checked_cast;

arrow::Status AppendStructBatch(const liborc::Type* type,
                                liborc::ColumnVectorBatch* column_vector_batch,
                                int64_t offset, int64_t length,
                                arrow::ArrayBuilder* abuilder) {
  auto builder = checked_cast<arrow::StructBuilder*>(abuilder);
  auto batch = checked_cast<liborc::StructVectorBatch*>(column_vector_batch);

  const uint8_t* valid_bytes = nullptr;
  if (batch->hasNulls) {
    valid_bytes = reinterpret_cast<const uint8_t*>(batch->notNull.data()) + offset;
  }
  RETURN_NOT_OK(builder->AppendValues(length, valid_bytes));

  for (int i = 0; i < builder->num_fields(); i++) {
    RETURN_NOT_OK(arrow::adapters::orc::AppendBatch(type->getSubtype(i), batch->fields[i],
                                                    offset, length,
                                                    builder->field_builder(i)));
  }
  return arrow::Status::OK();
}

arrow::Status AppendListBatch(const liborc::Type* type,
                              liborc::ColumnVectorBatch* column_vector_batch,
                              int64_t offset, int64_t length,
                              arrow::ArrayBuilder* abuilder) {
  auto builder = checked_cast<arrow::ListBuilder*>(abuilder);
  auto batch = checked_cast<liborc::ListVectorBatch*>(column_vector_batch);
  liborc::ColumnVectorBatch* elements = batch->elements.get();
  const liborc::Type* elemtype = type->getSubtype(0);

  const bool has_nulls = batch->hasNulls;
  for (int64_t i = offset; i < length + offset; i++) {
    if (!has_nulls || batch->notNull[i]) {
      int64_t start = batch->offsets[i];
      int64_t end = batch->offsets[i + 1];
      RETURN_NOT_OK(builder->Append());
      RETURN_NOT_OK(arrow::adapters::orc::AppendBatch(
          elemtype, elements, start, end - start, builder->value_builder()));
    } else {
      RETURN_NOT_OK(builder->AppendNull());
    }
  }
  return arrow::Status::OK();
}

arrow::Status AppendMapBatch(const liborc::Type* type,
                             liborc::ColumnVectorBatch* column_vector_batch,
                             int64_t offset, int64_t length,
                             arrow::ArrayBuilder* abuilder) {
  auto builder = checked_cast<arrow::MapBuilder*>(abuilder);
  auto batch = checked_cast<liborc::MapVectorBatch*>(column_vector_batch);
  liborc::ColumnVectorBatch* keys = batch->keys.get();
  liborc::ColumnVectorBatch* items = batch->elements.get();
  const liborc::Type* key_type = type->getSubtype(0);
  const liborc::Type* item_type = type->getSubtype(1);

  const bool has_nulls = batch->hasNulls;
  for (int64_t i = offset; i < length + offset; i++) {
    if (!has_nulls || batch->notNull[i]) {
      int64_t start = batch->offsets[i];
      int64_t end = batch->offsets[i + 1];
      RETURN_NOT_OK(builder->Append());
      RETURN_NOT_OK(arrow::adapters::orc::AppendBatch(key_type, keys, start, end - start,
                                                      builder->key_builder()));
      RETURN_NOT_OK(arrow::adapters::orc::AppendBatch(
          item_type, items, start, end - start, builder->item_builder()));
    } else {
      RETURN_NOT_OK(builder->AppendNull());
    }
  }
  return arrow::Status::OK();
}

template <class BuilderType, class BatchType, class ElemType>
arrow::Status AppendNumericBatch(liborc::ColumnVectorBatch* column_vector_batch,
                                 int64_t offset, int64_t length,
                                 arrow::ArrayBuilder* abuilder) {
  auto builder = checked_cast<BuilderType*>(abuilder);
  auto batch = checked_cast<BatchType*>(column_vector_batch);

  if (length == 0) {
    return arrow::Status::OK();
  }
  const uint8_t* valid_bytes = nullptr;
  if (batch->hasNulls) {
    valid_bytes = reinterpret_cast<const uint8_t*>(batch->notNull.data()) + offset;
  }
  const ElemType* source = batch->data.data() + offset;
  RETURN_NOT_OK(builder->AppendValues(source, length, valid_bytes));
  return arrow::Status::OK();
}

template <class BuilderType, class TargetType, class BatchType, class SourceType>
arrow::Status AppendNumericBatchCast(liborc::ColumnVectorBatch* column_vector_batch,
                                     int64_t offset, int64_t length,
                                     arrow::ArrayBuilder* abuilder) {
  auto builder = checked_cast<BuilderType*>(abuilder);
  auto batch = checked_cast<BatchType*>(column_vector_batch);

  if (length == 0) {
    return arrow::Status::OK();
  }

  const uint8_t* valid_bytes = nullptr;
  if (batch->hasNulls) {
    valid_bytes = reinterpret_cast<const uint8_t*>(batch->notNull.data()) + offset;
  }
  const SourceType* source = batch->data.data() + offset;
  auto cast_iter = arrow::internal::MakeLazyRange(
      [&source](int64_t index) { return static_cast<TargetType>(source[index]); },
      length);

  RETURN_NOT_OK(builder->AppendValues(cast_iter.begin(), cast_iter.end(), valid_bytes));

  return arrow::Status::OK();
}

arrow::Status AppendBoolBatch(liborc::ColumnVectorBatch* column_vector_batch,
                              int64_t offset, int64_t length,
                              arrow::ArrayBuilder* abuilder) {
  auto builder = checked_cast<arrow::BooleanBuilder*>(abuilder);
  auto batch = checked_cast<liborc::LongVectorBatch*>(column_vector_batch);

  if (length == 0) {
    return arrow::Status::OK();
  }

  const uint8_t* valid_bytes = nullptr;
  if (batch->hasNulls) {
    valid_bytes = reinterpret_cast<const uint8_t*>(batch->notNull.data()) + offset;
  }
  const int64_t* source = batch->data.data() + offset;

  auto cast_iter = arrow::internal::MakeLazyRange(
      [&source](int64_t index) { return static_cast<bool>(source[index]); }, length);

  RETURN_NOT_OK(builder->AppendValues(cast_iter.begin(), cast_iter.end(), valid_bytes));

  return arrow::Status::OK();
}

arrow::Status AppendTimestampBatch(liborc::ColumnVectorBatch* column_vector_batch,
                                   int64_t offset, int64_t length,
                                   arrow::ArrayBuilder* abuilder) {
  auto builder = checked_cast<arrow::TimestampBuilder*>(abuilder);
  auto batch = checked_cast<liborc::TimestampVectorBatch*>(column_vector_batch);

  if (length == 0) {
    return arrow::Status::OK();
  }

  const uint8_t* valid_bytes = nullptr;
  if (batch->hasNulls) {
    valid_bytes = reinterpret_cast<const uint8_t*>(batch->notNull.data()) + offset;
  }

  const int64_t* seconds = batch->data.data() + offset;
  const int64_t* nanos = batch->nanoseconds.data() + offset;

  auto transform_timestamp = [seconds, nanos](int64_t index) {
    return seconds[index] * kOneSecondNanos + nanos[index];
  };

  auto transform_range = arrow::internal::MakeLazyRange(transform_timestamp, length);

  RETURN_NOT_OK(
      builder->AppendValues(transform_range.begin(), transform_range.end(), valid_bytes));
  return arrow::Status::OK();
}

template <class BuilderType>
arrow::Status AppendBinaryBatch(liborc::ColumnVectorBatch* column_vector_batch,
                                int64_t offset, int64_t length,
                                arrow::ArrayBuilder* abuilder) {
  auto builder = checked_cast<BuilderType*>(abuilder);
  auto batch = checked_cast<liborc::StringVectorBatch*>(column_vector_batch);

  const bool has_nulls = batch->hasNulls;
  for (int64_t i = offset; i < length + offset; i++) {
    if (!has_nulls || batch->notNull[i]) {
      RETURN_NOT_OK(
          builder->Append(batch->data[i], static_cast<int32_t>(batch->length[i])));
    } else {
      RETURN_NOT_OK(builder->AppendNull());
    }
  }
  return arrow::Status::OK();
}

arrow::Status AppendFixedBinaryBatch(liborc::ColumnVectorBatch* column_vector_batch,
                                     int64_t offset, int64_t length,
                                     arrow::ArrayBuilder* abuilder) {
  auto builder = checked_cast<arrow::FixedSizeBinaryBuilder*>(abuilder);
  auto batch = checked_cast<liborc::StringVectorBatch*>(column_vector_batch);

  const bool has_nulls = batch->hasNulls;
  for (int64_t i = offset; i < length + offset; i++) {
    if (!has_nulls || batch->notNull[i]) {
      RETURN_NOT_OK(builder->Append(batch->data[i]));
    } else {
      RETURN_NOT_OK(builder->AppendNull());
    }
  }
  return arrow::Status::OK();
}

arrow::Status AppendDecimalBatch(const liborc::Type* type,
                                 liborc::ColumnVectorBatch* column_vector_batch,
                                 int64_t offset, int64_t length,
                                 arrow::ArrayBuilder* abuilder) {
  auto builder = checked_cast<arrow::Decimal128Builder*>(abuilder);

  const bool has_nulls = column_vector_batch->hasNulls;
  if (type->getPrecision() == 0 || type->getPrecision() > 18) {
    auto batch = checked_cast<liborc::Decimal128VectorBatch*>(column_vector_batch);
    for (int64_t i = offset; i < length + offset; i++) {
      if (!has_nulls || batch->notNull[i]) {
        RETURN_NOT_OK(builder->Append(arrow::Decimal128(batch->values[i].getHighBits(),
                                                        batch->values[i].getLowBits())));
      } else {
        RETURN_NOT_OK(builder->AppendNull());
      }
    }
  } else {
    auto batch = checked_cast<liborc::Decimal64VectorBatch*>(column_vector_batch);
    for (int64_t i = offset; i < length + offset; i++) {
      if (!has_nulls || batch->notNull[i]) {
        RETURN_NOT_OK(builder->Append(arrow::Decimal128(batch->values[i])));
      } else {
        RETURN_NOT_OK(builder->AppendNull());
      }
    }
  }
  return arrow::Status::OK();
}
}  // namespace

namespace arrow {

namespace adapters {

namespace orc {

Status AppendBatch(const liborc::Type* type, liborc::ColumnVectorBatch* batch,
                   int64_t offset, int64_t length, arrow::ArrayBuilder* builder) {
  if (type == nullptr) {
    return arrow::Status::OK();
  }
  liborc::TypeKind kind = type->getKind();
  switch (kind) {
    case liborc::STRUCT:
      return AppendStructBatch(type, batch, offset, length, builder);
    case liborc::LIST:
      return AppendListBatch(type, batch, offset, length, builder);
    case liborc::MAP:
      return AppendMapBatch(type, batch, offset, length, builder);
    case liborc::LONG:
      return AppendNumericBatch<Int64Builder, liborc::LongVectorBatch, int64_t>(
          batch, offset, length, builder);
    case liborc::INT:
      return AppendNumericBatchCast<Int32Builder, int32_t, liborc::LongVectorBatch,
                                    int64_t>(batch, offset, length, builder);
    case liborc::SHORT:
      return AppendNumericBatchCast<Int16Builder, int16_t, liborc::LongVectorBatch,
                                    int64_t>(batch, offset, length, builder);
    case liborc::BYTE:
      return AppendNumericBatchCast<Int8Builder, int8_t, liborc::LongVectorBatch,
                                    int64_t>(batch, offset, length, builder);
    case liborc::DOUBLE:
      return AppendNumericBatch<DoubleBuilder, liborc::DoubleVectorBatch, double>(
          batch, offset, length, builder);
    case liborc::FLOAT:
      return AppendNumericBatchCast<FloatBuilder, float, liborc::DoubleVectorBatch,
                                    double>(batch, offset, length, builder);
    case liborc::BOOLEAN:
      return AppendBoolBatch(batch, offset, length, builder);
    case liborc::VARCHAR:
    case liborc::STRING:
      return AppendBinaryBatch<StringBuilder>(batch, offset, length, builder);
    case liborc::BINARY:
      return AppendBinaryBatch<BinaryBuilder>(batch, offset, length, builder);
    case liborc::CHAR:
      return AppendFixedBinaryBatch(batch, offset, length, builder);
    case liborc::DATE:
      return AppendNumericBatchCast<Date32Builder, int32_t, liborc::LongVectorBatch,
                                    int64_t>(batch, offset, length, builder);
    case liborc::TIMESTAMP:
      return AppendTimestampBatch(batch, offset, length, builder);
    case liborc::DECIMAL:
      return AppendDecimalBatch(type, batch, offset, length, builder);
    default:
      return Status::NotImplemented("Not implemented type kind: ", kind);
  }
}
}  // namespace orc
}  // namespace adapters
}  // namespace arrow

namespace {

using arrow::internal::checked_cast;

arrow::Status WriteBatch(liborc::ColumnVectorBatch* column_vector_batch,
                         int64_t* arrow_offset, int64_t* orc_offset,
                         const int64_t& length, const arrow::Array& parray,
                         const std::vector<bool>* incoming_mask = nullptr);

// incoming_mask is exclusively used by FillStructBatch. The cause is that ORC is much
// stricter than Arrow in terms of consistency. In this case if a struct scalar is null
// all its children must be set to null or ORC is not going to function properly. This is
// why I added incoming_mask to pass on null status from a struct to its children.
//
// static_cast from int64_t or double to itself shouldn't introduce overhead
// Pleae see
// https://stackoverflow.com/questions/19106826/
// can-static-cast-to-same-type-introduce-runtime-overhead
template <class ArrayType, class BatchType, class TargetType>
arrow::Status WriteNumericBatch(liborc::ColumnVectorBatch* column_vector_batch,
                                int64_t* arrow_offset, int64_t* orc_offset,
                                const int64_t& length, const arrow::Array& array,
                                const std::vector<bool>* incoming_mask) {
  const ArrayType& numeric_array(checked_cast<const ArrayType&>(array));
  auto batch = checked_cast<BatchType*>(column_vector_batch);
  int64_t arrow_length = array.length();
  if (!arrow_length) {
    return arrow::Status::OK();
  }
  if (array.null_count() || incoming_mask) {
    batch->hasNulls = true;
  }
  for (; *orc_offset < length && *arrow_offset < arrow_length;
       (*orc_offset)++, (*arrow_offset)++) {
    if (array.IsNull(*arrow_offset) ||
        (incoming_mask && !(*incoming_mask)[*orc_offset])) {
      batch->notNull[*orc_offset] = false;
    } else {
      batch->data[*orc_offset] =
          static_cast<TargetType>(numeric_array.Value(*arrow_offset));
      batch->notNull[*orc_offset] = true;
    }
  }
  batch->numElements = *orc_offset;
  return arrow::Status::OK();
}

template <class ArrayType>
arrow::Status WriteTimestampBatch(liborc::ColumnVectorBatch* column_vector_batch,
                                  int64_t* arrow_offset, int64_t* orc_offset,
                                  const int64_t& length, const arrow::Array& array,
                                  const std::vector<bool>* incoming_mask,
                                  const int64_t& conversion_factor_from_second,
                                  const int64_t& conversion_factor_to_nano) {
  const ArrayType& timestamp_array(checked_cast<const ArrayType&>(array));
  auto batch = checked_cast<liborc::TimestampVectorBatch*>(column_vector_batch);
  int64_t arrow_length = array.length();
  if (!arrow_length) {
    return arrow::Status::OK();
  }
  if (array.null_count() || incoming_mask) {
    batch->hasNulls = true;
  }
  for (; *orc_offset < length && *arrow_offset < arrow_length;
       (*orc_offset)++, (*arrow_offset)++) {
    if (array.IsNull(*arrow_offset) ||
        (incoming_mask && !(*incoming_mask)[*orc_offset])) {
      batch->notNull[*orc_offset] = false;
    } else {
      int64_t data = timestamp_array.Value(*arrow_offset);
      batch->notNull[*orc_offset] = true;
      batch->data[*orc_offset] =
          static_cast<int64_t>(std::floor(data / conversion_factor_from_second));
      batch->nanoseconds[*orc_offset] =
          (data - conversion_factor_from_second * batch->data[*orc_offset]) *
          conversion_factor_to_nano;
    }
  }
  batch->numElements = *orc_offset;
  return arrow::Status::OK();
}

template <class ArrayType, class OffsetType>
arrow::Status WriteBinaryBatch(liborc::ColumnVectorBatch* column_vector_batch,
                               int64_t* arrow_offset, int64_t* orc_offset,
                               const int64_t& length, const arrow::Array& array,
                               const std::vector<bool>* incoming_mask) {
  const ArrayType& binary_array(checked_cast<const ArrayType&>(array));
  auto batch = checked_cast<liborc::StringVectorBatch*>(column_vector_batch);
  int64_t arrow_length = array.length();
  if (!arrow_length) {
    return arrow::Status::OK();
  }
  if (array.null_count() || incoming_mask) {
    batch->hasNulls = true;
  }
  for (; *orc_offset < length && *arrow_offset < arrow_length;
       (*orc_offset)++, (*arrow_offset)++) {
    if (array.IsNull(*arrow_offset) ||
        (incoming_mask && !(*incoming_mask)[*orc_offset])) {
      batch->notNull[*orc_offset] = false;
    } else {
      batch->notNull[*orc_offset] = true;
      OffsetType data_length = 0;
      const uint8_t* data = binary_array.GetValue(*arrow_offset, &data_length);
      if (batch->data[*orc_offset]) delete batch->data[*orc_offset];
      batch->data[*orc_offset] = new char[data_length];  // Do not include null
      memcpy(batch->data[*orc_offset], data, data_length);
      batch->length[*orc_offset] = data_length;
    }
  }
  batch->numElements = *orc_offset;
  return arrow::Status::OK();
}

arrow::Status WriteFixedSizeBinaryBatch(liborc::ColumnVectorBatch* column_vector_batch,
                                        int64_t* arrow_offset, int64_t* orc_offset,
                                        const int64_t& length, const arrow::Array& array,
                                        const std::vector<bool>* incoming_mask) {
  const arrow::FixedSizeBinaryArray& fixed_size_binary_array(
      checked_cast<const arrow::FixedSizeBinaryArray&>(array));
  auto batch = checked_cast<liborc::StringVectorBatch*>(column_vector_batch);
  int64_t arrow_length = array.length();
  if (!arrow_length) {
    return arrow::Status::OK();
  }
  const int32_t data_length = fixed_size_binary_array.byte_width();
  if (array.null_count() || incoming_mask) {
    batch->hasNulls = true;
  }
  for (; *orc_offset < length && *arrow_offset < arrow_length;
       (*orc_offset)++, (*arrow_offset)++) {
    if (array.IsNull(*arrow_offset) ||
        (incoming_mask && !(*incoming_mask)[*orc_offset])) {
      batch->notNull[*orc_offset] = false;
    } else {
      batch->notNull[*orc_offset] = true;
      const uint8_t* data = fixed_size_binary_array.GetValue(*arrow_offset);
      if (batch->data[*orc_offset]) delete batch->data[*orc_offset];
      batch->data[*orc_offset] = new char[data_length];  // Do not include null
      memcpy(batch->data[*orc_offset], data, data_length);
      batch->length[*orc_offset] = data_length;
    }
  }
  batch->numElements = *orc_offset;
  return arrow::Status::OK();
}

// If Arrow supports 256-bit decimals we can not support it unless ORC does it
arrow::Status WriteDecimal64Batch(liborc::ColumnVectorBatch* column_vector_batch,
                                  int64_t* arrow_offset, int64_t* orc_offset,
                                  const int64_t& length, const arrow::Array& array,
                                  const std::vector<bool>* incoming_mask) {
  const arrow::Decimal128Array& decimal128_array(
      checked_cast<const arrow::Decimal128Array&>(array));
  auto batch = checked_cast<liborc::Decimal64VectorBatch*>(column_vector_batch);
  // Arrow uses 128 bits for decimal type and in the future, 256 bits will also be
  // supported.
  int64_t arrow_length = array.length();
  if (!arrow_length) {
    return arrow::Status::OK();
  }
  if (array.null_count() || incoming_mask) {
    batch->hasNulls = true;
  }
  for (; *orc_offset < length && *arrow_offset < arrow_length;
       (*orc_offset)++, (*arrow_offset)++) {
    if (array.IsNull(*arrow_offset) ||
        (incoming_mask && !(*incoming_mask)[*orc_offset])) {
      batch->notNull[*orc_offset] = false;
    } else {
      batch->notNull[*orc_offset] = true;
      uint8_t* raw_int128 =
          const_cast<uint8_t*>(decimal128_array.GetValue(*arrow_offset));
      int64_t* lower_bits = reinterpret_cast<int64_t*>(raw_int128);
      batch->values[*orc_offset] = *lower_bits;
    }
  }
  batch->numElements = *orc_offset;
  return arrow::Status::OK();
}

arrow::Status WriteDecimal128Batch(liborc::ColumnVectorBatch* column_vector_batch,
                                   int64_t* arrow_offset, int64_t* orc_offset,
                                   const int64_t& length, const arrow::Array& array,
                                   const std::vector<bool>* incoming_mask) {
  const arrow::Decimal128Array& decimal128_array(
      checked_cast<const arrow::Decimal128Array&>(array));
  auto batch = checked_cast<liborc::Decimal128VectorBatch*>(column_vector_batch);
  // Arrow uses 128 bits for decimal type and in the future, 256 bits will also be
  // supported.
  int64_t arrow_length = array.length();
  if (!arrow_length) {
    return arrow::Status::OK();
  }
  if (array.null_count() || incoming_mask) {
    batch->hasNulls = true;
  }
  for (; *orc_offset < length && *arrow_offset < arrow_length;
       (*orc_offset)++, (*arrow_offset)++) {
    if (array.IsNull(*arrow_offset) ||
        (incoming_mask && !(*incoming_mask)[*orc_offset])) {
      batch->notNull[*orc_offset] = false;
    } else {
      batch->notNull[*orc_offset] = true;
      uint8_t* raw_int128 =
          const_cast<uint8_t*>(decimal128_array.GetValue(*arrow_offset));
      uint64_t* lower_bits = reinterpret_cast<uint64_t*>(raw_int128);
      int64_t* higher_bits = reinterpret_cast<int64_t*>(raw_int128 + 8);
      batch->values[*orc_offset] = liborc::Int128(*higher_bits, *lower_bits);
    }
  }
  batch->numElements = *orc_offset;
  return arrow::Status::OK();
}

arrow::Status WriteStructBatch(liborc::ColumnVectorBatch* column_vector_batch,
                               int64_t* arrow_offset, int64_t* orc_offset,
                               const int64_t& length, const arrow::Array& array,
                               const std::vector<bool>* incoming_mask) {
  const arrow::StructArray& struct_array(checked_cast<const arrow::StructArray&>(array));
  auto batch = checked_cast<liborc::StructVectorBatch*>(column_vector_batch);
  std::shared_ptr<std::vector<bool>> outgoing_mask;
  std::size_t size = array.type()->fields().size();
  int64_t arrow_length = array.length();
  if (!arrow_length) {
    return arrow::Status::OK();
  }
  const int64_t init_orc_offset = *orc_offset;
  const int64_t init_arrow_offset = *arrow_offset;
  // First fill fields of ColumnVectorBatch
  if (array.null_count() || incoming_mask) {
    batch->hasNulls = true;
    outgoing_mask = std::make_shared<std::vector<bool>>(length, true);
  } else {
    outgoing_mask = nullptr;
  }
  for (; *orc_offset < length && *arrow_offset < arrow_length;
       (*orc_offset)++, (*arrow_offset)++) {
    if (array.IsNull(*arrow_offset) ||
        (incoming_mask && !(*incoming_mask)[*orc_offset])) {
      batch->notNull[*orc_offset] = false;
      (*outgoing_mask)[*orc_offset] = false;
    } else {
      batch->notNull[*orc_offset] = true;
    }
  }
  batch->numElements += *orc_offset - init_orc_offset;
  // Fill the fields
  for (std::size_t i = 0; i < size; i++) {
    *orc_offset = init_orc_offset;
    *arrow_offset = init_arrow_offset;
    batch->fields[i]->resize(length);
    RETURN_NOT_OK(WriteBatch(batch->fields[i], arrow_offset, orc_offset, length,
                             *(struct_array.field(i)), outgoing_mask.get()));
  }
  return arrow::Status::OK();
}

template <class ArrayType>
arrow::Status WriteListBatch(liborc::ColumnVectorBatch* column_vector_batch,
                             int64_t* arrow_offset, int64_t* orc_offset,
                             const int64_t& length, const arrow::Array& array,
                             const std::vector<bool>* incoming_mask) {
  const ArrayType& list_array(checked_cast<const ArrayType&>(array));
  auto batch = checked_cast<liborc::ListVectorBatch*>(column_vector_batch);
  liborc::ColumnVectorBatch* element_batch = (batch->elements).get();
  int64_t arrow_length = array.length();
  if (!arrow_length) {
    return arrow::Status::OK();
  }
  if (*orc_offset == 0) {
    batch->offsets[0] = 0;
  }
  if (array.null_count() || incoming_mask) {
    batch->hasNulls = true;
  }
  for (; *orc_offset < length && *arrow_offset < arrow_length;
       (*orc_offset)++, (*arrow_offset)++) {
    if (array.IsNull(*arrow_offset) ||
        (incoming_mask && !(*incoming_mask)[*orc_offset])) {
      batch->notNull[*orc_offset] = false;
      batch->offsets[*orc_offset + 1] = batch->offsets[*orc_offset];
    } else {
      batch->notNull[*orc_offset] = true;
      batch->offsets[*orc_offset + 1] = batch->offsets[*orc_offset] +
                                        list_array.value_offset(*arrow_offset + 1) -
                                        list_array.value_offset(*arrow_offset);
      element_batch->resize(batch->offsets[*orc_offset + 1]);
      int64_t subarray_arrow_offset = list_array.value_offset(*arrow_offset),
              subarray_orc_offset = batch->offsets[*orc_offset],
              subarray_orc_length = batch->offsets[*orc_offset + 1];
      RETURN_NOT_OK(WriteBatch(element_batch, &subarray_arrow_offset,
                               &subarray_orc_offset, subarray_orc_length,
                               *(list_array.values()), nullptr));
    }
  }
  batch->numElements = *orc_offset;
  return arrow::Status::OK();
}

arrow::Status WriteMapBatch(liborc::ColumnVectorBatch* column_vector_batch,
                            int64_t* arrow_offset, int64_t* orc_offset,
                            const int64_t& length, const arrow::Array& array,
                            const std::vector<bool>* incoming_mask) {
  const arrow::MapArray& map_array(checked_cast<const arrow::MapArray&>(array));
  auto batch = checked_cast<liborc::MapVectorBatch*>(column_vector_batch);
  liborc::ColumnVectorBatch* key_batch = (batch->keys).get();
  liborc::ColumnVectorBatch* element_batch = (batch->elements).get();
  std::shared_ptr<arrow::Array> key_array = map_array.keys();
  std::shared_ptr<arrow::Array> element_array = map_array.items();
  int64_t arrow_length = array.length();
  if (!arrow_length) {
    return arrow::Status::OK();
  }
  if (*orc_offset == 0) {
    batch->offsets[0] = 0;
  }
  if (array.null_count() || incoming_mask) {
    batch->hasNulls = true;
  }
  for (; *orc_offset < length && *arrow_offset < arrow_length;
       (*orc_offset)++, (*arrow_offset)++) {
    if (array.IsNull(*arrow_offset) ||
        (incoming_mask && !(*incoming_mask)[*orc_offset])) {
      batch->notNull[*orc_offset] = false;
      batch->offsets[*orc_offset + 1] = batch->offsets[*orc_offset];
    } else {
      batch->notNull[*orc_offset] = true;
      batch->offsets[*orc_offset + 1] = batch->offsets[*orc_offset] +
                                        map_array.value_offset(*arrow_offset + 1) -
                                        map_array.value_offset(*arrow_offset);
      int64_t subarray_arrow_offset = map_array.value_offset(*arrow_offset),
              subarray_orc_offset = batch->offsets[*orc_offset],
              subarray_orc_length = batch->offsets[*orc_offset + 1],
              init_subarray_arrow_offset = subarray_arrow_offset,
              init_subarray_orc_offset = subarray_orc_offset;
      key_batch->resize(subarray_orc_length);
      element_batch->resize(subarray_orc_length);
      RETURN_NOT_OK(WriteBatch(key_batch, &subarray_arrow_offset, &subarray_orc_offset,
                               subarray_orc_length, *key_array, nullptr));
      subarray_arrow_offset = init_subarray_arrow_offset;
      subarray_orc_offset = init_subarray_orc_offset;
      RETURN_NOT_OK(WriteBatch(element_batch, &subarray_arrow_offset,
                               &subarray_orc_offset, subarray_orc_length, *element_array,
                               nullptr));
    }
  }
  batch->numElements = *orc_offset;
  return arrow::Status::OK();
}

std::shared_ptr<arrow::DataType> DedictionizeType(
    const std::shared_ptr<arrow::DataType>& type) {
  arrow::Type::type kind = type->id();
  switch (kind) {
    case arrow::Type::type::DICTIONARY: {
      return std::static_pointer_cast<arrow::DictionaryType>(type)->value_type();
    }
    case arrow::Type::type::STRUCT: {
      std::vector<std::shared_ptr<arrow::Field>> fields = type->fields();
      std::size_t size = fields.size();
      std::vector<std::shared_ptr<arrow::Field>> new_fields(size, nullptr);
      for (std::size_t i = 0; i < size; i++) {
        std::shared_ptr<arrow::Field> field = fields[i];
        new_fields[i] = field->WithType(DedictionizeType(field->type()));
      }
      return struct_(new_fields);
    }
    case arrow::Type::type::LIST: {
      return list(DedictionizeType(
          std::static_pointer_cast<arrow::ListType>(type)->value_type()));
    }
    case arrow::Type::type::LARGE_LIST: {
      return large_list(DedictionizeType(
          std::static_pointer_cast<arrow::LargeListType>(type)->value_type()));
    }
    case arrow::Type::type::FIXED_SIZE_LIST: {
      auto fixed_size_list_type =
          std::static_pointer_cast<arrow::FixedSizeListType>(type);
      return fixed_size_list(DedictionizeType(fixed_size_list_type->value_type()),
                             fixed_size_list_type->list_size());
    }
    case arrow::Type::type::MAP: {
      auto map_type = std::static_pointer_cast<arrow::MapType>(type);
      return map(DedictionizeType(map_type->key_type()),
                 DedictionizeType(map_type->item_type()));
    }
    default: {  // No dict found!
      return type;
    }
  }
}

// arrow::Array DedictionizeArray(arrow::Array array) {
//   arrow::Type::type kind = array.type_id();
//   switch (kind) {
//     case arrow::Type::type::DICTIONARY:
//     case arrow::Type::type::STRUCT:
//     case arrow::Type::type::LIST:
//     case arrow::Type::type::LARGE_LIST:
//     case arrow::Type::type::FIXED_SIZE_LIST:
//     case arrow::Type::type::MAP:
//     default: {  // No dict found!
//       return array;
//     }
//   }
// }

arrow::Status WriteBatch(liborc::ColumnVectorBatch* column_vector_batch,
                         int64_t* arrow_offset, int64_t* orc_offset,
                         const int64_t& length, const arrow::Array& array,
                         const std::vector<bool>* incoming_mask) {
  arrow::Type::type kind = array.type_id();
  switch (kind) {
    case arrow::Type::type::BOOL:
      return WriteNumericBatch<arrow::BooleanArray, liborc::LongVectorBatch, int64_t>(
          column_vector_batch, arrow_offset, orc_offset, length, array, incoming_mask);
    case arrow::Type::type::INT8:
      return WriteNumericBatch<arrow::NumericArray<arrow::Int8Type>,
                               liborc::LongVectorBatch, int64_t>(
          column_vector_batch, arrow_offset, orc_offset, length, array, incoming_mask);
    case arrow::Type::type::INT16:
      return WriteNumericBatch<arrow::NumericArray<arrow::Int16Type>,
                               liborc::LongVectorBatch, int64_t>(
          column_vector_batch, arrow_offset, orc_offset, length, array, incoming_mask);
    case arrow::Type::type::INT32:
      return WriteNumericBatch<arrow::NumericArray<arrow::Int32Type>,
                               liborc::LongVectorBatch, int64_t>(
          column_vector_batch, arrow_offset, orc_offset, length, array, incoming_mask);
    case arrow::Type::type::INT64:
      return WriteNumericBatch<arrow::NumericArray<arrow::Int64Type>,
                               liborc::LongVectorBatch, int64_t>(
          column_vector_batch, arrow_offset, orc_offset, length, array, incoming_mask);
    case arrow::Type::type::FLOAT:
      return WriteNumericBatch<arrow::NumericArray<arrow::FloatType>,
                               liborc::DoubleVectorBatch, double>(
          column_vector_batch, arrow_offset, orc_offset, length, array, incoming_mask);
    case arrow::Type::type::DOUBLE:
      return WriteNumericBatch<arrow::NumericArray<arrow::DoubleType>,
                               liborc::DoubleVectorBatch, double>(
          column_vector_batch, arrow_offset, orc_offset, length, array, incoming_mask);
    case arrow::Type::type::BINARY:
      return WriteBinaryBatch<arrow::BinaryArray, int32_t>(
          column_vector_batch, arrow_offset, orc_offset, length, array, incoming_mask);
    case arrow::Type::type::LARGE_BINARY:
      return WriteBinaryBatch<arrow::LargeBinaryArray, int64_t>(
          column_vector_batch, arrow_offset, orc_offset, length, array, incoming_mask);
    case arrow::Type::type::STRING:
      return WriteBinaryBatch<arrow::StringArray, int32_t>(
          column_vector_batch, arrow_offset, orc_offset, length, array, incoming_mask);
    case arrow::Type::type::LARGE_STRING:
      return WriteBinaryBatch<arrow::LargeStringArray, int64_t>(
          column_vector_batch, arrow_offset, orc_offset, length, array, incoming_mask);
    case arrow::Type::type::FIXED_SIZE_BINARY:
      return WriteFixedSizeBinaryBatch(column_vector_batch, arrow_offset, orc_offset,
                                       length, array, incoming_mask);
    case arrow::Type::type::DATE32:
      return WriteNumericBatch<arrow::NumericArray<arrow::Date32Type>,
                               liborc::LongVectorBatch, int64_t>(
          column_vector_batch, arrow_offset, orc_offset, length, array, incoming_mask);
    case arrow::Type::type::DATE64:
      return WriteTimestampBatch<arrow::Date64Array>(
          column_vector_batch, arrow_offset, orc_offset, length, array, incoming_mask,
          kOneSecondMillis, kOneMilliNanos);
    case arrow::Type::type::TIMESTAMP: {
      switch (arrow::internal::checked_pointer_cast<arrow::TimestampType>(array.type())
                  ->unit()) {
        case arrow::TimeUnit::type::SECOND:
          return WriteTimestampBatch<arrow::TimestampArray>(
              column_vector_batch, arrow_offset, orc_offset, length, array, incoming_mask,
              1, kOneSecondNanos);
        case arrow::TimeUnit::type::MILLI:
          return WriteTimestampBatch<arrow::TimestampArray>(
              column_vector_batch, arrow_offset, orc_offset, length, array, incoming_mask,
              kOneSecondMillis, kOneMilliNanos);
        case arrow::TimeUnit::type::MICRO:
          return WriteTimestampBatch<arrow::TimestampArray>(
              column_vector_batch, arrow_offset, orc_offset, length, array, incoming_mask,
              kOneSecondMicros, kOneMicroNanos);
        case arrow::TimeUnit::type::NANO:
          return WriteTimestampBatch<arrow::TimestampArray>(
              column_vector_batch, arrow_offset, orc_offset, length, array, incoming_mask,
              kOneSecondNanos, 1);
        default:
          return arrow::Status::Invalid("Unknown or unsupported Arrow type: ",
                                        array.type()->ToString());
      }
    }
    case arrow::Type::type::DECIMAL128: {
      auto arrow_decimal_type =
          std::static_pointer_cast<arrow::DecimalType>(array.type());
      int32_t precision = arrow_decimal_type->precision();
      if (precision > 18) {
        return WriteDecimal128Batch(column_vector_batch, arrow_offset, orc_offset, length,
                                    array, incoming_mask);
      } else {
        return WriteDecimal64Batch(column_vector_batch, arrow_offset, orc_offset, length,
                                   array, incoming_mask);
      }
    }
    case arrow::Type::type::STRUCT:
      return WriteStructBatch(column_vector_batch, arrow_offset, orc_offset, length,
                              array, incoming_mask);
    case arrow::Type::type::LIST:
      return WriteListBatch<arrow::ListArray>(column_vector_batch, arrow_offset,
                                              orc_offset, length, array, incoming_mask);
    case arrow::Type::type::LARGE_LIST:
      return WriteListBatch<arrow::LargeListArray>(
          column_vector_batch, arrow_offset, orc_offset, length, array, incoming_mask);
    case arrow::Type::type::FIXED_SIZE_LIST:
      return WriteListBatch<arrow::FixedSizeListArray>(
          column_vector_batch, arrow_offset, orc_offset, length, array, incoming_mask);
    case arrow::Type::type::MAP:
      return WriteMapBatch(column_vector_batch, arrow_offset, orc_offset, length, array,
                           incoming_mask);
    default: {
      return arrow::Status::Invalid("Unknown or unsupported Arrow type: ",
                                    array.type()->ToString());
    }
  }
  return arrow::Status::OK();
}
}  // namespace

namespace arrow {

namespace adapters {

namespace orc {

using internal::checked_cast;
Status WriteBatch(liborc::ColumnVectorBatch* column_vector_batch,
                  int64_t* arrow_index_offset, int* arrow_chunk_offset, int64_t length,
                  const ChunkedArray& chunked_array) {
  int num_batch = chunked_array.num_chunks();
  int64_t orc_offset = 0;
  Status st;
  while (*arrow_chunk_offset < num_batch && orc_offset < length) {
    RETURN_NOT_OK(::WriteBatch(column_vector_batch, arrow_index_offset, &orc_offset,
                               length, *(chunked_array.chunk(*arrow_chunk_offset)),
                               nullptr));
    if (*arrow_chunk_offset < num_batch && orc_offset < length) {
      *arrow_index_offset = 0;
      (*arrow_chunk_offset)++;
    }
  }
  return arrow::Status::OK();
}

Status GetArrowType(const liborc::Type* type, std::shared_ptr<DataType>* out) {
  // When subselecting fields on read, liborc will set some nodes to nullptr,
  // so we need to check for nullptr before progressing
  if (type == nullptr) {
    *out = null();
    return arrow::Status::OK();
  }
  liborc::TypeKind kind = type->getKind();
  const int subtype_count = static_cast<int>(type->getSubtypeCount());

  switch (kind) {
    case liborc::BOOLEAN:
      *out = boolean();
      break;
    case liborc::BYTE:
      *out = int8();
      break;
    case liborc::SHORT:
      *out = int16();
      break;
    case liborc::INT:
      *out = int32();
      break;
    case liborc::LONG:
      *out = int64();
      break;
    case liborc::FLOAT:
      *out = float32();
      break;
    case liborc::DOUBLE:
      *out = float64();
      break;
    case liborc::VARCHAR:
    case liborc::STRING:
      *out = utf8();
      break;
    case liborc::BINARY:
      *out = binary();
      break;
    case liborc::CHAR:
      *out = fixed_size_binary(static_cast<int>(type->getMaximumLength()));
      break;
    case liborc::TIMESTAMP:
      *out = timestamp(TimeUnit::NANO);
      break;
    case liborc::DATE:
      *out = date32();
      break;
    case liborc::DECIMAL: {
      const int precision = static_cast<int>(type->getPrecision());
      const int scale = static_cast<int>(type->getScale());
      if (precision == 0) {
        // In HIVE 0.11/0.12 precision is set as 0, but means max precision
        *out = decimal128(38, 6);
      } else {
        *out = decimal128(precision, scale);
      }
      break;
    }
    case liborc::LIST: {
      if (subtype_count != 1) {
        return Status::Invalid("Invalid Orc List type");
      }
      std::shared_ptr<DataType> elemtype;
      RETURN_NOT_OK(GetArrowType(type->getSubtype(0), &elemtype));
      *out = list(elemtype);
      break;
    }
    case liborc::MAP: {
      if (subtype_count != 2) {
        return Status::Invalid("Invalid Orc Map type");
      }
      std::shared_ptr<DataType> key_type, item_type;
      RETURN_NOT_OK(GetArrowType(type->getSubtype(0), &key_type));
      RETURN_NOT_OK(GetArrowType(type->getSubtype(1), &item_type));
      *out = map(key_type, item_type);
      break;
    }
    case liborc::STRUCT: {
      std::vector<std::shared_ptr<Field>> fields;
      for (int child = 0; child < subtype_count; ++child) {
        std::shared_ptr<DataType> elem_type;
        RETURN_NOT_OK(GetArrowType(type->getSubtype(child), &elem_type));
        std::string name = type->getFieldName(child);
        fields.push_back(field(name, elem_type));
      }
      *out = struct_(fields);
      break;
    }
    case liborc::UNION: {
      std::vector<std::shared_ptr<Field>> fields;
      std::vector<int8_t> type_codes;
      for (int child = 0; child < subtype_count; ++child) {
        std::shared_ptr<DataType> elem_type;
        RETURN_NOT_OK(GetArrowType(type->getSubtype(child), &elem_type));
        fields.push_back(field("_union_" + std::to_string(child), elem_type));
        type_codes.push_back(static_cast<int8_t>(child));
      }
      *out = sparse_union(fields, type_codes);
      break;
    }
    default: {
      return Status::Invalid("Unknown Orc type kind: ", type->toString());
    }
  }
  return arrow::Status::OK();
}

Result<ORC_UNIQUE_PTR<liborc::Type>> GetORCType(const DataType& type) {
  Type::type kind = type.id();
  switch (kind) {
    case Type::type::BOOL:
      return liborc::createPrimitiveType(liborc::TypeKind::BOOLEAN);
    case Type::type::INT8:
      return liborc::createPrimitiveType(liborc::TypeKind::BYTE);
    case Type::type::INT16:
      return liborc::createPrimitiveType(liborc::TypeKind::SHORT);
    case Type::type::INT32:
      return liborc::createPrimitiveType(liborc::TypeKind::INT);
    case Type::type::INT64:
      return liborc::createPrimitiveType(liborc::TypeKind::LONG);
    case Type::type::FLOAT:
      return liborc::createPrimitiveType(liborc::TypeKind::FLOAT);
    case Type::type::DOUBLE:
      return liborc::createPrimitiveType(liborc::TypeKind::DOUBLE);
    // Use STRING instead of VARCHAR for now, both use UTF-8
    case Type::type::STRING:
    case Type::type::LARGE_STRING:
      return liborc::createPrimitiveType(liborc::TypeKind::STRING);
    case Type::type::BINARY:
    case Type::type::LARGE_BINARY:
    case Type::type::FIXED_SIZE_BINARY:
      return liborc::createPrimitiveType(liborc::TypeKind::BINARY);
    case Type::type::DATE32:
      return liborc::createPrimitiveType(liborc::TypeKind::DATE);
    case Type::type::DATE64:
    case Type::type::TIMESTAMP:
      return liborc::createPrimitiveType(liborc::TypeKind::TIMESTAMP);
    case Type::type::DECIMAL128: {
      const uint64_t precision =
          static_cast<uint64_t>(static_cast<const Decimal128Type&>(type).precision());
      const uint64_t scale =
          static_cast<uint64_t>(static_cast<const Decimal128Type&>(type).scale());
      return liborc::createDecimalType(precision, scale);
    }
    case Type::type::LIST:
    case Type::type::FIXED_SIZE_LIST:
    case Type::type::LARGE_LIST: {
      std::shared_ptr<DataType> arrow_child_type =
          static_cast<const BaseListType&>(type).value_type();
      ORC_UNIQUE_PTR<liborc::Type> orc_subtype =
          GetORCType(*arrow_child_type).ValueOrDie();
      return liborc::createListType(std::move(orc_subtype));
    }
    case Type::type::STRUCT: {
      ORC_UNIQUE_PTR<liborc::Type> out_type = liborc::createStructType();
      std::vector<std::shared_ptr<Field>> arrow_fields =
          checked_cast<const StructType&>(type).fields();
      for (std::vector<std::shared_ptr<Field>>::iterator it = arrow_fields.begin();
           it != arrow_fields.end(); ++it) {
        std::string field_name = (*it)->name();
        std::shared_ptr<DataType> arrow_child_type = (*it)->type();
        ORC_UNIQUE_PTR<liborc::Type> orc_subtype =
            GetORCType(*arrow_child_type).ValueOrDie();
        out_type->addStructField(field_name, std::move(orc_subtype));
      }
      return out_type;
    }
    case Type::type::MAP: {
      std::shared_ptr<DataType> key_arrow_type =
          checked_cast<const MapType&>(type).key_type();
      std::shared_ptr<DataType> item_arrow_type =
          checked_cast<const MapType&>(type).item_type();
      ORC_UNIQUE_PTR<liborc::Type> key_orc_type =
                                       GetORCType(*key_arrow_type).ValueOrDie(),
                                   item_orc_type =
                                       GetORCType(*item_arrow_type).ValueOrDie();
      return liborc::createMapType(std::move(key_orc_type), std::move(item_orc_type));
    }
    case Type::type::DENSE_UNION:
    case Type::type::SPARSE_UNION: {
      ORC_UNIQUE_PTR<liborc::Type> out_type = liborc::createUnionType();
      std::vector<std::shared_ptr<Field>> arrow_fields =
          checked_cast<const UnionType&>(type).fields();
      for (std::vector<std::shared_ptr<Field>>::iterator it = arrow_fields.begin();
           it != arrow_fields.end(); ++it) {
        std::string field_name = (*it)->name();
        std::shared_ptr<DataType> arrow_child_type = (*it)->type();
        ORC_UNIQUE_PTR<liborc::Type> orc_subtype =
            GetORCType(*arrow_child_type).ValueOrDie();
        out_type->addUnionChild(std::move(orc_subtype));
      }
      return out_type;
    }
    // Dictionary is an encoding method, not a TypeKind in ORC. Hence we need to get the
    // actual value type.
    case Type::type::DICTIONARY: {
      std::shared_ptr<DataType> arrow_value_type =
          checked_cast<const DictionaryType&>(type).value_type();
      return GetORCType(*arrow_value_type).ValueOrDie();
    }
    default: {
      return Status::Invalid("Unknown or unsupported Arrow type: ", type.ToString());
    }
  }
}

Result<ORC_UNIQUE_PTR<liborc::Type>> GetORCType(const Schema& schema) {
  int numFields = schema.num_fields();
  ORC_UNIQUE_PTR<liborc::Type> out_type = liborc::createStructType();
  for (int i = 0; i < numFields; i++) {
    std::shared_ptr<Field> field = schema.field(i);
    std::string field_name = field->name();
    std::shared_ptr<DataType> arrow_child_type = field->type();
    ORC_UNIQUE_PTR<liborc::Type> orc_subtype = GetORCType(*arrow_child_type).ValueOrDie();
    out_type->addStructField(field_name, std::move(orc_subtype));
  }
  return out_type;
}

}  // namespace orc
}  // namespace adapters
}  // namespace arrow
