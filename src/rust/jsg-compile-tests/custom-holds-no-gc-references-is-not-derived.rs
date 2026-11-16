// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// With `custom_holds_no_gc_references`, a struct gets `HoldsNoGcReferences` only from its author,
// even if its fields all have it.
use std::cell::Cell;

use jsg_macros::jsg_resource;
use jsg_macros::jsg_struct;

#[jsg_struct(custom_holds_no_gc_references)]
pub struct Plain {
    pub label: String,
}

#[jsg_resource] //~ ERROR E0277
struct PlainInCell {
    plain: Cell<Option<Plain>>,
}

#[jsg_resource]
impl PlainInCell {}
