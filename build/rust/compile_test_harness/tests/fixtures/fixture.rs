// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! A library that compile test snippets depend on directly.

pub use fixture_reexported_macro::data_str;

pub fn make() -> fixture_dep::Inner {
    fixture_dep::Inner(1)
}
