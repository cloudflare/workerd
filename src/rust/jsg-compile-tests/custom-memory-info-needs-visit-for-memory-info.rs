// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// `#[jsg_resource(custom_memory_info)]` reports the resource's fields through the author's
// `jsg::VisitForMemoryInfo` impl, so the resource must have one.
use jsg_macros::jsg_resource;

#[jsg_resource(custom_memory_info)] //~ ERROR E0277
struct Holder {
    name: String,
}

#[jsg_resource]
impl Holder {}
