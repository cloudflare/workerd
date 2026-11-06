// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// V8 reads the names of heap snapshot edges after the tracker's callback returns, so an edge name
// is a `&'static CStr`, and one built at runtime is rejected.
use std::ffi::CString;

use jsg_macros::jsg_resource;

#[jsg_resource(custom_memory_info)]
struct Holder {
    name: String,
}

#[jsg_resource]
impl Holder {}

impl jsg::VisitForMemoryInfo for Holder {
    fn visit_for_memory_info(&self, tracker: &mut jsg::MemoryTracker<'_>) {
        let edge_name = CString::new("name").unwrap();
        tracker.track_field(&edge_name, &self.name); //~ ERROR E0597
    }
}
