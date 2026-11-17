// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// `Traced`, which every resource field and every function data must implement, requires `Send`:
// any thread that takes the isolate lock can use them, and a method can move part of them into a
// future that runs without the lock. `std::rc::Rc` and an `Arc` of data that is not `Sync` share
// their contents without synchronization.
use jsg::v8;

/// The bound `#[jsg_resource]` puts on every field.
fn resource_field<T: jsg::Traced>() {}

pub fn fields() {
    resource_field::<std::rc::Rc<std::cell::Cell<u32>>>(); //~ ERROR E0277
    resource_field::<std::sync::Arc<std::cell::RefCell<u32>>>(); //~ ERROR E0277
}

pub fn data<'s>(lock: &mut jsg::Lock<'s>) -> jsg::Result<v8::Local<'s, v8::Function>> {
    let data = std::rc::Rc::new(0_u32);
    v8::Function::new(lock, data, |lock, _data, _args| Ok(lock.undefined())) //~ ERROR E0277
}

pub struct Shared(std::rc::Rc<std::cell::Cell<u32>>);

impl jsg::Traced for Shared {
    //~^ ERROR E0277
    fn trace(&self, _visitor: &mut v8::GcVisitor) {}
}
