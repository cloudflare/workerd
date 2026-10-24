// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! JavaScript promises: [`Promise`] and [`PromiseResolver`] handles.
//!
//! A Rust method returns a promise like any other handle, as a `Local<'s, Promise>`, and
//! settles it through a [`PromiseResolver`], now or from a later call that holds the resolver in a
//! [`TracedGlobal`](super::TracedGlobal). [`Local::then`] reacts to a promise with Rust
//! callbacks, which run as microtasks like the reactions JavaScript registers.
//!
//! Every operation that can fail fails only because execution is terminating, and then returns
//! an internal [`Error`], as [`Function::new`] does when V8 cannot create a function.
//!
//! The handles are untyped, like C++ JSG's `JsPromise`: values are JavaScript values, converted
//! with [`ToJS`](crate::ToJS) and [`FromJS`] at the edges. C++ JSG's typed `jsg::Promise<T>`,
//! which can carry C++ values through JavaScript, has no counterpart.

use super::As;
use super::Function;
use super::Local;
use super::Object;
use super::Value;
use super::ffi;
pub use super::ffi::PromiseState;
use crate::Error;
use crate::FromJS;
use crate::Lock;
use crate::Traced;
use crate::UnitType;

/// The error for an operation that V8 refused because execution is terminating.
fn terminated() -> Error {
    Error::new_internal_error("JavaScript execution terminated")
}

/// Marker for a `v8::Promise` handle.
pub struct Promise;

/// Marker for a `v8::Promise::Resolver` handle: the capability to settle one promise.
///
/// Unlike a [`Promise`], a resolver does not convert to a JavaScript value, so it cannot reach
/// JavaScript, which only ever sees the promise.
pub struct PromiseResolver;

impl PromiseResolver {
    /// Creates a pending promise and the resolver that settles it.
    ///
    /// # Errors
    ///
    /// Fails if execution is terminating.
    pub fn new<'s>(lock: &mut Lock<'s>) -> crate::Result<Local<'s, Self>> {
        // SAFETY: lock guarantees the isolate is locked and a HandleScope is active; a
        // non-empty handle is a resolver V8 created in the lock's scope.
        unsafe { Local::from_ffi_maybe(ffi::promise_resolver_new(lock.isolate().as_ffi())) }
            .ok_or_else(terminated)
    }
}

impl<'s> Local<'s, PromiseResolver> {
    /// The promise this resolver settles.
    pub fn promise(self) -> Local<'s, Promise> {
        // SAFETY: self is a valid resolver handle; the promise lives in the same scope.
        unsafe { Local::from_ffi(ffi::local_promise_resolver_promise(self.raw())) }
    }

    /// Resolves the promise with `value`, like the `resolve` function of `new Promise()`: a
    /// thenable `value` is followed, and settling an already-settled promise does nothing.
    ///
    /// Following a thenable reads its `then` property, which can run JavaScript. If that
    /// throws, the promise is rejected with the exception.
    ///
    /// # Errors
    ///
    /// Fails if execution is terminating.
    pub fn resolve(self, lock: &mut Lock<'s>, value: Local<'s, Value>) -> crate::Result<()> {
        self.debug_assert_isolate(lock);
        value.debug_assert_isolate(lock);
        // SAFETY: lock guarantees the isolate is locked and a HandleScope is active; self and
        // value are valid handles.
        unsafe {
            ffi::local_promise_resolver_resolve(lock.isolate().as_ffi(), self.raw(), value.raw())
        }
        .then_some(())
        .ok_or_else(terminated)
    }

    /// Rejects the promise with `reason`. Settling an already-settled promise does nothing.
    ///
    /// # Errors
    ///
    /// Fails if execution is terminating.
    pub fn reject(self, lock: &mut Lock<'s>, reason: Local<'s, Value>) -> crate::Result<()> {
        self.debug_assert_isolate(lock);
        reason.debug_assert_isolate(lock);
        // SAFETY: lock guarantees the isolate is locked and a HandleScope is active; self and
        // reason are valid handles.
        unsafe {
            ffi::local_promise_resolver_reject(lock.isolate().as_ffi(), self.raw(), reason.raw())
        }
        .then_some(())
        .ok_or_else(terminated)
    }

    /// Rejects the promise with the exception that throwing `err` would throw; see
    /// [`Lock::exception`].
    ///
    /// # Errors
    ///
    /// Fails if execution is terminating.
    pub fn reject_with_error(self, lock: &mut Lock<'s>, err: &Error) -> crate::Result<()> {
        let reason = lock.exception(err);
        self.reject(lock, reason)
    }
}

impl Promise {
    /// A promise resolved with `value`, like `Promise.resolve(value)`: a promise is returned
    /// unchanged and a thenable is followed.
    ///
    /// # Errors
    ///
    /// Fails if execution is terminating.
    pub fn resolved<'s>(
        lock: &mut Lock<'s>,
        value: Local<'s, Value>,
    ) -> crate::Result<Local<'s, Self>> {
        if let Some(promise) = value.try_as::<Self>() {
            return Ok(promise);
        }
        let resolver = PromiseResolver::new(lock)?;
        resolver.resolve(lock, value)?;
        Ok(resolver.promise())
    }

    /// A promise rejected with `reason`, like `Promise.reject(reason)`.
    ///
    /// # Errors
    ///
    /// Fails if execution is terminating.
    pub fn rejected<'s>(
        lock: &mut Lock<'s>,
        reason: Local<'s, Value>,
    ) -> crate::Result<Local<'s, Self>> {
        let resolver = PromiseResolver::new(lock)?;
        resolver.reject(lock, reason)?;
        Ok(resolver.promise())
    }

    /// A promise rejected with the exception that throwing `err` would throw; see
    /// [`Lock::exception`].
    ///
    /// # Errors
    ///
    /// Fails if execution is terminating.
    pub fn rejected_with_error<'s>(
        lock: &mut Lock<'s>,
        err: &Error,
    ) -> crate::Result<Local<'s, Self>> {
        let reason = lock.exception(err);
        Self::rejected(lock, reason)
    }
}

/// The data of the two functions [`Local::then`] registers, which share it.
struct Reactions<T>(std::rc::Rc<T>);

impl<T: Traced> Traced for Reactions<T> {
    fn trace(&self, visitor: &mut crate::GcVisitor) {
        self.0.trace(visitor);
    }
}

impl<'s> Local<'s, Promise> {
    /// The promise's state.
    pub fn state(self) -> PromiseState {
        // SAFETY: self is a valid promise handle.
        unsafe { ffi::local_promise_state(self.raw()) }
    }

    /// The fulfillment value or rejection reason, or `None` while the promise is pending.
    pub fn result(self, _lock: &mut Lock<'s>) -> Option<Local<'s, Value>> {
        if self.state() == PromiseState::Pending {
            return None;
        }
        // SAFETY: self is a valid promise handle, which is not pending.
        let result = unsafe { ffi::local_promise_result(self.raw()) }.ok()?;
        // SAFETY: the result is a handle in the lock's scope.
        Some(unsafe { Local::from_ffi(result) })
    }

    /// Marks the promise as handled, so that its rejection is not reported as unhandled.
    pub fn mark_as_handled(self) {
        // SAFETY: self is a valid promise handle.
        unsafe { ffi::local_promise_mark_as_handled(self.raw()) }
    }

    /// Registers `on_fulfilled` and `on_rejected` to run when the promise settles, like
    /// `promise.then(onFulfilled, onRejected)` without looking up `then`, and returns the promise
    /// for the result of whichever runs.
    ///
    /// The callbacks run as microtasks, with `data` and the fulfillment value or rejection
    /// reason. An `Ok` value resolves the returned promise (a promise or thenable is followed)
    /// and an `Err` rejects it with the exception that throwing it would throw. A panic throws an
    /// internal error and terminates execution, as in a `#[jsg_method]`.
    ///
    /// As with [`Function::new`], the callbacks must capture nothing, which a compile-time
    /// assertion checks, and whatever they need comes from `data`. The garbage collector traces
    /// `data` until the promise settles and drops it once neither callback can run, so it may
    /// hold `TracedGlobal`s and `jsg::Rc`s, including ones that lead back to the promise.
    ///
    /// Registering `on_rejected` marks the promise as handled, as in JavaScript.
    ///
    /// # Errors
    ///
    /// Fails if execution is terminating.
    pub fn then<T, F, R>(
        self,
        lock: &mut Lock<'s>,
        data: T,
        on_fulfilled: F,
        on_rejected: R,
    ) -> crate::Result<Self>
    where
        T: Traced + 'static,
        F: UnitType
            + 'static
            + for<'a> Fn(&mut Lock<'a>, &T, Local<'a, Value>) -> Result<Local<'a, Value>, Error>,
        R: UnitType
            + 'static
            + for<'a> Fn(&mut Lock<'a>, &T, Local<'a, Value>) -> Result<Local<'a, Value>, Error>,
    {
        self.debug_assert_isolate(lock);
        let _ = (on_fulfilled, on_rejected);
        let data = std::rc::Rc::new(data);
        let fulfilled = Function::new(
            lock,
            Reactions(std::rc::Rc::clone(&data)),
            |lock, data, args| {
                // SAFETY: `then` was handed a value of `F`.
                let f = unsafe { crate::scope::unit_value::<F>() };
                f(lock, &data.0, args.get(0))
            },
        )?;
        let rejected = Function::new(lock, Reactions(data), |lock, data, args| {
            // SAFETY: `then` was handed a value of `R`.
            let f = unsafe { crate::scope::unit_value::<R>() };
            f(lock, &data.0, args.get(0))
        })?;
        // SAFETY: lock guarantees the isolate is locked and a HandleScope is active; all three
        // are valid handles, and a non-empty result is a promise in the lock's scope.
        unsafe {
            Local::from_ffi_maybe(ffi::local_promise_then(
                lock.isolate().as_ffi(),
                self.raw(),
                fulfilled.raw(),
                rejected.raw(),
            ))
        }
        .ok_or_else(terminated)
    }
}

impl<'s> FromJS<'s> for Local<'s, Promise> {
    type ResultType = Self;

    /// Like C++ JSG's promise arguments: a promise is taken as is, and any other value becomes
    /// a promise resolved with it.
    fn from_js(lock: &mut Lock<'s>, value: Local<'s, Value>) -> Result<Self, Error> {
        Promise::resolved(lock, value)
    }

    fn type_name() -> String {
        "Promise".to_owned()
    }
}

impl_as!(Promise, is_promise);
impl_local_cast!(Promise -> Value, is_promise);
impl_local_cast!(Promise -> Object, is_promise);
