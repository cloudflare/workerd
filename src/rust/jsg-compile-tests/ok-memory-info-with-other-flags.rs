// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// `memory_info` combines with the other flags of `#[jsg_resource]`, in any order.
#![deny(warnings)]

use std::ffi::CStr;

use jsg_macros::jsg_resource;

#[jsg_resource(name = "Renamed", memory_info, custom_trace)]
pub struct Holder {
    name: String,
}

#[jsg_resource]
impl Holder {}

impl jsg::Traced for Holder {}

impl jsg::VisitForMemoryInfo for Holder {
    fn visit_for_memory_info(&self, tracker: &mut jsg::MemoryTracker<'_>) {
        tracker.track_field(c"name", &self.name);
    }
}

#[jsg_resource(custom_trace, memory_info)]
pub struct Other {
    cache: Cache,
}

#[jsg_resource]
impl Other {}

impl jsg::Traced for Other {}

impl jsg::VisitForMemoryInfo for Other {
    fn visit_for_memory_info(&self, tracker: &mut jsg::MemoryTracker<'_>) {
        tracker.track_field(c"cache", &self.cache);
    }
}

pub struct Cache {
    entries: Vec<String>,
}

impl jsg::MemoryRetainer for Cache {
    fn memory_name(&self) -> &'static CStr {
        c"Cache"
    }

    fn memory_info(&self, tracker: &mut jsg::MemoryTracker<'_>) {
        tracker.track_field(c"entries", &self.entries);
    }
}

jsg::jsg_memory_retainer!(Cache);

pub fn t(name: String) -> (jsg::Rc<Holder>, jsg::Rc<Other>) {
    (
        jsg::Rc::new(Holder { name }),
        jsg::Rc::new(Other {
            cache: Cache {
                entries: Vec::new(),
            },
        }),
    )
}
