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
#include "arrow/util/bitmap_ops.h"
#include "arrow/util/checked_cast.h"
#include "arrow/util/decimal.h"
#include "arrow/util/range.h"
#include "arrow/util/string_view.h"
#include "arrow/visitor_inline.h"
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

arrow::Status WriteBatch(const arrow::Array& parray, int64_t orc_offset,
                         liborc::ColumnVectorBatch* column_vector_batch,
                         bool normalized = false);

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

// Make sure children of StructArray have appropriate null.
std::shared_ptr<arrow::Array> NormalizeArray(const std::shared_ptr<arrow::Array>& array) {
  arrow::Type::type kind = array->type_id();
  switch (kind) {
    case arrow::Type::type::BOOL:
    case arrow::Type::type::INT8:
    case arrow::Type::type::INT16:
    case arrow::Type::type::INT32:
    case arrow::Type::type::INT64:
    case arrow::Type::type::FLOAT:
    case arrow::Type::type::DOUBLE:
    case arrow::Type::type::STRING:
    case arrow::Type::type::LARGE_STRING:
    case arrow::Type::type::BINARY:
    case arrow::Type::type::LARGE_BINARY:
    case arrow::Type::type::FIXED_SIZE_BINARY:
    case arrow::Type::type::DATE32:
    case arrow::Type::type::DATE64:
    case arrow::Type::type::TIMESTAMP:
    case arrow::Type::type::DECIMAL128: {
      return array;
    }
    case arrow::Type::type::STRUCT: {
      if (array->null_count() == 0) {
        return array;
      } else {
        auto struct_array = std::static_pointer_cast<arrow::StructArray>(array);
        const std::shared_ptr<arrow::Buffer> bitmap = struct_array->null_bitmap();
        std::shared_ptr<arrow::DataType> struct_type = struct_array->type();
        std::size_t size = struct_type->fields().size();
        std::vector<std::shared_ptr<arrow::Array>> new_children(size, nullptr);
        for (std::size_t i = 0; i < size; i++) {
          std::shared_ptr<arrow::Array> child = struct_array->field(i);
          const std::shared_ptr<arrow::Buffer> child_bitmap = child->null_bitmap();
          std::shared_ptr<arrow::Buffer> final_child_bitmap;
          if (child_bitmap == nullptr) {
            final_child_bitmap = bitmap;
          } else {
            final_child_bitmap = arrow::internal::BitmapAnd(
                                     arrow::default_memory_pool(), bitmap->data(), 0,
                                     child_bitmap->data(), 0, struct_array->length(), 0)
                                     .ValueOrDie();
          }
          std::shared_ptr<arrow::ArrayData> child_array_data = child->data();
          std::vector<std::shared_ptr<arrow::Buffer>> child_buffers =
              child_array_data->buffers;
          child_buffers[0] = final_child_bitmap;
          std::shared_ptr<arrow::ArrayData> new_child_array_data = arrow::ArrayData::Make(
              child->type(), child->length(), child_buffers, child_array_data->child_data,
              child_array_data->dictionary);
          new_children[i] = NormalizeArray(arrow::MakeArray(new_child_array_data));
        }
        return std::make_shared<arrow::StructArray>(struct_type, struct_array->length(),
                                                    new_children, bitmap);
      }
    }
    case arrow::Type::type::LIST: {
      auto list_array = std::static_pointer_cast<arrow::ListArray>(array);
      return std::make_shared<arrow::ListArray>(
          list_array->type(), list_array->length(), list_array->value_offsets(),
          NormalizeArray(list_array->values()), list_array->null_bitmap());
    }
    case arrow::Type::type::LARGE_LIST: {
      auto list_array = std::static_pointer_cast<arrow::LargeListArray>(array);
      return std::make_shared<arrow::LargeListArray>(
          list_array->type(), list_array->length(), list_array->value_offsets(),
          NormalizeArray(list_array->values()), list_array->null_bitmap());
    }
    case arrow::Type::type::FIXED_SIZE_LIST: {
      auto list_array = std::static_pointer_cast<arrow::FixedSizeListArray>(array);
      return std::make_shared<arrow::FixedSizeListArray>(
          list_array->type(), list_array->length(), NormalizeArray(list_array->values()),
          list_array->null_bitmap());
    }
    case arrow::Type::type::MAP: {
      auto map_array = std::static_pointer_cast<arrow::MapArray>(array);
      return std::make_shared<arrow::MapArray>(
          map_array->type(), map_array->length(), map_array->value_offsets(),
          NormalizeArray(map_array->keys()), NormalizeArray(map_array->items()),
          map_array->null_bitmap());
    }
    default: {
      return array;
    }
  }
}

template <class DataType, class BatchType, typename Enable = void>
struct Appender {};

// Types for long/double-like Appender, that is, numeric, boolean or date32
template <typename T>
using is_generic_type =
    std::integral_constant<bool, arrow::is_number_type<T>::value ||
                                     std::is_same<arrow::Date32Type, T>::value ||
                                     arrow::is_boolean_type<T>::value>;
template <typename T, typename R = void>
using enable_if_generic = arrow::enable_if_t<is_generic_type<T>::value, R>;

// Number-like
template <class DataType, class BatchType>
struct Appender<DataType, BatchType, enable_if_generic<DataType>> {
  using ArrayType = typename arrow::TypeTraits<DataType>::ArrayType;
  using ValueType = typename arrow::TypeTraits<DataType>::CType;
  arrow::Status VisitNull() {
    batch->notNull[running_orc_offset] = false;
    running_orc_offset++;
    running_arrow_offset++;
    return arrow::Status::OK();
  }
  arrow::Status VisitValue(ValueType v) {
    batch->data[running_orc_offset] = array.Value(running_arrow_offset);
    batch->notNull[running_orc_offset] = true;
    running_orc_offset++;
    running_arrow_offset++;
    return arrow::Status::OK();
  }
  const ArrayType& array;
  BatchType* batch;
  int64_t running_orc_offset, running_arrow_offset;
};

// Binary
template <class DataType>
struct Appender<DataType, liborc::StringVectorBatch> {
  using ArrayType = typename arrow::TypeTraits<DataType>::ArrayType;
  using COffsetType = typename arrow::TypeTraits<DataType>::OffsetType::c_type;
  arrow::Status VisitNull() {
    batch->notNull[running_orc_offset] = false;
    running_orc_offset++;
    running_arrow_offset++;
    return arrow::Status::OK();
  }
  arrow::Status VisitValue(arrow::util::string_view v) {
    batch->notNull[running_orc_offset] = true;
    COffsetType data_length = 0;
    batch->data[running_orc_offset] = reinterpret_cast<char*>(
        const_cast<uint8_t*>(array.GetValue(running_arrow_offset, &data_length)));
    batch->length[running_orc_offset] = data_length;
    running_orc_offset++;
    running_arrow_offset++;
    return arrow::Status::OK();
  }
  const ArrayType& array;
  liborc::StringVectorBatch* batch;
  int64_t running_orc_offset, running_arrow_offset;
};

// Decimal
template <>
struct Appender<arrow::Decimal128Type, liborc::Decimal64VectorBatch> {
  arrow::Status VisitNull() {
    batch->notNull[running_orc_offset] = false;
    running_orc_offset++;
    running_arrow_offset++;
    return arrow::Status::OK();
  }
  arrow::Status VisitValue(arrow::util::string_view v) {
    batch->notNull[running_orc_offset] = true;
    uint8_t* raw_int128 = const_cast<uint8_t*>(array.GetValue(running_arrow_offset));
    int64_t* lower_bits = reinterpret_cast<int64_t*>(raw_int128);
    batch->values[running_orc_offset] = *lower_bits;
    running_orc_offset++;
    running_arrow_offset++;
    return arrow::Status::OK();
  }
  const arrow::Decimal128Array& array;
  liborc::Decimal64VectorBatch* batch;
  int64_t running_orc_offset, running_arrow_offset;
};

template <>
struct Appender<arrow::Decimal128Type, liborc::Decimal128VectorBatch> {
  arrow::Status VisitNull() {
    batch->notNull[running_orc_offset] = false;
    running_orc_offset++;
    running_arrow_offset++;
    return arrow::Status::OK();
  }
  arrow::Status VisitValue(arrow::util::string_view v) {
    batch->notNull[running_orc_offset] = true;
    uint8_t* raw_int128 = const_cast<uint8_t*>(array.GetValue(running_arrow_offset));
    uint64_t* lower_bits = reinterpret_cast<uint64_t*>(raw_int128);
    int64_t* higher_bits = reinterpret_cast<int64_t*>(raw_int128 + 8);
    batch->values[running_orc_offset] = liborc::Int128(*higher_bits, *lower_bits);
    running_orc_offset++;
    running_arrow_offset++;
    return arrow::Status::OK();
  }
  const arrow::Decimal128Array& array;
  liborc::Decimal128VectorBatch* batch;
  int64_t running_orc_offset, running_arrow_offset;
};

// Date64 and Timestamp
template <class DataType>
struct TimestampAppender {
  using ArrayType = typename arrow::TypeTraits<DataType>::ArrayType;
  arrow::Status VisitNull() {
    batch->notNull[running_orc_offset] = false;
    running_orc_offset++;
    running_arrow_offset++;
    return arrow::Status::OK();
  }
  arrow::Status VisitValue(int64_t v) {
    int64_t data = array.Value(running_arrow_offset);
    batch->notNull[running_orc_offset] = true;
    batch->data[running_orc_offset] =
        static_cast<int64_t>(std::floor(data / conversion_factor_from_second));
    batch->nanoseconds[running_orc_offset] =
        (data - conversion_factor_from_second * batch->data[running_orc_offset]) *
        conversion_factor_to_nano;
    running_orc_offset++;
    running_arrow_offset++;
    return arrow::Status::OK();
  }
  const ArrayType& array;
  liborc::TimestampVectorBatch* batch;
  int64_t running_orc_offset, running_arrow_offset;
  int64_t conversion_factor_from_second, conversion_factor_to_nano;
};

// FSB
struct FixedSizeBinaryAppender {
  arrow::Status VisitNull() {
    batch->notNull[running_orc_offset] = false;
    running_orc_offset++;
    running_arrow_offset++;
    return arrow::Status::OK();
  }
  arrow::Status VisitValue(arrow::util::string_view v) {
    batch->notNull[running_orc_offset] = true;
    batch->data[running_orc_offset] = reinterpret_cast<char*>(
        const_cast<uint8_t*>(array.GetValue(running_arrow_offset)));
    batch->length[running_orc_offset] = data_length;
    running_orc_offset++;
    running_arrow_offset++;
    return arrow::Status::OK();
  }
  const arrow::FixedSizeBinaryArray& array;
  liborc::StringVectorBatch* batch;
  int64_t running_orc_offset, running_arrow_offset;
  const int32_t data_length;
};

// static_cast from int64_t or double to itself shouldn't introduce overhead
// Pleae see
// https://stackoverflow.com/questions/19106826/
// can-static-cast-to-same-type-introduce-runtime-overhead
template <class DataType, class BatchType>
arrow::Status WriteGenericBatch(const arrow::Array& array, int64_t orc_offset,
                                liborc::ColumnVectorBatch* column_vector_batch) {
  using ArrayType = typename arrow::TypeTraits<DataType>::ArrayType;
  const ArrayType& array_(checked_cast<const ArrayType&>(array));
  auto batch = checked_cast<BatchType*>(column_vector_batch);
  if (array.null_count()) {
    batch->hasNulls = true;
  }
  Appender<DataType, BatchType> appender{array_, batch, orc_offset, 0};
  arrow::ArrayDataVisitor<DataType> visitor;
  RETURN_NOT_OK(visitor.Visit(*(array_.data()), &appender));
  return arrow::Status::OK();
}

template <class DataType>
arrow::Status WriteTimestampBatch(const arrow::Array& array, int64_t orc_offset,
                                  liborc::ColumnVectorBatch* column_vector_batch,
                                  const int64_t& conversion_factor_from_second,
                                  const int64_t& conversion_factor_to_nano) {
  using ArrayType = typename arrow::TypeTraits<DataType>::ArrayType;
  const ArrayType& array_(checked_cast<const ArrayType&>(array));
  auto batch = checked_cast<liborc::TimestampVectorBatch*>(column_vector_batch);
  if (array.null_count()) {
    batch->hasNulls = true;
  }
  TimestampAppender<DataType> appender{array_,
                                       batch,
                                       orc_offset,
                                       0,
                                       conversion_factor_from_second,
                                       conversion_factor_to_nano};
  arrow::ArrayDataVisitor<DataType> visitor;
  RETURN_NOT_OK(visitor.Visit(*(array_.data()), &appender));
  return arrow::Status::OK();
}

arrow::Status WriteFixedSizeBinaryBatch(const arrow::Array& array, int64_t orc_offset,
                                        liborc::ColumnVectorBatch* column_vector_batch) {
  const arrow::FixedSizeBinaryArray& array_(
      checked_cast<const arrow::FixedSizeBinaryArray&>(array));
  auto batch = checked_cast<liborc::StringVectorBatch*>(column_vector_batch);
  if (array.null_count()) {
    batch->hasNulls = true;
  }
  FixedSizeBinaryAppender appender{array_, batch, orc_offset, 0, array_.byte_width()};
  arrow::ArrayDataVisitor<arrow::FixedSizeBinaryType> visitor;
  RETURN_NOT_OK(visitor.Visit(*(array_.data()), &appender));
  return arrow::Status::OK();
}

arrow::Status WriteStructBatch(const arrow::Array& array, int64_t orc_offset,
                               liborc::ColumnVectorBatch* column_vector_batch,
                               bool normalized) {
  std::shared_ptr<arrow::Array> array_ = arrow::MakeArray(array.data());
  std::shared_ptr<arrow::StructArray> struct_array(
      std::static_pointer_cast<arrow::StructArray>(array_));
  auto batch = checked_cast<liborc::StructVectorBatch*>(column_vector_batch);
  std::size_t size = array.type()->fields().size();
  int64_t arrow_length = array.length();
  int64_t running_arrow_offset = 0, running_orc_offset = orc_offset;
  // First fill fields of ColumnVectorBatch
  if (array.null_count()) {
    batch->hasNulls = true;
  }
  for (; running_arrow_offset < arrow_length;
       running_orc_offset++, running_arrow_offset++) {
    if (array.IsNull(running_arrow_offset)) {
      batch->notNull[running_orc_offset] = false;
    } else {
      batch->notNull[running_orc_offset] = true;
    }
  }
  // Fill the fields
  for (std::size_t i = 0; i < size; i++) {
    batch->fields[i]->resize(orc_offset + arrow_length);
    RETURN_NOT_OK(
        WriteBatch(*(struct_array->field(i)), orc_offset, batch->fields[i], true));
  }
  return arrow::Status::OK();
}

template <class ArrayType>
arrow::Status WriteListBatch(const arrow::Array& array, int64_t orc_offset,
                             liborc::ColumnVectorBatch* column_vector_batch) {
  const ArrayType& list_array(checked_cast<const ArrayType&>(array));
  auto batch = checked_cast<liborc::ListVectorBatch*>(column_vector_batch);
  liborc::ColumnVectorBatch* element_batch = (batch->elements).get();
  int64_t arrow_length = array.length();
  int64_t running_arrow_offset = 0, running_orc_offset = orc_offset;
  if (orc_offset == 0) {
    batch->offsets[0] = 0;
  }
  if (array.null_count()) {
    batch->hasNulls = true;
  }
  for (; running_arrow_offset < arrow_length;
       running_orc_offset++, running_arrow_offset++) {
    if (array.IsNull(running_arrow_offset)) {
      batch->notNull[running_orc_offset] = false;
      batch->offsets[running_orc_offset + 1] = batch->offsets[running_orc_offset];
    } else {
      batch->notNull[running_orc_offset] = true;
      batch->offsets[running_orc_offset + 1] =
          batch->offsets[running_orc_offset] +
          list_array.value_offset(running_arrow_offset + 1) -
          list_array.value_offset(running_arrow_offset);
      element_batch->resize(batch->offsets[running_orc_offset + 1]);
      int64_t subarray_arrow_offset = list_array.value_offset(running_arrow_offset),
              subarray_orc_offset = batch->offsets[running_orc_offset],
              subarray_orc_length =
                  batch->offsets[running_orc_offset + 1] - subarray_orc_offset;
      RETURN_NOT_OK(WriteBatch(
          *(list_array.values()->Slice(subarray_arrow_offset, subarray_orc_length)),
          subarray_orc_offset, element_batch));
    }
  }
  return arrow::Status::OK();
}

arrow::Status WriteMapBatch(const arrow::Array& array, int64_t orc_offset,
                            liborc::ColumnVectorBatch* column_vector_batch) {
  const arrow::MapArray& map_array(checked_cast<const arrow::MapArray&>(array));
  auto batch = checked_cast<liborc::MapVectorBatch*>(column_vector_batch);
  liborc::ColumnVectorBatch* key_batch = (batch->keys).get();
  liborc::ColumnVectorBatch* element_batch = (batch->elements).get();
  std::shared_ptr<arrow::Array> key_array = map_array.keys();
  std::shared_ptr<arrow::Array> element_array = map_array.items();
  int64_t arrow_length = array.length();
  int64_t running_arrow_offset = 0, running_orc_offset = orc_offset;
  if (orc_offset == 0) {
    batch->offsets[0] = 0;
  }
  if (array.null_count()) {
    batch->hasNulls = true;
  }
  for (; running_arrow_offset < arrow_length;
       running_orc_offset++, running_arrow_offset++) {
    if (array.IsNull(running_arrow_offset)) {
      batch->notNull[running_orc_offset] = false;
      batch->offsets[running_orc_offset + 1] = batch->offsets[running_orc_offset];
    } else {
      batch->notNull[running_orc_offset] = true;
      batch->offsets[running_orc_offset + 1] =
          batch->offsets[running_orc_offset] +
          map_array.value_offset(running_arrow_offset + 1) -
          map_array.value_offset(running_arrow_offset);
      int64_t subarray_arrow_offset = map_array.value_offset(running_arrow_offset),
              subarray_orc_offset = batch->offsets[running_orc_offset],
              new_subarray_orc_offset = batch->offsets[running_orc_offset + 1],
              subarray_orc_length = new_subarray_orc_offset - subarray_orc_offset;
      key_batch->resize(new_subarray_orc_offset);
      element_batch->resize(new_subarray_orc_offset);
      RETURN_NOT_OK(
          WriteBatch(*(key_array->Slice(subarray_arrow_offset, subarray_orc_length)),
                     subarray_orc_offset, key_batch));
      RETURN_NOT_OK(
          WriteBatch(*(element_array->Slice(subarray_arrow_offset, subarray_orc_length)),
                     subarray_orc_offset, element_batch));
    }
  }
  return arrow::Status::OK();
}

arrow::Status WriteBatch(liborc::ColumnVectorBatch* column_vector_batch,
                         int64_t* arrow_offset, int64_t* orc_offset,
                         const int64_t& length, const arrow::Array& array,
                         const std::vector<bool>* incoming_mask) {
  arrow::Type::type kind = array.type_id();
  column_vector_batch->numElements = orc_offset;
  switch (kind) {
    case arrow::Type::type::BOOL:
      return WriteGenericBatch<arrow::BooleanType, liborc::LongVectorBatch>(
          array, orc_offset, column_vector_batch);
    case arrow::Type::type::INT8:
      return WriteGenericBatch<arrow::Int8Type, liborc::LongVectorBatch>(
          array, orc_offset, column_vector_batch);
    case arrow::Type::type::INT16:
      return WriteGenericBatch<arrow::Int16Type, liborc::LongVectorBatch>(
          array, orc_offset, column_vector_batch);
    case arrow::Type::type::INT32:
      return WriteGenericBatch<arrow::Int32Type, liborc::LongVectorBatch>(
          array, orc_offset, column_vector_batch);
    case arrow::Type::type::INT64:
      return WriteGenericBatch<arrow::Int64Type, liborc::LongVectorBatch>(
          array, orc_offset, column_vector_batch);
    case arrow::Type::type::FLOAT:
      return WriteGenericBatch<arrow::FloatType, liborc::DoubleVectorBatch>(
          array, orc_offset, column_vector_batch);
    case arrow::Type::type::DOUBLE:
      return WriteGenericBatch<arrow::DoubleType, liborc::DoubleVectorBatch>(
          array, orc_offset, column_vector_batch);
    case arrow::Type::type::BINARY:
      return WriteGenericBatch<arrow::BinaryType, liborc::StringVectorBatch>(
          array, orc_offset, column_vector_batch);
    case arrow::Type::type::LARGE_BINARY:
      return WriteGenericBatch<arrow::LargeBinaryType, liborc::StringVectorBatch>(
          array, orc_offset, column_vector_batch);
    case arrow::Type::type::STRING:
      return WriteGenericBatch<arrow::StringType, liborc::StringVectorBatch>(
          array, orc_offset, column_vector_batch);
    case arrow::Type::type::LARGE_STRING:
      return WriteGenericBatch<arrow::LargeStringType, liborc::StringVectorBatch>(
          array, orc_offset, column_vector_batch);
    case arrow::Type::type::FIXED_SIZE_BINARY:
      return WriteFixedSizeBinaryBatch(array, orc_offset, column_vector_batch);
    case arrow::Type::type::DATE32:
      return WriteGenericBatch<arrow::Date32Type, liborc::LongVectorBatch>(
          array, orc_offset, column_vector_batch);
    case arrow::Type::type::DATE64:
      return WriteTimestampBatch<arrow::Date64Type>(
          array, orc_offset, column_vector_batch, kOneSecondMillis, kOneMilliNanos);
    case arrow::Type::type::TIMESTAMP: {
      switch (arrow::internal::checked_pointer_cast<arrow::TimestampType>(array.type())
                  ->unit()) {
        case arrow::TimeUnit::type::SECOND:
          return WriteTimestampBatch<arrow::TimestampType>(
              array, orc_offset, column_vector_batch, 1, kOneSecondNanos);
        case arrow::TimeUnit::type::MILLI:
          return WriteTimestampBatch<arrow::TimestampType>(
              array, orc_offset, column_vector_batch, kOneSecondMillis, kOneMilliNanos);
        case arrow::TimeUnit::type::MICRO:
          return WriteTimestampBatch<arrow::TimestampType>(
              array, orc_offset, column_vector_batch, kOneSecondMicros, kOneMicroNanos);
        case arrow::TimeUnit::type::NANO:
          return WriteTimestampBatch<arrow::TimestampType>(
              array, orc_offset, column_vector_batch, kOneSecondNanos, 1);
        default:
          return arrow::Status::Invalid("Unknown or unsupported Arrow type: ",
                                        array.type()->ToString());
      }
    }
    case arrow::Type::type::DECIMAL128: {
      int32_t precision =
          std::static_pointer_cast<arrow::Decimal128Type>(array.type())->precision();
      if (precision > 18) {
        return WriteGenericBatch<arrow::Decimal128Type, liborc::Decimal128VectorBatch>(
            array, orc_offset, column_vector_batch);
      } else {
        return WriteGenericBatch<arrow::Decimal128Type, liborc::Decimal64VectorBatch>(
            array, orc_offset, column_vector_batch);
      }
    }
    case arrow::Type::type::STRUCT:
      return WriteStructBatch(array, orc_offset, column_vector_batch, normalized);
    case arrow::Type::type::LIST:
      return WriteListBatch<arrow::ListArray>(array, orc_offset, column_vector_batch);
    case arrow::Type::type::LARGE_LIST:
      return WriteListBatch<arrow::LargeListArray>(array, orc_offset,
                                                   column_vector_batch);
    case arrow::Type::type::FIXED_SIZE_LIST:
      return WriteListBatch<arrow::FixedSizeListArray>(array, orc_offset,
                                                       column_vector_batch);
    case arrow::Type::type::MAP:
      return WriteMapBatch(array, orc_offset, column_vector_batch);
    default: {
      return arrow::Status::Invalid("Unknown or unsupported Arrow type: ",
                                    array.type()->ToString());
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

// arrow::Result<ORC_UNIQUE_PTR<liborc::Type>> GetORCType(const arrow::DataType& type) {
//   arrow::Type::type kind = type.id();
//   switch (kind) {
//     case arrow::Type::type::BOOL:
//       return liborc::createPrimitiveType(liborc::TypeKind::BOOLEAN);
//     case arrow::Type::type::INT8:
//       return liborc::createPrimitiveType(liborc::TypeKind::BYTE);
//     case arrow::Type::type::INT16:
//       return liborc::createPrimitiveType(liborc::TypeKind::SHORT);
//     case arrow::Type::type::INT32:
//       return liborc::createPrimitiveType(liborc::TypeKind::INT);
//     case arrow::Type::type::INT64:
//       return liborc::createPrimitiveType(liborc::TypeKind::LONG);
//     case arrow::Type::type::FLOAT:
//       return liborc::createPrimitiveType(liborc::TypeKind::FLOAT);
//     case arrow::Type::type::DOUBLE:
//       return liborc::createPrimitiveType(liborc::TypeKind::DOUBLE);
//     // Use STRING instead of VARCHAR for now, both use UTF-8
//     case arrow::Type::type::STRING:
//     case arrow::Type::type::LARGE_STRING:
//       return liborc::createPrimitiveType(liborc::TypeKind::STRING);
//     case arrow::Type::type::BINARY:
//     case arrow::Type::type::LARGE_BINARY:
//     case arrow::Type::type::FIXED_SIZE_BINARY:
//       return liborc::createPrimitiveType(liborc::TypeKind::BINARY);
//     case arrow::Type::type::DATE32:
//       return liborc::createPrimitiveType(liborc::TypeKind::DATE);
//     case arrow::Type::type::DATE64:
//     case arrow::Type::type::TIMESTAMP:
//       return liborc::createPrimitiveType(liborc::TypeKind::TIMESTAMP);
//     case arrow::Type::type::DECIMAL128: {
//       const uint64_t precision = static_cast<uint64_t>(
//           static_cast<const arrow::Decimal128Type&>(type).precision());
//       const uint64_t scale =
//           static_cast<uint64_t>(static_cast<const
//           arrow::Decimal128Type&>(type).scale());
//       return liborc::createDecimalType(precision, scale);
//     }
//     case arrow::Type::type::LIST:
//     case arrow::Type::type::FIXED_SIZE_LIST:
//     case arrow::Type::type::LARGE_LIST: {
//       std::shared_ptr<arrow::DataType> arrow_child_type =
//           static_cast<const arrow::BaseListType&>(type).value_type();
//       ORC_UNIQUE_PTR<liborc::Type> orc_subtype =
//           GetORCType(*arrow_child_type).ValueOrDie();
//       return liborc::createListType(std::move(orc_subtype));
//     }
//     case arrow::Type::type::STRUCT: {
//       ORC_UNIQUE_PTR<liborc::Type> out_type = liborc::createStructType();
//       std::vector<std::shared_ptr<arrow::Field>> arrow_fields =
//           checked_cast<const arrow::StructType&>(type).fields();
//       for (std::vector<std::shared_ptr<arrow::Field>>::iterator it =
//       arrow_fields.begin();
//            it != arrow_fields.end(); ++it) {
//         std::string field_name = (*it)->name();
//         std::shared_ptr<arrow::DataType> arrow_child_type = (*it)->type();
//         ORC_UNIQUE_PTR<liborc::Type> orc_subtype =
//             GetORCType(*arrow_child_type).ValueOrDie();
//         out_type->addStructField(field_name, std::move(orc_subtype));
//       }
//       return out_type;
//     }
//     case arrow::Type::type::MAP: {
//       std::shared_ptr<arrow::DataType> key_arrow_type =
//           checked_cast<const arrow::MapType&>(type).key_type();
//       std::shared_ptr<arrow::DataType> item_arrow_type =
//           checked_cast<const arrow::MapType&>(type).item_type();
//       ORC_UNIQUE_PTR<liborc::Type> key_orc_type =
//                                        GetORCType(*key_arrow_type).ValueOrDie(),
//                                    item_orc_type =
//                                        GetORCType(*item_arrow_type).ValueOrDie();
//       return liborc::createMapType(std::move(key_orc_type), std::move(item_orc_type));
//     }
//     case arrow::Type::type::DENSE_UNION:
//     case arrow::Type::type::SPARSE_UNION: {
//       ORC_UNIQUE_PTR<liborc::Type> out_type = liborc::createUnionType();
//       std::vector<std::shared_ptr<arrow::Field>> arrow_fields =
//           checked_cast<const arrow::UnionType&>(type).fields();
//       for (std::vector<std::shared_ptr<arrow::Field>>::iterator it =
//       arrow_fields.begin();
//            it != arrow_fields.end(); ++it) {
//         std::string field_name = (*it)->name();
//         std::shared_ptr<arrow::DataType> arrow_child_type = (*it)->type();
//         ORC_UNIQUE_PTR<liborc::Type> orc_subtype =
//             GetORCType(*arrow_child_type).ValueOrDie();
//         out_type->addUnionChild(std::move(orc_subtype));
//       }
//       return out_type;
//     }
//     default: {
//       return arrow::Status::Invalid("Unknown or unsupported Arrow type: ",
//                                     type.ToString());
//     }
//   }
// }
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
  while (*arrow_chunk_offset < num_batch && orc_offset < length) {
    std::shared_ptr<Array> array =
        NormalizeArray(chunked_array.chunk(*arrow_chunk_offset));
    int64_t num_written_elements =
        std::min(length - orc_offset, array->length() - *arrow_index_offset);
    if (num_written_elements > 0) {
      RETURN_NOT_OK(
          ::WriteBatch(*(array->Slice(*arrow_index_offset, num_written_elements)),
                       orc_offset, column_vector_batch));
      orc_offset += num_written_elements;
      *arrow_index_offset += num_written_elements;
    }
    if (orc_offset < length) {  // Another Arrow Array done
      *arrow_index_offset = 0;
      (*arrow_chunk_offset)++;
    }
  }
  column_vector_batch->numElements = orc_offset;
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

Result<ORC_UNIQUE_PTR<liborc::Type>> GetORCType(const Schema& schema) {
  int numFields = schema.num_fields();
  ORC_UNIQUE_PTR<liborc::Type> out_type = liborc::createStructType();
  for (int i = 0; i < numFields; i++) {
    std::shared_ptr<Field> field = schema.field(i);
    std::string field_name = field->name();
    std::shared_ptr<DataType> arrow_child_type = field->type();
    ORC_UNIQUE_PTR<liborc::Type> orc_subtype =
        ::GetORCType(*arrow_child_type).ValueOrDie();
    out_type->addStructField(field_name, std::move(orc_subtype));
  }
  return out_type;
}

}  // namespace orc
}  // namespace adapters
}  // namespace arrow
