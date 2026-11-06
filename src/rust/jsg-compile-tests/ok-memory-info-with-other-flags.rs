// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// `memory_info` and `custom_memory_info` combine with the other flags of `#[jsg_resource]`, in
// any order, and the generated `VisitForMemoryInfo` reports fields that are not `pub`, skips
// a field marked `#[jsg_memory_info(skip)]` and takes a struct with no fields.
#![deny(warnings)]

use std::ffi::CStr;

use jsg_macros::jsg_resource;

#[jsg_resource(name = "Renamed", memory_info, custom_trace, ts_root)]
pub struct Holder {
    name: String,
    /// No `MemoryInfo`, so it is skipped.
    #[jsg_memory_info(skip)]
    #[allow(dead_code, reason = "only declared")]
    opaque: NotReported,
}

pub struct NotReported;

#[jsg_resource]
impl Holder {}

impl jsg::Traced for Holder {
    fn trace(&self, _: &mut jsg::v8::GcVisitor) {}
}

#[jsg_resource(custom_trace, custom_memory_info)]
pub struct Other {
    cache: Cache,
}

#[jsg_resource]
impl Other {}

impl jsg::Traced for Other {
    fn trace(&self, _: &mut jsg::v8::GcVisitor) {}
}

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

pub fn t(lock: &mut jsg::Lock<'_>, name: String) -> (jsg::Rc<Holder>, jsg::Rc<Other>) {
    (
        jsg::Rc::new(
            lock,
            Holder {
                name,
                opaque: NotReported,
            },
        ),
        jsg::Rc::new(
            lock,
            Other {
                cache: Cache {
                    entries: Vec::new(),
                },
            },
        ),
    )
}

#[jsg_resource(memory_info)]
pub struct Empty {}

#[jsg_resource]
impl Empty {}
