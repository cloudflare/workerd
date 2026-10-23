// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The liveness of an isolate, shared with the persistent handles created in it.
//!
//! A [`v8::Global`](crate::v8::Global) outlives every scope, and nothing ties its lifetime
//! to its isolate's: one can be dropped after `v8::Isolate::Dispose`, for example when the
//! Rust object holding it is owned by something that outlives the worker. `Dispose` frees
//! every global and traced handle of the isolate in bulk, so resetting a handle after it
//! would touch freed memory.
//!
//! The [`Realm`](crate::Realm) therefore owns an [`IsolateState`], shared through an `Arc`
//! with every `Global` of its isolate, and closes it when it is dropped. A `Realm` is dropped
//! under the isolate lock, before the isolate is disposed, so a `Global` that finds the state
//! closed leaves its handles to `Dispose`.

use std::sync::Arc;
use std::sync::atomic::AtomicBool;
use std::sync::atomic::Ordering;

use crate::Isolate;
use crate::v8;
use crate::v8::ffi;

/// What the handles of an isolate need to know about it after their scopes are gone.
pub struct IsolateState {
    /// Only compared with other isolate pointers.
    isolate: *mut ffi::Isolate,
    /// Set by [`close`](Self::close), under the isolate lock, before the isolate is disposed.
    closed: AtomicBool,
}

// SAFETY: `isolate` is a raw pointer that is only compared. The other field synchronizes
// itself.
#[expect(unsafe_code, reason = "shares the raw isolate pointer between threads")]
unsafe impl Send for IsolateState {}
// SAFETY: as for `Send`.
#[expect(unsafe_code, reason = "shares the raw isolate pointer between threads")]
unsafe impl Sync for IsolateState {}

impl IsolateState {
    pub(crate) fn new(isolate: v8::IsolatePtr) -> Arc<Self> {
        Arc::new(Self {
            isolate: isolate.as_ffi(),
            closed: AtomicBool::new(false),
        })
    }

    /// Whether `isolate` is the isolate of this state. Only compares the pointers.
    pub(crate) fn belongs_to(&self, isolate: *mut ffi::Isolate) -> bool {
        self.isolate == isolate
    }

    /// Whether the isolate is about to be disposed, or already has been. Its handles are then
    /// freed with it, and must not be touched.
    pub(crate) fn is_closed(&self) -> bool {
        self.closed.load(Ordering::Acquire)
    }

    /// Records that the isolate is about to be disposed. Every handle of the isolate that is
    /// dropped from now on is left to `Dispose`.
    ///
    /// The `Isolate` reference proves that the caller holds the isolate lock; no handle of the
    /// isolate can be reset concurrently.
    pub(crate) fn close(&self, _isolate: &mut Isolate) {
        self.closed.store(true, Ordering::Release);
    }
}
