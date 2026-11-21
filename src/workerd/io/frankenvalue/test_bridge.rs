// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The Rust half of `frankenvalue-ffi-test.c++`: `Probe`, a Rust resource that builds `Frankenvalue`s
//! from its arguments, as a ported Worker Loader or loopback stub would, and the function through
//! which C++ creates it.

use frankenvalue::Frankenvalue;
use jsg::ToJS;
use jsg::v8;
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;

#[cxx::bridge(namespace = "workerd::frankenvalue::test")]
mod ffi {
    #[namespace = "workerd::rust::jsg"]
    unsafe extern "C++" {
        include!("workerd/rust/jsg/ffi.h");
        include!("workerd/rust/jsg/v8.rs.h");
        type Isolate = jsg::v8::ffi::Isolate;
        type MaybeLocal = jsg::v8::ffi::MaybeLocal;
    }

    #[namespace = "workerd"]
    unsafe extern "C++" {
        include!("workerd/io/frankenvalue/ffi.h");
        type Frankenvalue = frankenvalue::Frankenvalue;
    }

    unsafe extern "C++" {
        include!("workerd/io/frankenvalue/test-bridge.h");

        /// Takes the value by value, as `WorkerStubChannel::getEntrypoint()` takes props, and
        /// keeps it for the test to check.
        fn consume_frankenvalue(value: KjOwn<Frankenvalue>);
    }

    extern "Rust" {
        /// A new `Probe`. C++ calls it with the isolate locked, a context entered and a
        /// `HandleScope` open.
        unsafe fn probe_new(isolate: *mut Isolate) -> MaybeLocal;

        /// `value.to_js()`, or empty with its exception pending. C++ calls it with the isolate
        /// locked, a context entered and a `HandleScope` open, with or without an `IoContext`.
        unsafe fn to_js_from_rust(isolate: *mut Isolate, value: &Frankenvalue) -> MaybeLocal;
    }
}

#[jsg_resource]
pub struct Probe {}

#[jsg_resource]
impl Probe {
    /// `value`, through a `Frankenvalue`.
    #[jsg_method]
    pub fn round_trip<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        value: v8::Local<'s, v8::Value>,
    ) -> jsg::Result<v8::Local<'s, v8::Value>> {
        Frankenvalue::from_js(lock, value)?.to_js(lock)
    }

    /// `value`, through a clone of its `Frankenvalue`, which outlives the original.
    #[jsg_method]
    pub fn clone_round_trip<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        value: v8::Local<'s, v8::Value>,
    ) -> jsg::Result<v8::Local<'s, v8::Value>> {
        let original = Frankenvalue::from_js(lock, value)?;
        let copy = original.try_clone()?;
        drop(original);
        copy.to_js(lock)
    }

    /// The estimated size of `value`'s `Frankenvalue`.
    #[jsg_method]
    pub fn estimate_size<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        value: v8::Local<'s, v8::Value>,
    ) -> jsg::Result<f64> {
        #[expect(clippy::cast_precision_loss, reason = "test values are small")]
        Ok(Frankenvalue::from_js(lock, value)?.estimate_size() as f64)
    }

    /// The empty `Frankenvalue`, and whether it says it is empty.
    #[jsg_method]
    pub fn empty<'s>(&self, lock: &mut jsg::Lock<'s>) -> jsg::Result<v8::Local<'s, v8::Value>> {
        let empty = Frankenvalue::empty();
        let value = empty.to_js(lock)?;
        if !empty.is_empty() {
            return Err(jsg::Error::new(
                "Error",
                "the empty Frankenvalue is not empty",
            ));
        }
        Ok(value)
    }

    /// Whether `value`'s `Frankenvalue` is empty.
    #[jsg_method]
    pub fn is_empty<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        value: v8::Local<'s, v8::Value>,
    ) -> jsg::Result<bool> {
        Ok(Frankenvalue::from_js(lock, value)?.is_empty())
    }

    /// Hands `value`'s `Frankenvalue` to C++ by value, keeping a clone, which it then drops, to
    /// check that the two are independent.
    #[jsg_method]
    pub fn consume<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        value: v8::Local<'s, v8::Value>,
    ) -> jsg::Result<()> {
        let original = Frankenvalue::from_js(lock, value)?;
        let copy = original.try_clone()?;
        ffi::consume_frankenvalue(original);
        drop(copy);
        Ok(())
    }
}

/// # Safety
/// C++ calls this with the isolate locked, a context entered and a `HandleScope` open.
unsafe fn probe_new(isolate: *mut ffi::Isolate) -> ffi::MaybeLocal {
    // SAFETY: forwarded from the caller.
    unsafe { v8::run_ffi_callback(isolate, |lock| jsg::Rc::new(lock, Probe {}).to_js(lock)) }
}

/// # Safety
/// C++ calls this with the isolate locked, a context entered and a `HandleScope` open.
unsafe fn to_js_from_rust(isolate: *mut ffi::Isolate, value: &Frankenvalue) -> ffi::MaybeLocal {
    // SAFETY: forwarded from the caller.
    unsafe { v8::run_ffi_callback(isolate, |lock| value.to_js(lock)) }
}
