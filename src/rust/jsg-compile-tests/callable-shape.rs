// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A resource type is callable at most one way, through a method that takes `&self` or
// `this: jsg::Rc<Self>`, and `#[jsg_callable]` takes no arguments.

use jsg_macros::jsg_callable;
use jsg_macros::jsg_resource;

#[jsg_resource]
pub struct Twice;

#[jsg_resource]
impl Twice {
    #[jsg_callable]
    pub fn first(&self) {}

    #[jsg_callable]
    pub fn second(&self) {}
    //~^ ERROR a resource type has at most one #[jsg_callable]
}

#[jsg_resource]
pub struct Static;

#[jsg_resource]
impl Static {
    #[jsg_callable]
    pub fn call() {}
    //~^ ERROR a #[jsg_callable] method takes `&self`, or `this: jsg::Rc<Self>` in its place
}

#[jsg_resource]
pub struct Mutable;

#[jsg_resource]
impl Mutable {
    #[jsg_callable]
    pub fn call(&mut self) {}
    //~^ ERROR a resource method takes `&self`
}

#[jsg_resource]
pub struct MisplacedThis;

#[jsg_resource]
impl MisplacedThis {
    #[jsg_callable]
    pub fn call(lock: &mut jsg::Lock, this: jsg::Rc<Self>) {
        //~^ ERROR `this` must be the first parameter
        let _ = (lock, this);
    }
}

#[jsg_resource]
pub struct Arguments;

#[jsg_resource]
impl Arguments {
    #[jsg_callable(name = "x")]
    //~^ ERROR #[jsg_callable] takes no arguments
    pub fn call(&self) {}
}
