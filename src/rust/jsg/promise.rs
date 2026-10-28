// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! JavaScript promises: the [`Promise`] and [`PromiseResolver`] handles, which `jsg::v8`
//! exports with the other handle types.

#![allow(
    unsafe_code,
    reason = "settles and reacts to promises through the V8 FFI"
)]

use std::cell::RefCell;

use crate::Error;
use crate::FromJS;
use crate::Lock;
use crate::Traced;
use crate::UnitType;
use crate::v8::Function;
use crate::v8::FunctionCallbackInfo;
use crate::v8::Local;
use crate::v8::PromiseState;
use crate::v8::Value;
use crate::v8::ffi;

/// Marker for a `v8::Promise` handle.
///
/// A Rust method returns a promise like any other handle, as a `Local<'s, Promise>`, and
/// settles it through a [`PromiseResolver`], now or from a later call that holds the resolver in a
/// [`TracedGlobal`](crate::v8::TracedGlobal). [`Local::then`], [`Local::catch`] and
/// [`Local::then_or_catch`] react to a promise with Rust callbacks, which run as microtasks like
/// the reactions JavaScript registers.
///
/// An operation fails when V8 does and leaves an exception pending: a termination, or an
/// exception that a promise hook threw. It then returns an error for which [`Error::is_pending`]
/// is true, as [`Local::<Function>::call`] does, so that returning it propagates the exception.
///
/// The handles are untyped, like C++ JSG's `JsPromise`: values are JavaScript values, converted
/// with [`ToJS`](crate::ToJS) and [`FromJS`] at the edges. C++ JSG's typed `jsg::Promise<T>`,
/// which can carry C++ values through JavaScript, has no counterpart.
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
    /// Fails, leaving an exception pending, if V8 does; see [`Promise`].
    pub fn new<'s>(lock: &mut Lock<'s>) -> crate::Result<Local<'s, Self>> {
        // SAFETY: lock guarantees the isolate is locked and a HandleScope is active; a
        // non-empty handle is a resolver V8 created in the lock's scope.
        unsafe { Local::from_ffi_maybe(ffi::promise_resolver_new(lock.isolate().as_ffi())) }
            .ok_or_else(Error::pending)
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
    /// Fails, leaving an exception pending, if V8 does; see [`Promise`].
    pub fn resolve(self, lock: &mut Lock<'s>, value: Local<'s, Value>) -> crate::Result<()> {
        self.debug_assert_isolate(lock);
        value.debug_assert_isolate(lock);
        // SAFETY: lock guarantees the isolate is locked and a HandleScope is active; self and
        // value are valid handles.
        unsafe {
            ffi::local_promise_resolver_resolve(lock.isolate().as_ffi(), self.raw(), value.raw())
        }
        .then_some(())
        .ok_or_else(Error::pending)
    }

    /// Rejects the promise with `reason`. Settling an already-settled promise does nothing.
    ///
    /// # Errors
    ///
    /// Fails, leaving an exception pending, if V8 does; see [`Promise`].
    pub fn reject(self, lock: &mut Lock<'s>, reason: Local<'s, Value>) -> crate::Result<()> {
        self.debug_assert_isolate(lock);
        reason.debug_assert_isolate(lock);
        // SAFETY: lock guarantees the isolate is locked and a HandleScope is active; self and
        // reason are valid handles.
        unsafe {
            ffi::local_promise_resolver_reject(lock.isolate().as_ffi(), self.raw(), reason.raw())
        }
        .then_some(())
        .ok_or_else(Error::pending)
    }

    /// Rejects the promise with the exception that throwing `err` would throw; see
    /// [`Lock::exception`].
    ///
    /// # Errors
    ///
    /// Fails, leaving an exception pending, if V8 does; see [`Promise`]. Returns
    /// `err` itself, rejecting nothing, if [`Error::is_pending`] is true for it.
    pub fn reject_with_error(self, lock: &mut Lock<'s>, err: &Error) -> crate::Result<()> {
        let reason = lock.exception(err)?;
        self.reject(lock, reason)
    }
}

impl Promise {
    /// A promise resolved with `value`, like `Promise.resolve(value)`: a promise is returned
    /// unchanged and a thenable is followed.
    ///
    /// Unlike `Promise.resolve(value)`, which returns a promise unchanged only if its
    /// `constructor` is the current realm's `Promise`, this returns any promise unchanged,
    /// including an instance of a subclass and a promise from another context, as C++ JSG does
    /// for its promise arguments.
    ///
    /// # Errors
    ///
    /// Fails, leaving an exception pending, if V8 does; see [`Promise`].
    pub fn resolved<'s>(
        lock: &mut Lock<'s>,
        value: Local<'s, Value>,
    ) -> crate::Result<Local<'s, Self>> {
        if let Ok(promise) = Local::<Self>::try_from(value) {
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
    /// Fails, leaving an exception pending, if V8 does; see [`Promise`].
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
    /// Fails, leaving an exception pending, if V8 does; see [`Promise`]. Returns
    /// `err` itself if [`Error::is_pending`] is true for it.
    pub fn rejected_with_error<'s>(
        lock: &mut Lock<'s>,
        err: &Error,
    ) -> crate::Result<Local<'s, Self>> {
        let reason = lock.exception(err)?;
        Self::rejected(lock, reason)
    }
}

/// The data of a promise's reactions, which the first of them to run drops once it returns, as
/// C++ JSG drops a `ThenCatchPair`: a promise settles once, so the other can no longer run. Until
/// then, the garbage collector traces it through the reactions.
struct ReactionData<T>(RefCell<Option<T>>);

impl<T: Traced> Traced for ReactionData<T> {
    fn holds_gc_references() -> bool {
        T::holds_gc_references()
    }

    fn trace(&self, visitor: &mut crate::GcVisitor) {
        self.0.trace(visitor);
    }
}

/// The callback of a reaction that [`Local::then`], [`Local::catch`] or
/// [`Local::then_or_catch`] registers: calls `F` with the reaction's data and its argument, the
/// fulfillment value or rejection reason, then drops the data.
fn react<'a, T, F>(
    lock: &mut Lock<'a>,
    data: &ReactionData<T>,
    args: &mut FunctionCallbackInfo<'a>,
) -> Result<Local<'a, Value>, Error>
where
    T: Traced + 'static,
    F: UnitType
        + 'static
        + for<'b> Fn(&mut Lock<'b>, &T, Local<'b, Value>) -> Result<Local<'b, Value>, Error>,
{
    // SAFETY: `then`, `catch` and `then_or_catch` were handed a value of `F`.
    let f = unsafe { crate::scope::unit_value::<F>() };
    // The data stays in the cell, where the garbage collector traces it, while `f` runs.
    let result = match data.0.borrow().as_ref() {
        Some(data) => f(lock, data, args.get(0)),
        None => Err(Error::new_internal_error(
            "a promise reaction ran after another reaction to the promise",
        )),
    };
    // Dropped after the borrow ends, so that what its drop does cannot find the cell borrowed.
    let data = data.0.borrow_mut().take();
    drop(data);
    result
}

/// The callback of a function created with reaction data `T` that calls the reaction `F`.
fn reaction<T, F>() -> crate::RawFunctionCallback
where
    T: Traced + 'static,
    F: UnitType
        + 'static
        + for<'b> Fn(&mut Lock<'b>, &T, Local<'b, Value>) -> Result<Local<'b, Value>, Error>,
{
    Function::callback::<ReactionData<T>, _>(react::<T, F>)
}

/// The reaction [`Local::then`] registers for a rejection: passes it through.
fn pass_rejection<'a, T>(
    lock: &mut Lock<'a>,
    _data: &T,
    reason: Local<'a, Value>,
) -> Result<Local<'a, Value>, Error> {
    // SAFETY: `lock` guarantees the isolate is locked; `reason` is a handle in its scope.
    unsafe { ffi::isolate_throw_exception(lock.isolate().as_ffi(), reason.into_ffi()) };
    Err(Error::pending())
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
        // SAFETY: self is a valid promise handle, which is not pending, and the result is a
        // handle in the lock's scope.
        Some(unsafe { Local::from_ffi(ffi::local_promise_result(self.raw())) })
    }

    /// Marks the promise as handled, so that its rejection is not reported as unhandled.
    pub fn mark_as_handled(self) {
        // SAFETY: self is a valid promise handle.
        unsafe { ffi::local_promise_mark_as_handled(self.raw()) }
    }

    /// Registers `on_fulfilled` to run when the promise is fulfilled, like
    /// `promise.then(onFulfilled)`, and returns the promise for its result. A rejection passes
    /// through to the returned promise unchanged.
    ///
    /// The callback runs as a microtask, with `data` and the fulfillment value. An `Ok` value
    /// resolves the returned promise (a promise or thenable is followed) and an `Err` rejects it
    /// with the exception that throwing it would throw. A panic throws an internal error and
    /// terminates execution, as in a `#[jsg_method]`.
    ///
    /// Unlike `promise.then()`, this neither looks up `then` nor reads `promise.constructor`, so
    /// no JavaScript runs until the promise settles.
    ///
    /// As with [`Function::new`], the callback must capture nothing, which a compile-time
    /// assertion checks, and whatever it needs comes from `data`. As in C++ JSG, `data` is
    /// dropped as soon as the promise settles: after the callback returns, or when a rejection
    /// passes through. If the promise never settles, the garbage collector drops it once the
    /// promise is unreachable. Until then it traces `data`, so it may hold `TracedGlobal`s and
    /// `jsg::Rc`s, including ones that lead back to the promise. It must be their only owner that
    /// the garbage collector traces: to share a resource's state, hold a `jsg::Rc` or
    /// `jsg::Weak` to the resource rather than an `std::rc::Rc` of the state.
    ///
    /// # Errors
    ///
    /// Fails, leaving an exception pending, if V8 does; see [`Promise`].
    pub fn then<T, F>(self, lock: &mut Lock<'s>, data: T, on_fulfilled: F) -> crate::Result<Self>
    where
        T: Traced + 'static,
        F: UnitType
            + 'static
            + for<'a> Fn(&mut Lock<'a>, &T, Local<'a, Value>) -> Result<Local<'a, Value>, Error>,
    {
        // As C++ JSG does, a rejection passes through a reaction of its own, so that `data` is
        // dropped then too.
        self.then_or_catch(lock, data, on_fulfilled, pass_rejection::<T>)
    }

    /// Registers `on_rejected` to run when the promise is rejected, like
    /// `promise.catch(onRejected)`, and returns the promise for its result. A fulfillment passes
    /// through to the returned promise unchanged.
    ///
    /// Like [`Local::then`] otherwise, with the rejection reason as the callback's argument, and
    /// `data` dropped when a fulfillment passes through. Unlike `promise.catch()`, this does not
    /// call `promise.then`. Registering `on_rejected` marks the promise as handled, as in
    /// JavaScript.
    ///
    /// # Errors
    ///
    /// Fails, leaving an exception pending, if V8 does; see [`Promise`].
    pub fn catch<T, R>(self, lock: &mut Lock<'s>, data: T, on_rejected: R) -> crate::Result<Self>
    where
        T: Traced + 'static,
        R: UnitType
            + 'static
            + for<'a> Fn(&mut Lock<'a>, &T, Local<'a, Value>) -> Result<Local<'a, Value>, Error>,
    {
        // As C++ JSG does, a fulfillment passes through a reaction of its own, so that `data` is
        // dropped then too.
        self.then_or_catch(lock, data, |_, _, value| Ok(value), on_rejected)
    }

    /// Registers `on_fulfilled` and `on_rejected` to run when the promise settles, like
    /// `promise.then(onFulfilled, onRejected)`, and returns the promise for the result of
    /// whichever runs.
    ///
    /// Like [`Local::then`] and [`Local::catch`] otherwise. The two callbacks share `data`: one
    /// owner holds it for both, and the garbage collector traces it once.
    ///
    /// # Errors
    ///
    /// Fails, leaving an exception pending, if V8 does; see [`Promise`].
    pub fn then_or_catch<T, F, R>(
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
        let data = ReactionData(RefCell::new(Some(data)));
        let [on_fulfilled, on_rejected] =
            Function::new_sharing_data(lock, data, [reaction::<T, F>(), reaction::<T, R>()])?;
        // SAFETY: lock guarantees the isolate is locked and a HandleScope is active; all three
        // are valid handles, and a non-empty result is a promise in the lock's scope.
        unsafe {
            Local::from_ffi_maybe(ffi::local_promise_then_or_catch(
                lock.isolate().as_ffi(),
                self.raw(),
                on_fulfilled.raw(),
                on_rejected.raw(),
            ))
        }
        .ok_or_else(Error::pending)
    }
}

impl<'s> FromJS<'s> for Local<'s, Promise> {
    type ResultType = Self;

    /// Like C++ JSG's promise arguments: a promise is taken as is, and any other value becomes
    /// a promise resolved with it; see [`Promise::resolved`].
    fn from_js(lock: &mut Lock<'s>, value: Local<'s, Value>) -> Result<Self, Error> {
        Promise::resolved(lock, value)
    }

    fn type_name() -> String {
        "Promise".to_owned()
    }
}
