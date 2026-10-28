// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A `this` parameter is the resource a method or accessor was called on. It is the first
// parameter, in place of `&self`, and its type is `jsg::Rc<Self>`.

use jsg_macros::jsg_inspect_property;
use jsg_macros::jsg_method;
use jsg_macros::jsg_property;
use jsg_macros::jsg_resource;

#[jsg_resource]
pub struct Counter {
    count: std::cell::Cell<u32>,
}

#[jsg_resource]
pub struct Other;

#[jsg_resource]
impl Other {}

#[jsg_resource]
impl Counter {
    #[jsg_method]
    pub fn with_self(&self, this: jsg::Rc<Self>) {
        //~^ ERROR `this` must be the first parameter
        let _ = (self, this);
    }

    #[jsg_method]
    pub fn after_lock(lock: &mut jsg::Lock, this: jsg::Rc<Self>) {
        //~^ ERROR `this` must be the first parameter
        let _ = (lock, this);
    }

    #[jsg_method]
    pub fn after_argument(count: u32, this: jsg::Rc<Self>) {
        //~^ ERROR `this` must be the first parameter
        let _ = (count, this);
    }

    #[jsg_method]
    pub fn std_rc(this: std::rc::Rc<Self>) {
        //~^ ERROR `this` must be the first parameter
        let _ = this;
    }

    #[jsg_method]
    pub fn by_reference(this: &jsg::Rc<Self>) {
        //~^ ERROR `this` must be the first parameter
        let _ = this;
    }

    #[jsg_method]
    pub fn of_another_type(this: jsg::Rc<Other>) {
        //~^ ERROR `this` must be the first parameter
        let _ = this;
    }

    #[jsg_method]
    pub fn weak(this: jsg::Weak<Self>) {
        //~^ ERROR `this` must be the first parameter
        let _ = this;
    }

    #[jsg_property]
    pub fn get_total(&self, this: jsg::Rc<Self>) -> f64 {
        //~^ ERROR `this` must be the first parameter
        let _ = this;
        f64::from(self.count.get())
    }

    #[jsg_property]
    pub fn set_total(lock: &mut jsg::Lock, this: jsg::Rc<Self>, v: f64) {
        //~^ ERROR `this` must be the first parameter
        let _ = (lock, this, v);
    }

    #[jsg_inspect_property]
    pub fn debug_info(&self, this: jsg::Rc<Self>) -> String {
        //~^ ERROR `this` must be the first parameter
        let _ = (self, this);
        String::new()
    }
}

// A rejected method is still emitted, so its callers add no errors of their own.
fn _calls(counter: jsg::Rc<Counter>, other: jsg::Rc<Other>) {
    counter.with_self(counter.clone());
    Counter::after_argument(1, counter.clone());
    Counter::of_another_type(other);
    Counter::weak(counter.downgrade());
    let _ = counter.get_total(counter.clone());
}
