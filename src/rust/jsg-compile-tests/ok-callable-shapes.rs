// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A callable method has the shapes of a `#[jsg_method]`, and may also be one, with the
// attributes in either order. Custom registration may choose between several callables.

use jsg::MemberRegistry;
use jsg::Resource;
use jsg::feature_flags::compatibility_flags;
use jsg_macros::jsg_callable;
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;

#[jsg_resource]
pub struct WithLock;

#[jsg_resource]
impl WithLock {
    #[jsg_callable]
    pub fn call(
        &self,
        lock: &mut jsg::Lock,
        a: String,
        b: Option<bool>,
    ) -> Result<String, jsg::Error> {
        let _ = (lock, b);
        Ok(a)
    }
}

#[jsg_resource]
pub struct WithThis;

#[jsg_resource]
impl WithThis {
    #[jsg_callable]
    pub fn call(this: jsg::Rc<Self>, lock: &mut jsg::Lock) -> jsg::Rc<Self> {
        let _ = lock;
        this
    }
}

#[jsg_resource]
pub struct CallableFirst;

#[jsg_resource]
impl CallableFirst {
    #[jsg_callable]
    #[jsg_method]
    pub fn call(&self) {}
}

#[jsg_resource]
pub struct MethodFirst;

#[jsg_resource]
impl MethodFirst {
    #[jsg_method]
    #[jsg_callable]
    pub fn call(&self) {}
}

#[jsg_resource]
pub struct Versioned;

#[jsg_resource(custom_members)]
impl Versioned {
    #[jsg_callable]
    pub fn call_old(&self) {}

    #[jsg_callable]
    pub fn call_new(&self) {}
}

impl Resource for Versioned {
    fn members(registry: &mut impl MemberRegistry, flags: compatibility_flags::Reader<'_>) {
        if flags.get_node_js_compat() {
            registry.add(Self::call_new_callable_member());
        } else {
            registry.add(Self::call_old_callable_member());
        }
    }
}
