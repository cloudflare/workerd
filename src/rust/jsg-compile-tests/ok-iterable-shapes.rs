// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// An iterable method has the shapes of a `#[jsg_method]`, and may also be one, with the
// attributes in either order. An iterator inherits `Iterator.prototype` with automatic or custom
// registration, and its `next()` result can carry an array.

use jsg::Intrinsic;
use jsg::MemberRegistry;
use jsg::Resource;
use jsg::feature_flags::compatibility_flags;
use jsg_macros::jsg_iterable;
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;
use jsg_macros::jsg_struct;

#[jsg_struct]
pub struct Next {
    pub done: bool,
    pub value: Option<Vec<String>>,
}

#[jsg_resource]
pub struct Entries;

#[jsg_resource(inherit_intrinsic = IteratorPrototype)]
impl Entries {
    #[jsg_method]
    pub fn next(&self) -> Result<Next, jsg::Error> {
        Ok(Next {
            done: true,
            value: None,
        })
    }

    #[jsg_iterable]
    pub fn iterator(this: jsg::Rc<Self>) -> jsg::Rc<Self> {
        this
    }
}

#[jsg_resource]
pub struct WithLock;

#[jsg_resource(nested_types(Entries), inherit_intrinsic = IteratorPrototype)]
impl WithLock {
    #[jsg_iterable]
    #[jsg_method]
    pub fn entries(&self, lock: &mut jsg::Lock) -> jsg::Rc<Entries> {
        jsg::Rc::new(lock, Entries)
    }
}

#[jsg_resource]
pub struct MethodFirst;

#[jsg_resource]
impl MethodFirst {
    #[jsg_method]
    #[jsg_iterable]
    pub fn values(this: jsg::Rc<Self>, lock: &mut jsg::Lock) -> jsg::Rc<Entries> {
        let _ = this;
        jsg::Rc::new(lock, Entries)
    }
}

#[jsg_resource]
pub struct Versioned;

#[jsg_resource(custom_members)]
impl Versioned {
    #[jsg_method]
    pub fn next(&self) -> Next {
        Next {
            done: true,
            value: None,
        }
    }
}

impl Resource for Versioned {
    fn members(registry: &mut impl MemberRegistry, flags: compatibility_flags::Reader<'_>) {
        registry.add(Self::next_member());
        if flags.get_node_js_compat() {
            registry.inherit_intrinsic(Intrinsic::IteratorPrototype);
        }
    }
}
