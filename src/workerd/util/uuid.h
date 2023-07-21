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

}
