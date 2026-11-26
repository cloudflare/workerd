// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A `#[jsg_oneof]` converts to JavaScript by moving its variant's value out, so it can't
// implement `Drop`, which could move a GC reference out of a variant.
#[jsg_macros::jsg_oneof] //~ ERROR E0509
pub enum NameOrCount {
    Name(String),
    Count(f64),
}

impl Drop for NameOrCount {
    fn drop(&mut self) {}
}
