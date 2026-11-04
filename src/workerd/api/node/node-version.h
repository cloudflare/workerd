// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

#include <kj/string.h>

namespace workerd::api::node {

// Node.js versions reported by the platform, for Node.js compatibility.
// Each `nodeVersion<major>` constant tracks the latest release in its major series and is updated
// by the script in tools/update_node_version.py or via `just update-reported-node-version`.
// Libraries feature-sniff on the reported major version, so reporting a newer major requires a
// compatibility flag.
// This is not a guarantee for compatibility, and APIs will still be incomplete,
// but it at least can be used to indicate what Node.js version target is being
// supported for Node.js platform code.

// Reported before compatibility date 2026-11-24.
static constexpr kj::StringPtr nodeVersion22 = "22.23.4"_kj;
// Reported when the `nodejs_process_version_24` compatibility flag is enabled, which it is by
// default from compatibility date 2026-11-24.
static constexpr kj::StringPtr nodeVersion24 = "24.22.2"_kj;

}  // namespace workerd::api::node
