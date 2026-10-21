// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// `fixture` resolves through --extern. The type of `fixture::make()` is in `fixture_dep`, which
// rustc finds through -Ldependency.

pub fn direct_and_transitive() -> u32 {
    fixture::make().0
}
