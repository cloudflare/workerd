// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "analytics-engine-impl.h"

namespace workerd::api {

kj::ArrayPtr<const kj::byte> dataPointBytes(const DataPointBytes& value) {
  KJ_SWITCH_ONEOF(value) {
    KJ_CASE_ONEOF(bytes, kj::Array<kj::byte>) {
      return bytes.asBytes();
    }
    KJ_CASE_ONEOF(string, kj::String) {
      return string.asBytes();
    }
  }
  KJ_UNREACHABLE;
}

void validateDoubles(kj::ArrayPtr<const double> arr, kj::StringPtr errorPrefix) {
  JSG_REQUIRE(arr.size() <= MAX_ARRAY_MEMBERS, TypeError, errorPrefix, "Maximum of ",
      MAX_ARRAY_MEMBERS, " doubles supported.");
}

void validateBlobs(kj::ArrayPtr<const kj::Maybe<DataPointBytes>> arr, kj::StringPtr errorPrefix) {
  JSG_REQUIRE(arr.size() <= MAX_ARRAY_MEMBERS, TypeError, errorPrefix, "Maximum of ",
      MAX_ARRAY_MEMBERS, " blobs supported.");
  size_t sizeSum = 0;
  for (auto& item: arr) {
    KJ_IF_SOME(value, item) {
      sizeSum += dataPointBytes(value).size();
      JSG_REQUIRE(sizeSum <= MAX_CUMULATIVE_BYTES_IN_BLOBS, TypeError, errorPrefix,
          "Cumulative size of blobs exceeds ", MAX_CUMULATIVE_BYTES_IN_BLOBS, " bytes).");
    }
  }
}

void validateIndexes(kj::ArrayPtr<const kj::Maybe<DataPointBytes>> arr, kj::StringPtr errorPrefix) {
  JSG_REQUIRE(arr.size() <= MAX_INDEXES_LENGTH, TypeError, errorPrefix, "Maximum of ",
      MAX_INDEXES_LENGTH, " indexes supported.");
  if (arr.size() == 0) {
    return;
  }
  KJ_IF_SOME(value, arr[0]) {
    JSG_REQUIRE(dataPointBytes(value).size() <= MAX_INDEX_SIZE_IN_BYTES, TypeError, errorPrefix,
        "Size of indexes[0] exceeds ", MAX_INDEX_SIZE_IN_BYTES, " bytes).");
  }
}

}  // namespace workerd::api
