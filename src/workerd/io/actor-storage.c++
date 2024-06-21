// Copyright (c) 2022-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "actor-storage.h"

#include <workerd/jsg/exception.h>

namespace workerd {
void ActorStorageLimits::checkMaxKeySize(kj::StringPtr key) {
  // It's tempting to put the key in this message, but that key could be surprisingly large so let's
  // return a simple message.
  JSG_REQUIRE(key.size() <= MAX_KEY_SIZE, RangeError, kj::str(
      "Keys cannot be larger than ", MAX_KEY_SIZE, " bytes. ",
      "A key of size ", key.size(), " was provided."));
}

void ActorStorageLimits::checkMaxValueSize(kj::StringPtr, kj::ArrayPtr<kj::byte> value) {
  // It's tempting to put the key in this message, but that key could be surprisingly large so let's
  // return a simple message.
  JSG_REQUIRE(value.size() <= ENFORCED_MAX_VALUE_SIZE, RangeError, kj::str(
      "Values cannot be larger than ", ADVERTISED_MAX_VALUE_SIZE, " bytes. ",
      "A value of size ", value.size(), " was provided."));
}

void ActorStorageLimits::checkMaxPairsCount(size_t count) {
  JSG_REQUIRE(count <= rpc::ActorStorage::MAX_KEYS, RangeError, kj::str(
      "Maximum number of key value pairs is ", rpc::ActorStorage::MAX_KEYS, ". ",
      count, " pairs were provided."));
}

} // namespace workerd
