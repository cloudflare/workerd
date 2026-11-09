// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A misspelled flag of `#[jsg_resource]` on a struct is an error, rather than a flag that is
// silently off: with `memroy_info`, the resource would report no fields.
use jsg_macros::jsg_resource;

#[jsg_resource(memroy_info)] //~ ERROR expected `name = "..."`, `custom_trace`, `memory_info`, `custom_memory_info` or `ts_root`
struct Holder {
    name: String,
}

#[jsg_resource]
impl Holder {}
