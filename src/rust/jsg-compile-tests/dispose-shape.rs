// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A resource type has at most one `#[jsg_dispose]` and at most one `#[jsg_async_dispose]`, which
// take `&self` or `this: jsg::Rc<Self>`. Having one of each is in `ok-dispose.rs`.

use jsg_macros::jsg_async_dispose;
use jsg_macros::jsg_dispose;
use jsg_macros::jsg_inspect_property;
use jsg_macros::jsg_property;
use jsg_macros::jsg_resource;

#[jsg_resource]
pub struct Twice;

#[jsg_resource]
impl Twice {
    #[jsg_dispose]
    pub fn first(&self) {}

    #[jsg_dispose]
    pub fn second(&self) {}
    //~^ ERROR a resource type has at most one #[jsg_dispose]
}

#[jsg_resource]
pub struct TwiceAsync;

#[jsg_resource]
impl TwiceAsync {
    #[jsg_async_dispose]
    pub fn first<'s>(&self, lock: &mut jsg::Lock<'s>) -> jsg::Result<jsg::Promise<'s, ()>> {
        jsg::Promise::resolved(lock, ())
    }

    #[jsg_async_dispose]
    pub fn second<'s>(&self, lock: &mut jsg::Lock<'s>) -> jsg::Result<jsg::Promise<'s, ()>> {
        //~^ ERROR a resource type has at most one #[jsg_async_dispose]
        jsg::Promise::resolved(lock, ())
    }
}

#[jsg_resource]
pub struct OneMethodBoth;

#[jsg_resource]
impl OneMethodBoth {
    #[jsg_dispose]
    #[jsg_async_dispose]
    pub fn dispose<'s>(&self, lock: &mut jsg::Lock<'s>) -> jsg::Result<jsg::Promise<'s, ()>> {
        //~^ ERROR cannot be both #[jsg_dispose] and #[jsg_async_dispose]
        jsg::Promise::resolved(lock, ())
    }
}

#[jsg_resource]
pub struct Static;

#[jsg_resource]
impl Static {
    #[jsg_dispose]
    pub fn dispose() {}
    //~^ ERROR a #[jsg_dispose] method takes `&self`
}

#[jsg_resource]
pub struct Mutable;

#[jsg_resource]
impl Mutable {
    #[jsg_dispose]
    pub fn dispose(&mut self) {}
    //~^ ERROR a resource method takes `&self`
}

// A disposer's callback trusts the type's `v8::Signature`, so a disposer is not also a member
// registered without it, whichever attribute comes first.
#[jsg_resource]
pub struct InspectFirst;

#[jsg_resource]
impl InspectFirst {
    #[jsg_inspect_property]
    #[jsg_dispose]
    pub fn debug(&self) -> u32 {
        //~^ ERROR a #[jsg_dispose] method cannot be a #[jsg_inspect_property]
        1
    }
}

#[jsg_resource]
pub struct DisposeFirst;

#[jsg_resource]
impl DisposeFirst {
    #[jsg_async_dispose]
    #[jsg_inspect_property]
    pub fn debug(&self) -> u32 {
        //~^ ERROR a #[jsg_async_dispose] method cannot be a #[jsg_inspect_property]
        1
    }
}

#[jsg_resource]
pub struct Property;

#[jsg_resource]
impl Property {
    #[jsg_property(readonly)]
    #[jsg_dispose]
    pub fn get_value(&self) -> u32 {
        //~^ ERROR a #[jsg_dispose] method cannot be a #[jsg_property]
        1
    }
}

#[jsg_resource]
pub struct Arguments;

#[jsg_resource]
impl Arguments {
    #[jsg_dispose(name = "close")]
    //~^ ERROR #[jsg_dispose] takes no arguments
    pub fn dispose(&self) {}
}
