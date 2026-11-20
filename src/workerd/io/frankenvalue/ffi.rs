// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#![allow(
    unsafe_code,
    reason = "the cxx bridge expands to unsafe FFI glue, and the shims take C++'s raw isolate and \
              move handles across the bridge"
)]

//! The bridge to `ffi.h`, and the Rust API over it.

use jsg::v8;
use kj_rs::KjMaybe;
use kj_rs::KjOwn;

// Every function that takes an isolate takes the isolate of the caller's lock, locked by this
// thread with a context entered and a `HandleScope` open, and handles in that scope.
#[cxx::bridge(namespace = "workerd::frankenvalue")]
pub mod bridge {
    #[namespace = "workerd::rust::jsg"]
    unsafe extern "C++" {
        include!("workerd/rust/jsg/ffi.h");
        include!("workerd/rust/jsg/v8.rs.h");
        type Isolate = jsg::v8::ffi::Isolate;
        type Local = jsg::v8::ffi::Local;
        type MaybeLocal = jsg::v8::ffi::MaybeLocal;
    }

    #[namespace = "workerd"]
    unsafe extern "C++" {
        include!("workerd/io/frankenvalue/ffi.h");

        /// C++ `workerd::Frankenvalue` (`workerd/io/frankenvalue.h`): a value that can be
        /// evaluated to JavaScript in another isolate, such as `ctx.props` or a dynamic Worker's
        /// `env`. Its cap table holds the capabilities that serialized into it, such as the
        /// channels of `Fetcher`s and `DurableObjectClass`es.
        ///
        /// Rust holds one as a `KjOwn<Frankenvalue>`, which is neither `Send` nor `Sync`: the
        /// capabilities in its cap table are I/O objects of the thread whose request made them,
        /// shared through non-atomic reference counts, which `clone` and dropping change. A
        /// `KjOwn<Frankenvalue>` passed to C++ is moved out of with `kj::mv(*value)` where C++
        /// takes a `Frankenvalue` by value.
        type Frankenvalue;
    }

    unsafe extern "C++" {
        fn frankenvalue_new() -> KjOwn<Frankenvalue>;
        unsafe fn frankenvalue_from_js(
            isolate: *mut Isolate,
            value: Local,
            result: &mut KjMaybe<KjOwn<Frankenvalue>>,
        ) -> bool;
        unsafe fn frankenvalue_to_js(
            isolate: *mut Isolate,
            value: &Frankenvalue,
            result: &mut MaybeLocal,
        ) -> bool;
        fn frankenvalue_estimate_size(value: &Frankenvalue) -> usize;
        fn frankenvalue_is_empty(value: &Frankenvalue) -> bool;
        // A capability's `clone()` is virtual, and may throw.
        fn frankenvalue_clone(value: &Frankenvalue) -> Result<KjOwn<Frankenvalue>>;
    }

    impl KjOwn<Frankenvalue> {}
}

use bridge::Frankenvalue;

impl Frankenvalue {
    /// The empty object, as C++ `Frankenvalue()` is: what C++ passes for props that were not
    /// given.
    #[must_use]
    pub fn empty() -> KjOwn<Self> {
        bridge::frankenvalue_new()
    }

    /// The `Frankenvalue` of `value`, as C++ `Frankenvalue::fromJs(js, value)` makes it: `value`
    /// serialized by C++ JSG's `Serializer`, with each capability in it, such as a `Fetcher`,
    /// added to the cap table.
    ///
    /// # Errors
    ///
    /// A pending error, for which [`jsg::Error::is_pending`] is true, with the exception that C++
    /// throws pending: what JavaScript in `value` throws (a getter, a proxy trap), the
    /// `DataCloneError` for a value that can't be serialized, or the error a capability throws
    /// when it can't be passed on, such as the `DataCloneError` of a `Fetcher` whose channel
    /// can't be transferred. A JSG method that returns it throws that exception, unchanged, as a
    /// C++ JSG method that calls `fromJs` does.
    pub fn from_js<'s>(
        lock: &mut jsg::Lock<'s>,
        value: v8::Local<'s, v8::Value>,
    ) -> jsg::Result<KjOwn<Self>> {
        let mut slot = KjMaybe::None;
        jsg_cpp::call_cpp(lock, |isolate| {
            // SAFETY: `call_cpp` calls the shim with the lock's isolate, and `value` is a handle
            // in the lock's scope.
            unsafe { bridge::frankenvalue_from_js(isolate, value.into_ffi(), &mut slot) }
        })?;
        Ok(Option::from(slot)
            .unwrap_or_else(|| unreachable!("a shim that completed sets its result")))
    }

    /// The JavaScript value, as C++ `toJs(js)` makes it: a new value each time, with a new object
    /// for each capability, such as a `Fetcher` bound to the current `IoContext`.
    ///
    /// # Errors
    ///
    /// A pending error, with what C++ throws pending, such as for a capability that needs a
    /// current `IoContext` when there is none.
    pub fn to_js<'s>(&self, lock: &mut jsg::Lock<'s>) -> jsg::Result<v8::Local<'s, v8::Value>> {
        let mut slot = v8::ffi::MaybeLocal::default();
        jsg_cpp::call_cpp(lock, |isolate| {
            // SAFETY: `call_cpp` calls the shim with the lock's isolate.
            unsafe { bridge::frankenvalue_to_js(isolate, self, &mut slot) }
        })?;
        // SAFETY: the shim set `slot` to a handle in the lock's scope `'s`.
        Ok(unsafe { v8::Local::from_ffi_maybe(slot) }
            .unwrap_or_else(|| unreachable!("a shim that completed sets its result")))
    }

    /// An estimate of the value's size in bytes, as C++ `estimateSize()` gives it, for size
    /// limits: the serialized and JSON content, and the names of stitched-in properties, but not
    /// the cap table.
    #[must_use]
    pub fn estimate_size(&self) -> usize {
        bridge::frankenvalue_estimate_size(self)
    }

    /// Whether this is the empty object, as C++ `empty()` says.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        bridge::frankenvalue_is_empty(self)
    }

    /// A copy, as C++ `clone()` makes it: the content is copied, and each capability is cloned,
    /// typically by adding a reference to it. Needs no isolate lock.
    ///
    /// # Errors
    ///
    /// What a capability's `clone()` throws.
    pub fn try_clone(&self) -> Result<KjOwn<Self>, cxx::KjException> {
        bridge::frankenvalue_clone(self)
    }
}
