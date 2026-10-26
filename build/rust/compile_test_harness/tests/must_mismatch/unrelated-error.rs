// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The expected error is there, but so is one nobody annotated (a typo).

pub fn f() -> u32 {
    let _ = undefined_function();
    "not a number" //~ ERROR E0308
}
