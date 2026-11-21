// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/jsg/jsg.h>

namespace workerd::api {

constexpr uint MAX_INDEXES_LENGTH = 1;
constexpr size_t MAX_INDEX_SIZE_IN_BYTES = 96;
constexpr uint MAX_ARRAY_MEMBERS = 20;
constexpr size_t MAX_CUMULATIVE_BYTES_IN_BLOBS = 800 * MAX_ARRAY_MEMBERS;

// A blob or index value: the bytes of a buffer source, or of a string as UTF-8.
using DataPointBytes = kj::OneOf<kj::Array<kj::byte>, kj::String>;

kj::ArrayPtr<const kj::byte> dataPointBytes(const DataPointBytes& value);

// The validate*() functions throw what the matching set*() function would, without building
// anything, so that a caller can reject a data point before it starts writing it.

void validateDoubles(kj::ArrayPtr<const double> arr, kj::StringPtr errorPrefix);
void validateBlobs(kj::ArrayPtr<const kj::Maybe<DataPointBytes>> arr, kj::StringPtr errorPrefix);
void validateIndexes(kj::ArrayPtr<const kj::Maybe<DataPointBytes>> arr, kj::StringPtr errorPrefix);

template <typename Message>
void setDoubles(Message msg, kj::ArrayPtr<double> arr, kj::StringPtr errorPrefix) {
  validateDoubles(arr, errorPrefix);

  uint index = 1;
  for (auto& item: arr) {
    switch (index) {
      case 1:
        msg.setDouble1(item);
        break;
      case 2:
        msg.setDouble2(item);
        break;
      case 3:
        msg.setDouble3(item);
        break;
      case 4:
        msg.setDouble4(item);
        break;
      case 5:
        msg.setDouble5(item);
        break;
      case 6:
        msg.setDouble6(item);
        break;
      case 7:
        msg.setDouble7(item);
        break;
      case 8:
        msg.setDouble8(item);
        break;
      case 9:
        msg.setDouble9(item);
        break;
      case 10:
        msg.setDouble10(item);
        break;
      case 11:
        msg.setDouble11(item);
        break;
      case 12:
        msg.setDouble12(item);
        break;
      case 13:
        msg.setDouble13(item);
        break;
      case 14:
        msg.setDouble14(item);
        break;
      case 15:
        msg.setDouble15(item);
        break;
      case 16:
        msg.setDouble16(item);
        break;
      case 17:
        msg.setDouble17(item);
        break;
      case 18:
        msg.setDouble18(item);
        break;
      case 19:
        msg.setDouble19(item);
        break;
      case 20:
        msg.setDouble20(item);
        break;
    }
    index++;
  }
}

template <typename Message>
void setBlobs(Message msg, kj::ArrayPtr<kj::Maybe<DataPointBytes>> arr, kj::StringPtr errorPrefix) {
  validateBlobs(arr, errorPrefix);
  uint index = 1;
  for (auto& item: arr) {
    KJ_IF_SOME(i, item) {
      auto value = dataPointBytes(i);
      switch (index) {
        case 1:
          msg.setBlob1(value);
          break;
        case 2:
          msg.setBlob2(value);
          break;
        case 3:
          msg.setBlob3(value);
          break;
        case 4:
          msg.setBlob4(value);
          break;
        case 5:
          msg.setBlob5(value);
          break;
        case 6:
          msg.setBlob6(value);
          break;
        case 7:
          msg.setBlob7(value);
          break;
        case 8:
          msg.setBlob8(value);
          break;
        case 9:
          msg.setBlob9(value);
          break;
        case 10:
          msg.setBlob10(value);
          break;
        case 11:
          msg.setBlob11(value);
          break;
        case 12:
          msg.setBlob12(value);
          break;
        case 13:
          msg.setBlob13(value);
          break;
        case 14:
          msg.setBlob14(value);
          break;
        case 15:
          msg.setBlob15(value);
          break;
        case 16:
          msg.setBlob16(value);
          break;
        case 17:
          msg.setBlob17(value);
          break;
        case 18:
          msg.setBlob18(value);
          break;
        case 19:
          msg.setBlob19(value);
          break;
        case 20:
          msg.setBlob20(value);
          break;
      }
    }
    index++;
  }
}

template <typename Message>
void setIndexes(
    Message msg, kj::ArrayPtr<kj::Maybe<DataPointBytes>> arr, kj::StringPtr errorPrefix) {
  validateIndexes(arr, errorPrefix);
  if (arr.size() == 0) {
    return;
  }
  KJ_IF_SOME(value, arr[0]) {
    msg.setIndex1(dataPointBytes(value));
  }
}
}  // namespace workerd::api
