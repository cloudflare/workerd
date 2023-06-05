// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/jsg/jsg.h>

namespace workerd::api {

constexpr uint MAX_INDEXES_LENGTH = 1;
constexpr size_t MAX_INDEX_SIZE_IN_BYTES = 96;
constexpr uint MAX_ARRAY_MEMBERS = 20;
constexpr size_t MAX_CUMULATIVE_BYTES_IN_BLOBS = 256 * MAX_ARRAY_MEMBERS;

template <typename Message>
void setDoubles(Message msg, kj::ArrayPtr<double> arr,
                kj::StringPtr errorPrefix) {
  JSG_REQUIRE(arr.size() <= MAX_ARRAY_MEMBERS, TypeError, errorPrefix,
               "Maximum of ", MAX_ARRAY_MEMBERS, " doubles supported.");

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
void setBlobs(
    Message msg,
    kj::ArrayPtr<kj::Maybe<kj::OneOf<kj::Array<kj::byte>, kj::String>>> arr,
    kj::StringPtr errorPrefix) {
  JSG_REQUIRE(arr.size() <= MAX_ARRAY_MEMBERS, TypeError, errorPrefix,
               "Maximum of ", MAX_ARRAY_MEMBERS, " blobs supported.");
  kj::ArrayPtr<kj::byte> value;
  uint index = 1;
  size_t sizeSum = 0;
  for (auto& item: arr) {
    KJ_IF_MAYBE(*i, item) {
      KJ_SWITCH_ONEOF(*i) {
        KJ_CASE_ONEOF(val, kj::Array<kj::byte>) {
          value = val.asBytes();
        }
        KJ_CASE_ONEOF(val, kj::String) {
          value = val.asBytes();
        }
      }
      sizeSum += value.size();
      JSG_REQUIRE(sizeSum <= MAX_CUMULATIVE_BYTES_IN_BLOBS, TypeError,
                   errorPrefix, "Cumulative size of blobs exceeds ",
                   MAX_CUMULATIVE_BYTES_IN_BLOBS, " bytes).");
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
    Message msg,
    kj::ArrayPtr<kj::Maybe<kj::OneOf<kj::Array<kj::byte>, kj::String>>> arr,
    kj::StringPtr errorPrefix) {
  JSG_REQUIRE(arr.size() <= MAX_INDEXES_LENGTH, TypeError, errorPrefix,
              "Maximum of ", MAX_INDEXES_LENGTH, " indexes supported.");
  if (arr.size() == 0) {
    return;
  }
  auto item = kj::mv(arr[0]);
  kj::ArrayPtr<kj::byte> value;
  KJ_IF_MAYBE(*i, item) {
    KJ_SWITCH_ONEOF(*i) {
      KJ_CASE_ONEOF(val, kj::Array<kj::byte>) {
        value = val.asBytes();
      }
      KJ_CASE_ONEOF(val, kj::String) {
        value = val.asBytes();
      }
    }
    JSG_REQUIRE(value.size() <= MAX_INDEX_SIZE_IN_BYTES, TypeError,
                errorPrefix, "Size of indexes[0] exceeds ",
                MAX_INDEX_SIZE_IN_BYTES, " bytes).");
    msg.setIndex1(value);
  }
}
}  // namespace workerd::api
