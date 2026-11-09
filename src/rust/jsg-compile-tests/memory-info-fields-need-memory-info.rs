// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// `#[jsg_resource(memory_info)]` reports every field that is not marked
// `#[jsg_memory_info(skip)]`, so a field whose type has no `jsg::MemoryInfo` does not compile:
// a field added to a resource is reported, or skipped explicitly.
use jsg_macros::jsg_resource;

struct NotReported;

jsg::jsg_traced!(NotReported);

#[jsg_resource(memory_info)]
struct Holder {
    name: String,
    opaque: NotReported, //~ ERROR E0277
}

#[jsg_resource]
impl Holder {}
