// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// `#[jsg_traced]` on an enum requires the fields of every variant to hold no GC references, as
// on a struct. A `jsg::Rc` or `TracedGlobal` in a variant would never be visited.
use jsg::v8;
use jsg_macros::jsg_resource;
use jsg_macros::jsg_traced;

#[jsg_resource]
struct Leaf;

#[jsg_resource]
impl Leaf {}

#[jsg_traced]
enum HoldsRc {
    Empty,
    Leaf(jsg::Rc<Leaf>), //~ ERROR E0277
}

#[jsg_traced]
enum HoldsGlobal {
    Value {
        value: v8::TracedGlobal<v8::Value>, //~ ERROR E0277
    },
}

// Plain variants and `&'static` references to plain data are covered, so the enum can sit in a
// std cell.
#[jsg_traced]
enum Plain {
    Unit,
    Name(String),
    Named {
        count: &'static std::sync::atomic::AtomicUsize,
        label: &'static str,
    },
}

#[jsg_resource]
struct PlainInCell {
    plain: std::cell::RefCell<Plain>,
}

#[jsg_resource]
impl PlainInCell {}
