// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// `this: jsg::Rc<Self>` takes the place of `&self` on methods and accessors, before the lock
// and the JavaScript arguments, and a JavaScript argument of type `jsg::Rc<Self>` stays one. The
// resource behind a `jsg::Rc` is borrowed with the lock.

use jsg::Rc;
use jsg::ToJS;
use jsg::v8;
use jsg_macros::jsg_inspect_property;
use jsg_macros::jsg_method;
use jsg_macros::jsg_property;
use jsg_macros::jsg_resource;

#[jsg_resource]
pub struct Counter {
    count: std::cell::Cell<u32>,
}

#[jsg_resource]
impl Counter {
    #[jsg_method]
    pub fn itself(this: jsg::Rc<Self>) -> jsg::Rc<Self> {
        this
    }

    #[jsg_method]
    pub fn imported(this: Rc<Self>, lock: &mut jsg::Lock<'_>) -> u32 {
        this.get(lock).count.get()
    }

    #[jsg_method]
    pub fn with_lock<'s>(
        this: jsg::Rc<Self>,
        lock: &mut jsg::Lock<'s>,
        by: u32,
    ) -> jsg::Result<v8::Local<'s, v8::Value>> {
        let count = &this.get(lock).count;
        count.set(count.get() + by);
        count.get().to_js(lock)
    }

    #[jsg_method]
    pub fn same_as(this: jsg::Rc<Self>, other: jsg::Rc<Self>) -> bool {
        this == other
    }

    #[jsg_method]
    pub fn argument_of_own_type(&self, lock: &mut jsg::Lock<'_>, other: jsg::Rc<Self>) -> bool {
        std::ptr::eq(self, other.get(lock))
    }

    #[jsg_method]
    pub fn compare(a: jsg::Rc<Self>, b: jsg::Rc<Self>) -> bool {
        a == b
    }

    #[jsg_property]
    pub fn get_total(this: jsg::Rc<Self>, lock: &mut jsg::Lock<'_>) -> u32 {
        this.get(lock).count.get()
    }

    #[jsg_property]
    pub fn set_total(this: jsg::Rc<Self>, lock: &mut jsg::Lock<'_>, v: u32) {
        this.get(lock).count.set(v);
    }

    #[jsg_property(instance)]
    pub fn get_current(this: jsg::Rc<Self>, lock: &mut jsg::Lock<'_>) -> u32 {
        this.get(lock).count.get()
    }

    #[jsg_inspect_property]
    pub fn debug_info(this: jsg::Rc<Self>, lock: &mut jsg::Lock<'_>) -> String {
        format!("Counter({})", this.get(lock).count.get())
    }
}

// The methods are ordinary associated functions to Rust.
pub fn calls(lock: &mut jsg::Lock<'_>, counter: jsg::Rc<Counter>) -> bool {
    let _ = Counter::itself(counter.clone(lock));
    let _ = Counter::imported(counter.clone(lock), lock);
    Counter::set_total(counter.clone(lock), lock, 1);
    Counter::compare(counter.clone(lock), counter.clone(lock))
        && Counter::same_as(counter.clone(lock), counter)
}
