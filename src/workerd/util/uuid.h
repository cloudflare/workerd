// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/compat/http.h>
#include <kj/string.h>
#include <kj/hash.h>

namespace workerd {

// Generates a random version 4 UUID using the given entropy source or a default
// secure random number generator. Unless you pass in a predictable entropy
// source, it is safe to assume that the output of this function is unique.
kj::String randomUUID(kj::Maybe<kj::EntropySource&> optionalEntropySource);

// A 128-bit universally unique identifier (UUID).
//
// A UUID can be created from and converted between between two formats:
// 1. Upper/lower format: an "upper" field representing the most signficant bits and "lower" field
//    representings the least significant bits.
// 2. Stringified 8-4-4-4-12 hex format.
//
// A "null UUID" (a UUID with a value of 0) is considered invalid and is not possible to create.
class UUID {
public:
  // Create a UUID from upper and lower parts. If the UUID would be null, return kj::none.
  //
  // For example, creating a UUID from upper and lower values of 81985529216486895 and
  // 81985529216486895 respectively yields a UUID which stringifies to
  // "01234567-89ab-cdef-0123-456789abcdef".
  static kj::Maybe<UUID> fromUpperLower(uint64_t upper, uint64_t lower);

  // Create a UUID from 8-4-4-4-12 hex format. If the provided string is not valid, or the UUID
  // would be null, return kj::none.
  static kj::Maybe<UUID> fromString(kj::StringPtr str);

  uint64_t getUpper() const {
    return upper;
  }

  uint64_t getLower() const {
    return lower;
  }

  // Stringify the UUID to 8-4-4-4-12 hex format.
  //
  // Note that this is NOT just a debugging API. Its behaviour is relied upon to implement
  // user-facing APIs.
  kj::String toString() const;

  bool operator==(const UUID& other) const {
    return upper == other.upper && lower == other.lower;
  }

  size_t hashCode() const {
    return kj::hashCode(upper, lower);
  }

private:
  uint64_t upper;
  uint64_t lower;

  UUID(uint64_t upper, uint64_t lower) : upper(upper), lower(lower) {}
};


} // namespace workerd
