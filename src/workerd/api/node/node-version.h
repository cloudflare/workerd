// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

#include <kj/string.h>

namespace workerd::api::node {

// Node.js version supported by the platform.
// This represents the most current Node.js version supported by the platform
// and will change as Node.js release updates ship.
static constexpr kj::StringPtr nodeVersion = "22.15.1"_kj;

}  // namespace workerd::api::node