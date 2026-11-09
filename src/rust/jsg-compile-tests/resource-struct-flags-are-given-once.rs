// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A flag of `#[jsg_resource]` on a struct given twice is an error, as a misspelled one is.
use jsg_macros::jsg_resource;

#[jsg_resource(memory_info, memory_info)] //~ ERROR a #[jsg_resource] struct flag can be given once
struct Holder {
    name: String,
}

#[jsg_resource]
impl Holder {}
