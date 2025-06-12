// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

#include <kj/string.h>

namespace workerd::api::node {

// Node.js version reported by the platform, for Node.js compatibility.
// Generally should track the current LTS Node.js version.
// This is not a guarantee for compatibility, and APIs will still be incomplete,
// but it at least can be used to indicate what Node.js version target is being
// supported for Node.js platform code.
static constexpr kj::StringPtr nodeVersion = "22.16.0"_kj;

}  // namespace workerd::api::node
