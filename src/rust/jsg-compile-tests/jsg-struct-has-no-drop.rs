// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A `#[jsg_struct]` converts to JavaScript by moving its fields out, so it can't implement
// `Drop`, which could move a GC reference out of a field.
#[jsg_macros::jsg_struct] //~ ERROR E0509
pub struct Options {
    pub name: String,
}

impl Drop for Options {
    fn drop(&mut self) {}
}
