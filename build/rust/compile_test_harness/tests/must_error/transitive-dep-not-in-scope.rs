// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A transitive dependency is found through -Ldependency, which keeps it out of the snippet's
// namespace.

pub fn transitive() -> u32 {
    fixture_dep::Inner(1).0 //~ ERROR E0433
}
