// Copyright (c) 2022-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/common.h>
#include <kj/string.h>

#include <workerd/io/actor-storage.capnp.h>

namespace workerd {
class ActorStorageLimits {
// This class wraps common values and functions for interacting durable object (actor) storage.
public:
  static constexpr size_t ADVERTISED_MAX_VALUE_SIZE = 128 * 1024;
  static constexpr size_t ENFORCED_MAX_VALUE_SIZE = ADVERTISED_MAX_VALUE_SIZE + 34;
  // We grant some extra cushion on top of the advertised max size in order
  // to avoid penalizing people for pushing right up against the advertised size.
  // The v8 serialization method we use can add a few extra bytes for its type tag
  // and other metadata, such as the length of a string or number of items in an
  // array. The most important cases (where users are most likely to try to
  // intentionally run right up against the limit) are Strings and ArrayBuffers,
  // which each get 4 bytes of metadata attached when encoded. We throw a little
  // extra on just for future proofing and an abundance of caution.
  //
  // If you're curious why we add 34 bytes of cushion -- we used to add 32, but
  // then started writing v8 serialization headers, which are 2 bytes, and didn't
  // want to stop accepting values that we accepted before writing headers.

  static constexpr size_t MAX_KEY_SIZE = 2048;

  static void checkMaxKeySize(kj::StringPtr key);
  static void checkMaxValueSize(kj::StringPtr key, kj::ArrayPtr<kj::byte> value);
  static void checkMaxPairsCount(size_t count);
};

} // namespace workerd
