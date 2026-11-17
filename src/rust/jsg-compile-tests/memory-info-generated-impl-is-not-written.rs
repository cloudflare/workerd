// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// `#[jsg_resource(memory_info)]` generates `jsg::VisitForMemoryInfo`, so a hand-written impl
// conflicts with it; `custom_memory_info` is the flag for one.
use jsg_macros::jsg_resource;

#[jsg_resource(memory_info)] //~ ERROR E0119
struct Holder {
    name: String,
}

#[jsg_resource]
impl Holder {}

impl jsg::VisitForMemoryInfo for Holder {
    fn visit_for_memory_info(&self, tracker: &mut jsg::MemoryTracker<'_>) {
        tracker.track_field(c"name", &self.name);
    }
}
