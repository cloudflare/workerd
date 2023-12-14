// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/compat/http.h>
#include <kj/string.h>

namespace workerd {

// Generates a random version 4 UUID using the given entropy source or a default
// secure random number generator. Unless you pass in a predictable entropy
// source, it is safe to assume that the output of this function is unique.
kj::String randomUUID(kj::Maybe<kj::EntropySource&> optionalEntropySource);

// Convert a UUID represented by two 64-bit integers to a string in the 8-4-4-4-12 format i.e.
// a dash-separated hex string in the form xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx.
// The `upper` parameter represents the most signficant bits and `lower` the least significant bits
// of the UUID value.
kj::String UUIDToString(uint64_t upper, uint64_t lower);

}
