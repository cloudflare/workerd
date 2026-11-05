// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The shapes a disposer can take.

use jsg::v8;
use jsg_macros::jsg_async_dispose;
use jsg_macros::jsg_dispose;
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;

#[jsg_resource]
pub struct WithLockAndResult;

#[jsg_resource]
impl WithLockAndResult {
    #[jsg_dispose]
    pub fn dispose(&self, _lock: &mut jsg::Lock) -> jsg::Result<()> {
        Ok(())
    }
}

#[jsg_resource]
pub struct NamedMethod;

#[jsg_resource]
impl NamedMethod {
    #[jsg_method]
    #[jsg_dispose]
    pub fn close(this: jsg::Rc<Self>) {
        let _ = this;
    }
}

#[jsg_resource]
pub struct TypedPromise;

#[jsg_resource]
impl TypedPromise {
    #[jsg_async_dispose]
    pub fn dispose<'s>(&self, lock: &mut jsg::Lock<'s>) -> jsg::Result<jsg::Promise<'s, ()>> {
        jsg::Promise::resolved(lock, ())
    }
}

// Qualified paths, in either order with #[jsg_method].
#[jsg_resource]
pub struct QualifiedMethodFirst;

#[jsg_resource]
impl QualifiedMethodFirst {
    #[jsg_macros::jsg_method]
    #[jsg_macros::jsg_dispose]
    pub fn close(&self) {}
}

#[jsg_resource]
pub struct QualifiedDisposeFirst;

#[jsg_resource]
impl QualifiedDisposeFirst {
    #[jsg_macros::jsg_async_dispose]
    #[jsg_macros::jsg_method]
    pub fn close<'s>(&self, lock: &mut jsg::Lock<'s>) -> jsg::Result<jsg::Promise<'s, ()>> {
        jsg::Promise::resolved(lock, ())
    }
}

#[jsg_resource]
pub struct UntypedPromise;

#[jsg_resource]
impl UntypedPromise {
    #[jsg_async_dispose]
    #[jsg_method]
    pub fn close<'s>(&self, lock: &mut jsg::Lock<'s>) -> jsg::Result<v8::Local<'s, v8::Promise>> {
        let undefined = lock.undefined();
        v8::Promise::resolved(lock, undefined)
    }
}

// One disposer of each kind, as C++ JSG allows.
#[jsg_resource]
pub struct SyncAndAsync;

#[jsg_resource]
impl SyncAndAsync {
    #[jsg_dispose]
    pub fn dispose(&self) {}

    #[jsg_async_dispose]
    pub fn dispose_async<'s>(&self, lock: &mut jsg::Lock<'s>) -> jsg::Result<jsg::Promise<'s, ()>> {
        jsg::Promise::resolved(lock, ())
    }
}

// A disposer can also be the callable, in either order: their callbacks differ.
#[jsg_resource]
pub struct CallableFirst;

#[jsg_resource]
impl CallableFirst {
    #[jsg_macros::jsg_callable]
    #[jsg_dispose]
    pub fn close(&self) {}
}

#[jsg_resource]
pub struct DisposeBeforeCallable;

#[jsg_resource]
impl DisposeBeforeCallable {
    #[jsg_dispose]
    #[jsg_macros::jsg_callable]
    #[jsg_method]
    pub fn close(&self) {}
}
