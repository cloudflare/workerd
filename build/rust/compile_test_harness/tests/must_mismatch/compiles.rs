// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Annotated, but compiles.

pub fn f() -> u32 {
    1 //~ ERROR E0308
}
