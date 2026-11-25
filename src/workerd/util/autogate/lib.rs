// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The process's autogates, as C++ `util::Autogate` (`workerd/util/autogate.h`) has them. The
//! gates are declared and configured in C++; Rust only reads them.

mod ffi;

use ffi::bridge;

/// The names of the enabled autogates, each in kebab-case without the `workerd-autogate-` prefix
/// (`"test-workerd"` for `AutogateKey::TEST_WORKERD`), in the order of `WORKERD_AUTOGATES`.
pub fn enabled() -> impl Iterator<Item = &'static str> {
    (0..bridge::autogate_count())
        .filter(|&index| bridge::autogate_is_enabled(index))
        .map(bridge::autogate_name)
}

#[cfg(test)]
#[path = "lib-test.rs"]
mod tests;
