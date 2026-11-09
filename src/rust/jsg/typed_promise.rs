// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Typed promises: [`Promise<'s, T>`], the counterpart of C++ JSG's `jsg::Promise<T>`, with
//! [`PromiseResolver`] and the persistent [`TracedPromise`] and [`TracedPromiseResolver`].
//!
//! A typed promise carries a Rust value, as `jsg::Promise<T>` carries a C++ one: the underlying
//! JavaScript promise is settled with an opaque wrapper of the value (see `opaque.rs`), which
//! JavaScript never sees. The value is converted only where the promise crosses into or out of
//! JavaScript, exactly where C++ JSG converts it, so that a ported API settles its promises in the
//! same microtasks and runs the same conversions as the C++ one:
//!
//! - [`ToJS`] returns the promise of a reaction that converts the value with `ToJS`, as C++
//!   `PromiseWrapper::wrap()` does with `thenWrap()`. JavaScript gets that promise, which settles
//!   one microtask after the one it derives from, and is marked as handled if that one was. The
//!   reaction holds the promise's creator, the resource whose method returned it
//!   ([`ToJS::to_js_with_creator`]), so the resource lives until the promise settles.
//! - [`FromJS`] takes a promise and adds a reaction that converts its value once, as
//!   `PromiseWrapper::tryUnwrap()` does with `thenUnwrap()`. A value of the wrong type rejects the
//!   promise with C++ JSG's `TypeError`. A value that is not a promise is converted at once and
//!   carried by a resolved promise; a thenable is followed first if `unwrap_custom_thenables` is
//!   on.
//!
//! [`PromiseValue`] says how a type is carried: a Rust value in an opaque wrapper, `()` as
//! `undefined` (`jsg::Promise<void>`), or a JavaScript value as itself for [`v8::Value`]
//! (`jsg::Promise<jsg::Value>`). Untyped promises, `Local<'s, v8::Promise>`, stay the counterpart
//! of C++ JSG's `JsPromise`.
//!
//! As in C++, a typed promise is used once: [`Promise::then`], [`Promise::catch`] and
//! [`Promise::then_or_catch`] consume it, and only one reaction ever takes its value. Their
//! `_promise` variants take callbacks that return promises, which the returned promise follows.

#![allow(
    unsafe_code,
    reason = "recreates the zero-sized reaction callbacks it is handed"
)]

use std::any::Any;
use std::any::TypeId;
use std::marker::PhantomData;

use crate::Error;
use crate::FromJS;
use crate::Lock;
use crate::ToJS;
use crate::Traced;
use crate::Type;
use crate::TypeInfo;
use crate::UnitType;
use crate::opaque::unwrap_opaque;
use crate::opaque::wrap_opaque;
use crate::scope::unit_value;
use crate::v8;
use crate::v8::Local;
use crate::v8::PromiseState;
use crate::v8::TracedGlobal;
use crate::v8::Value;

/// How a value of a [`Promise<'s, Self>`](Promise) is carried by the JavaScript promise under it,
/// as C++ JSG's `PromiseWrapper` treats the `T` of `jsg::Promise<T>`.
///
/// - Any [`Traced`] type is carried as an opaque wrapper of the value, which JavaScript never sees.
///   Its reactions take the value. `()` is carried as `undefined`, as `jsg::Promise<void>` is.
/// - [`v8::Value`] is carried as the JavaScript value itself, as `jsg::Promise<jsg::Value>` is.
///   Its reactions take a `Local<'a, v8::Value>`.
pub trait PromiseValue: sealed::Sealed + 'static {
    /// What the promise's reactions take and its resolver settles it with.
    type Value<'a>;

    /// The JavaScript value that settles the underlying promise with `value`.
    #[doc(hidden)]
    fn into_raw<'a>(lock: &mut Lock<'a>, value: Self::Value<'a>)
    -> crate::Result<Local<'a, Value>>;

    /// The value that the underlying promise was settled with, as [`Self::into_raw`] made it.
    ///
    /// Fails with an internal error if the promise was settled with anything else. Rust JSG never
    /// does that, but JavaScript can, through a `Promise.prototype.then` that it replaced: V8
    /// calls it when [`PromiseResolver::resolve_with_promise`] resolves a promise with another,
    /// as C++ JSG's `Resolver::resolve()` does, and C++ JSG fails in the same way.
    #[doc(hidden)]
    fn from_raw<'a>(lock: &mut Lock<'a>, raw: Local<'a, Value>) -> crate::Result<Self::Value<'a>>;
}

/// Keeps [`PromiseValue`], and with it [`PromiseArgument`], to the impls in this module, which
/// are what keeps every typed promise settled with a value of its type.
mod sealed {
    pub trait Sealed {}

    impl<T: crate::Traced + 'static> Sealed for T {}

    impl Sealed for crate::v8::Value {}
}

/// `()` if `T` is `()`.
fn unit<T: 'static>() -> Option<T> {
    let unit: Box<dyn Any> = Box::new(());
    unit.downcast::<T>().ok().map(|unit| *unit)
}

impl<T: Traced + 'static> PromiseValue for T {
    type Value<'a> = Self;

    fn into_raw<'a>(lock: &mut Lock<'a>, value: Self) -> crate::Result<Local<'a, Value>> {
        if TypeId::of::<Self>() == TypeId::of::<()>() {
            return Ok(lock.undefined());
        }
        wrap_opaque(lock, value)
    }

    fn from_raw<'a>(lock: &mut Lock<'a>, raw: Local<'a, Value>) -> crate::Result<Self> {
        // A `Promise<()>` from JavaScript may have been settled with any value, which C++ JSG
        // ignores too.
        if let Some(unit) = unit::<Self>() {
            return Ok(unit);
        }
        unwrap_opaque(lock, raw).ok_or_else(|| {
            Error::new_internal_error(format!(
                "a Promise<{}> was settled with a value other than its own",
                std::any::type_name::<Self>()
            ))
        })
    }
}

/// Coexists with the impl for every `Traced` type only because `v8::Value` is not `Traced`.
impl PromiseValue for v8::Value {
    type Value<'a> = Local<'a, Self>;

    fn into_raw<'a>(
        _lock: &mut Lock<'a>,
        value: Local<'a, Self>,
    ) -> crate::Result<Local<'a, Self>> {
        Ok(value)
    }

    fn from_raw<'a>(_lock: &mut Lock<'a>, raw: Local<'a, Self>) -> crate::Result<Local<'a, Self>> {
        Ok(raw)
    }
}

/// How a JavaScript value converts to a [`Promise<'s, Self>`](Promise) argument, as C++ JSG's
/// `PromiseWrapper::tryUnwrap()` converts one to a `jsg::Promise<T>`.
pub trait PromiseArgument: PromiseValue {
    /// The typed promise for `promise`, a JavaScript promise.
    #[doc(hidden)]
    fn from_promise<'a>(
        lock: &mut Lock<'a>,
        promise: Local<'a, v8::Promise>,
    ) -> crate::Result<Local<'a, v8::Promise>>;

    /// A typed promise resolved with `value`, which is not a promise, or `None` if `value` is not
    /// of the type.
    #[doc(hidden)]
    fn try_from_value<'a>(
        lock: &mut Lock<'a>,
        value: Local<'a, Value>,
    ) -> crate::Result<Option<Local<'a, v8::Promise>>>;
}

/// As for `jsg::Promise<void>`: a promise is taken as it is, and any other value becomes a promise
/// resolved with `undefined`, whatever it is.
///
/// Coexists with the impl for every `Traced` type that converts from JavaScript only because `()`
/// does not convert from JavaScript.
impl PromiseArgument for () {
    fn from_promise<'a>(
        _lock: &mut Lock<'a>,
        promise: Local<'a, v8::Promise>,
    ) -> crate::Result<Local<'a, v8::Promise>> {
        Ok(promise)
    }

    fn try_from_value<'a>(
        lock: &mut Lock<'a>,
        _value: Local<'a, Value>,
    ) -> crate::Result<Option<Local<'a, v8::Promise>>> {
        let undefined = lock.undefined();
        v8::Promise::resolved(lock, undefined).map(Some)
    }
}

/// As for `jsg::Promise<jsg::Value>`: a promise is taken as it is, and any other value becomes a
/// promise resolved with it, which follows a thenable.
///
/// Coexists with the impl for every `Traced` type only because `v8::Value` is not `Traced`.
impl PromiseArgument for v8::Value {
    fn from_promise<'a>(
        _lock: &mut Lock<'a>,
        promise: Local<'a, v8::Promise>,
    ) -> crate::Result<Local<'a, v8::Promise>> {
        Ok(promise)
    }

    fn try_from_value<'a>(
        lock: &mut Lock<'a>,
        value: Local<'a, Value>,
    ) -> crate::Result<Option<Local<'a, v8::Promise>>> {
        v8::Promise::resolved(lock, value).map(Some)
    }
}

/// As for a `jsg::Promise<T>` of a C++ type: a promise gets a reaction that converts its value,
/// and any other value is converted at once.
impl<T> PromiseArgument for T
where
    T: Traced + 'static + for<'a> FromJS<'a, ResultType = T>,
{
    fn from_promise<'a>(
        lock: &mut Lock<'a>,
        promise: Local<'a, v8::Promise>,
    ) -> crate::Result<Local<'a, v8::Promise>> {
        promise.then(lock, (), convert_resolution::<T>)
    }

    fn try_from_value<'a>(
        lock: &mut Lock<'a>,
        value: Local<'a, Value>,
    ) -> crate::Result<Option<Local<'a, v8::Promise>>> {
        let Some(value) = T::try_from_js(lock, value)? else {
            return Ok(None);
        };
        Promise::<T>::resolved(lock, value).map(|promise| Some(promise.handle))
    }
}

/// The reaction that converts what a promise from JavaScript resolved with, as C++ JSG's
/// `thenUnwrap()` does.
#[expect(
    clippy::trivially_copy_pass_by_ref,
    reason = "has the signature of a reaction, which borrows its data"
)]
fn convert_resolution<'a, T>(
    lock: &mut Lock<'a>,
    _data: &(),
    value: Local<'a, Value>,
) -> crate::Result<Local<'a, Value>>
where
    T: Traced + 'static + for<'b> FromJS<'b, ResultType = T>,
{
    let Some(value) = T::try_from_js(lock, value)? else {
        return Err(Error::new_type_error(format!(
            "Incorrect type for Promise: the Promise did not resolve to '{}'.",
            T::type_name()
        )));
    };
    wrap_opaque(lock, value)
}

/// The reaction that converts a typed promise's value to JavaScript, as C++ JSG's `thenWrap()`
/// does. Its data is the promise's creator, which it keeps alive until it runs, as `thenWrap()`'s
/// does.
#[expect(
    clippy::ref_option,
    reason = "has the signature of a reaction, which borrows its data"
)]
fn convert_to_js<'a, T>(
    lock: &mut Lock<'a>,
    _creator: &Option<TracedGlobal<v8::Object>>,
    raw: Local<'a, Value>,
) -> crate::Result<Local<'a, Value>>
where
    T: PromiseValue,
    for<'b> T::Value<'b>: ToJS<'b>,
{
    T::from_raw(lock, raw)?.to_js(lock)
}

/// The reaction that calls the `on_fulfilled` callback `F` of [`Promise::then`] or
/// [`Promise::then_or_catch`] with the value of a `Promise<T>`, for a `Promise<U>`.
fn on_fulfilled_reaction<'a, D, T, U, F>(
    lock: &mut Lock<'a>,
    data: &D,
    raw: Local<'a, Value>,
) -> crate::Result<Local<'a, Value>>
where
    T: PromiseValue,
    U: PromiseValue,
    F: UnitType + for<'b> Fn(&mut Lock<'b>, &D, T::Value<'b>) -> crate::Result<U::Value<'b>>,
{
    let value = T::from_raw(lock, raw)?;
    // SAFETY: `then` and `then_or_catch` were handed a value of `F`.
    let f = unsafe { unit_value::<F>() };
    let value = f(lock, data, value)?;
    U::into_raw(lock, value)
}

/// The reaction that calls the `on_rejected` callback `R` of [`Promise::catch`] or
/// [`Promise::then_or_catch`] with the rejection reason, for a `Promise<U>`.
fn on_rejected_reaction<'a, D, U, R>(
    lock: &mut Lock<'a>,
    data: &D,
    reason: Local<'a, Value>,
) -> crate::Result<Local<'a, Value>>
where
    U: PromiseValue,
    R: UnitType + for<'b> Fn(&mut Lock<'b>, &D, Local<'b, Value>) -> crate::Result<U::Value<'b>>,
{
    // SAFETY: `catch` and `then_or_catch` were handed a value of `R`.
    let r = unsafe { unit_value::<R>() };
    let value = r(lock, data, reason)?;
    U::into_raw(lock, value)
}

/// The reaction that calls the `on_fulfilled` callback `F` of [`Promise::then_promise`] or
/// [`Promise::then_or_catch_promise`] with the value of a `Promise<T>`, and resolves the reaction's
/// promise with the `Promise<U>` it returns, which V8 then follows, as C++ JSG's
/// `promiseContinuation()` does for a continuation that returns a promise.
fn on_fulfilled_promise_reaction<'a, D, T, U, F>(
    lock: &mut Lock<'a>,
    data: &D,
    raw: Local<'a, Value>,
) -> crate::Result<Local<'a, Value>>
where
    T: PromiseValue,
    U: PromiseValue,
    F: UnitType + for<'b> Fn(&mut Lock<'b>, &D, T::Value<'b>) -> crate::Result<Promise<'b, U>>,
{
    let value = T::from_raw(lock, raw)?;
    // SAFETY: `then_promise` and `then_or_catch_promise` were handed a value of `F`.
    let f = unsafe { unit_value::<F>() };
    Ok(f(lock, data, value)?.handle.into())
}

/// The reaction that calls the `on_rejected` callback `R` of [`Promise::catch_promise`] or
/// [`Promise::then_or_catch_promise`] with the rejection reason, and resolves the reaction's
/// promise with the `Promise<U>` it returns.
fn on_rejected_promise_reaction<'a, D, U, R>(
    lock: &mut Lock<'a>,
    data: &D,
    reason: Local<'a, Value>,
) -> crate::Result<Local<'a, Value>>
where
    U: PromiseValue,
    R: UnitType + for<'b> Fn(&mut Lock<'b>, &D, Local<'b, Value>) -> crate::Result<Promise<'b, U>>,
{
    // SAFETY: `catch_promise` and `then_or_catch_promise` were handed a value of `R`.
    let r = unsafe { unit_value::<R>() };
    Ok(r(lock, data, reason)?.handle.into())
}

/// A promise that carries a `T`, the counterpart of C++ JSG's `jsg::Promise<T>`; see the module
/// docs.
///
/// A `#[jsg_method]` returns one like any other value. It is used once: reacting to it consumes
/// it. To keep it beyond the current scope, as in a resource field, make it a [`TracedPromise`]
/// with [`Promise::into_traced`].
#[must_use = "a promise that nothing reacts to drops its value"]
pub struct Promise<'s, T: PromiseValue> {
    handle: Local<'s, v8::Promise>,
    /// Whether [`Promise::mark_as_handled`] marked the promise, which the promise JavaScript gets
    /// from [`ToJS`] then is too, as in C++.
    handled: bool,
    _marker: PhantomData<fn() -> T>,
}

impl<'s, T: PromiseValue> Promise<'s, T> {
    fn new(handle: Local<'s, v8::Promise>) -> Self {
        Self {
            handle,
            handled: false,
            _marker: PhantomData,
        }
    }

    /// A promise resolved with `value`, like C++ JSG's `Lock::resolvedPromise()`.
    ///
    /// # Errors
    ///
    /// Fails, leaving an exception pending, if V8 does; see [`v8::Promise`].
    pub fn resolved(lock: &mut Lock<'s>, value: T::Value<'s>) -> crate::Result<Self> {
        let (promise, resolver) = Self::new_with_resolver(lock)?;
        resolver.resolve(lock, value)?;
        Ok(promise)
    }

    /// A promise rejected with `reason`, like C++ JSG's `Lock::rejectedPromise()`.
    ///
    /// # Errors
    ///
    /// Fails, leaving an exception pending, if V8 does; see [`v8::Promise`].
    pub fn rejected(lock: &mut Lock<'s>, reason: Local<'s, Value>) -> crate::Result<Self> {
        v8::Promise::rejected(lock, reason).map(Self::new)
    }

    /// A promise rejected with the exception that throwing `err` would throw; see
    /// [`Lock::exception`].
    ///
    /// # Errors
    ///
    /// Fails, leaving an exception pending, if V8 does; see [`v8::Promise`]. Returns `err` itself
    /// if [`Error::is_pending`] is true for it.
    pub fn rejected_with_error(lock: &mut Lock<'s>, err: &Error) -> crate::Result<Self> {
        v8::Promise::rejected_with_error(lock, err).map(Self::new)
    }

    /// A pending promise and the resolver that settles it, like C++ JSG's
    /// `Lock::newPromiseAndResolver()`.
    ///
    /// # Errors
    ///
    /// Fails, leaving an exception pending, if V8 does; see [`v8::Promise`].
    pub fn new_with_resolver(lock: &mut Lock<'s>) -> crate::Result<(Self, PromiseResolver<'s, T>)> {
        let resolver = v8::PromiseResolver::new(lock)?;
        Ok((
            Self::new(resolver.promise()),
            PromiseResolver::new(resolver),
        ))
    }

    /// Marks the promise as handled, so that its rejection is not reported as unhandled. The
    /// promise that [`ToJS`] gives JavaScript is marked too.
    pub fn mark_as_handled(&mut self) {
        self.handle.mark_as_handled();
        self.handled = true;
    }

    /// Registers `on_fulfilled` to run with the value when the promise is fulfilled, and returns
    /// the promise for what it returns. A rejection passes through unchanged.
    ///
    /// Works as [`Local::<v8::Promise>::then`] does, which describes how `data` is owned, traced
    /// and dropped; `on_fulfilled` takes the promise's value rather than a JavaScript value, and
    /// returns the value of the `Promise<U>`.
    ///
    /// # Errors
    ///
    /// Fails, leaving an exception pending, if V8 does; see [`v8::Promise`].
    pub fn then<D, U, F>(
        self,
        lock: &mut Lock<'s>,
        data: D,
        on_fulfilled: F,
    ) -> crate::Result<Promise<'s, U>>
    where
        D: Traced + 'static,
        U: PromiseValue,
        F: UnitType
            + 'static
            + for<'a> Fn(&mut Lock<'a>, &D, T::Value<'a>) -> crate::Result<U::Value<'a>>,
    {
        let _ = on_fulfilled;
        self.handle
            .then(lock, data, on_fulfilled_reaction::<D, T, U, F>)
            .map(Promise::new)
    }

    /// Registers `on_rejected` to run with the rejection reason when the promise is rejected, and
    /// returns the promise for what it returns, of the same type. A fulfillment passes through.
    ///
    /// Works as [`Local::<v8::Promise>::catch`] does; `on_rejected` returns the promise's value.
    ///
    /// # Errors
    ///
    /// Fails, leaving an exception pending, if V8 does; see [`v8::Promise`].
    pub fn catch<D, R>(self, lock: &mut Lock<'s>, data: D, on_rejected: R) -> crate::Result<Self>
    where
        D: Traced + 'static,
        R: UnitType
            + 'static
            + for<'a> Fn(&mut Lock<'a>, &D, Local<'a, Value>) -> crate::Result<T::Value<'a>>,
    {
        let _ = on_rejected;
        self.handle
            .catch(lock, data, on_rejected_reaction::<D, T, R>)
            .map(Self::new)
    }

    /// Registers `on_fulfilled` and `on_rejected`, which share `data`, and returns the promise for
    /// what whichever runs returns.
    ///
    /// Works as [`Local::<v8::Promise>::then_or_catch`] does, with the callbacks of
    /// [`Promise::then`] and [`Promise::catch`].
    ///
    /// # Errors
    ///
    /// Fails, leaving an exception pending, if V8 does; see [`v8::Promise`].
    pub fn then_or_catch<D, U, F, R>(
        self,
        lock: &mut Lock<'s>,
        data: D,
        on_fulfilled: F,
        on_rejected: R,
    ) -> crate::Result<Promise<'s, U>>
    where
        D: Traced + 'static,
        U: PromiseValue,
        F: UnitType
            + 'static
            + for<'a> Fn(&mut Lock<'a>, &D, T::Value<'a>) -> crate::Result<U::Value<'a>>,
        R: UnitType
            + 'static
            + for<'a> Fn(&mut Lock<'a>, &D, Local<'a, Value>) -> crate::Result<U::Value<'a>>,
    {
        let _ = (on_fulfilled, on_rejected);
        self.handle
            .then_or_catch(
                lock,
                data,
                on_fulfilled_reaction::<D, T, U, F>,
                on_rejected_reaction::<D, U, R>,
            )
            .map(Promise::new)
    }

    /// Like [`Promise::then`], for an `on_fulfilled` that returns a promise: the returned promise
    /// follows it, as in C++ JSG, whose `then()` chains a continuation that returns a
    /// `jsg::Promise<U>`.
    ///
    /// # Errors
    ///
    /// Fails, leaving an exception pending, if V8 does; see [`v8::Promise`].
    pub fn then_promise<D, U, F>(
        self,
        lock: &mut Lock<'s>,
        data: D,
        on_fulfilled: F,
    ) -> crate::Result<Promise<'s, U>>
    where
        D: Traced + 'static,
        U: PromiseValue,
        F: UnitType
            + 'static
            + for<'a> Fn(&mut Lock<'a>, &D, T::Value<'a>) -> crate::Result<Promise<'a, U>>,
    {
        let _ = on_fulfilled;
        self.handle
            .then(lock, data, on_fulfilled_promise_reaction::<D, T, U, F>)
            .map(Promise::new)
    }

    /// Like [`Promise::catch`], for an `on_rejected` that returns a promise, which the returned
    /// promise follows.
    ///
    /// # Errors
    ///
    /// Fails, leaving an exception pending, if V8 does; see [`v8::Promise`].
    pub fn catch_promise<D, R>(
        self,
        lock: &mut Lock<'s>,
        data: D,
        on_rejected: R,
    ) -> crate::Result<Self>
    where
        D: Traced + 'static,
        R: UnitType
            + 'static
            + for<'a> Fn(&mut Lock<'a>, &D, Local<'a, Value>) -> crate::Result<Promise<'a, T>>,
    {
        let _ = on_rejected;
        self.handle
            .catch(lock, data, on_rejected_promise_reaction::<D, T, R>)
            .map(Self::new)
    }

    /// Like [`Promise::then_or_catch`], for callbacks that return promises, which the returned
    /// promise follows.
    ///
    /// # Errors
    ///
    /// Fails, leaving an exception pending, if V8 does; see [`v8::Promise`].
    pub fn then_or_catch_promise<D, U, F, R>(
        self,
        lock: &mut Lock<'s>,
        data: D,
        on_fulfilled: F,
        on_rejected: R,
    ) -> crate::Result<Promise<'s, U>>
    where
        D: Traced + 'static,
        U: PromiseValue,
        F: UnitType
            + 'static
            + for<'a> Fn(&mut Lock<'a>, &D, T::Value<'a>) -> crate::Result<Promise<'a, U>>,
        R: UnitType
            + 'static
            + for<'a> Fn(&mut Lock<'a>, &D, Local<'a, Value>) -> crate::Result<Promise<'a, U>>,
    {
        let _ = (on_fulfilled, on_rejected);
        self.handle
            .then_or_catch(
                lock,
                data,
                on_fulfilled_promise_reaction::<D, T, U, F>,
                on_rejected_promise_reaction::<D, U, R>,
            )
            .map(Promise::new)
    }

    /// A promise that settles when this one does, without its value, like C++ JSG's
    /// `whenResolved()`. Unlike reacting, this does not consume the promise, and the promise it
    /// returns is marked as handled if this one is.
    pub fn when_resolved(&self) -> Promise<'s, ()> {
        Promise {
            handle: self.handle,
            handled: self.handled,
            _marker: PhantomData,
        }
    }

    /// The value, if the promise is fulfilled, like C++ JSG's `tryConsumeResolved()`; otherwise
    /// the promise itself. Use it as an optimization or in tests, never for correctness.
    ///
    /// # Errors
    ///
    /// Returns the promise if it is pending or rejected, or if it was settled with a value other
    /// than its own (see [`PromiseValue::from_raw`]).
    pub fn try_consume_resolved(self, lock: &mut Lock<'s>) -> Result<T::Value<'s>, Self> {
        if self.handle.state() != PromiseState::Fulfilled {
            return Err(self);
        }
        let Some(raw) = self.handle.result(lock) else {
            unreachable!("a fulfilled promise has a result");
        };
        T::from_raw(lock, raw).map_err(|_| self)
    }

    /// Makes the promise persistent, to keep it beyond the current scope.
    pub fn into_traced(self, lock: &mut Lock<'s>) -> TracedPromise<T> {
        TracedPromise {
            handle: TracedGlobal::new(lock, self.handle),
            handled: self.handled,
            _marker: PhantomData,
        }
    }
}

/// Converts the value to JavaScript in a reaction, as C++ JSG does when a method returns a
/// `jsg::Promise<T>`; see the module docs.
impl<'s, T> ToJS<'s> for Promise<'s, T>
where
    T: PromiseValue,
    for<'a> T::Value<'a>: ToJS<'a>,
{
    fn to_js(self, lock: &mut Lock<'s>) -> crate::Result<Local<'s, Value>> {
        self.to_js_with_creator(lock, None)
    }

    /// Keeps `creator` alive until the promise settles, as C++ JSG's `PromiseWrapper::wrap()`
    /// does: the object whose method returned the promise may own what settles it.
    fn to_js_with_creator(
        self,
        lock: &mut Lock<'s>,
        creator: Option<Local<'s, v8::Object>>,
    ) -> crate::Result<Local<'s, Value>> {
        let creator = creator.map(|creator| creator.to_traced_global(lock));
        let promise = self.handle.then(lock, creator, convert_to_js::<T>)?;
        if self.handled {
            promise.mark_as_handled();
        }
        Ok(promise.into())
    }
}

/// Converts an argument as C++ JSG converts one to a `jsg::Promise<T>`; see the module docs and
/// [`PromiseArgument`].
impl<'s, T: PromiseArgument> FromJS<'s> for Promise<'s, T> {
    type ResultType = Self;

    fn type_name() -> String {
        // C++ JSG's name for every `jsg::Promise<T>`.
        "Promise".to_owned()
    }

    fn from_js(lock: &mut Lock<'s>, value: Local<'s, Value>) -> crate::Result<Self> {
        let type_name = value.type_of(lock);
        Self::try_from_js(lock, value)?
            .ok_or_else(|| Error::new_type_error(format!("Expected Promise but got {type_name}")))
    }

    fn try_from_js(lock: &mut Lock<'s>, value: Local<'s, Value>) -> crate::Result<Option<Self>> {
        if let Ok(promise) = Local::<v8::Promise>::try_from(value) {
            return T::from_promise(lock, promise).map(|promise| Some(Self::new(promise)));
        }
        // As in C++ JSG, behind a flag since existing Workers rely on thenables not being followed.
        if lock.feature_flags().get_unwrap_custom_thenables() && is_thenable(lock, value)? {
            let resolver = v8::PromiseResolver::new(lock)?;
            resolver.resolve(lock, value)?;
            return T::from_promise(lock, resolver.promise())
                .map(|promise| Some(Self::new(promise)));
        }
        T::try_from_value(lock, value).map(|promise| promise.map(Self::new))
    }
}

/// Whether `value` is an object with a `then` property, as C++ JSG's `PromiseWrapper::isThenable()`
/// checks it: through `has`, which a Proxy can trap.
fn is_thenable<'s>(lock: &mut Lock<'s>, value: Local<'s, Value>) -> crate::Result<bool> {
    match Local::<v8::Object>::try_from(value) {
        Ok(object) => object.has(lock, "then"),
        Err(_) => Ok(false),
    }
}

/// `Promise<T>`, as C++ describes `jsg::Promise<T>`: `Promise<void>` for `()` and
/// `Promise<any>` for [`v8::Value`].
impl<T: PromiseValue + TypeInfo> TypeInfo for Promise<'_, T> {
    fn describe(cx: &mut crate::rtti::Context) -> crate::rtti::Type {
        crate::rtti::Type::Promise(Box::new(T::describe(cx)))
    }
}

impl<T: PromiseValue> Type for Promise<'_, T> {
    const UNION_NAMES: &'static [&'static str] = <v8::Promise as Type>::UNION_NAMES;
    const UNION_TYPES: crate::UnionTypes = <v8::Promise as Type>::UNION_TYPES;

    fn class_name() -> &'static str {
        "Promise"
    }

    fn is_exact(value: Local<'_, Value>) -> bool {
        value.is_promise()
    }
}

/// The capability to settle one [`Promise<'s, T>`](Promise), like C++ JSG's
/// `jsg::Promise<T>::Resolver`. Settling a settled promise does nothing.
///
/// To keep it beyond the current scope, as in a resource field, make it a
/// [`TracedPromiseResolver`] with [`PromiseResolver::into_traced`].
pub struct PromiseResolver<'s, T: PromiseValue> {
    handle: Local<'s, v8::PromiseResolver>,
    _marker: PhantomData<fn() -> T>,
}

impl<'s, T: PromiseValue> PromiseResolver<'s, T> {
    const fn new(handle: Local<'s, v8::PromiseResolver>) -> Self {
        Self {
            handle,
            _marker: PhantomData,
        }
    }

    /// Resolves the promise with `value`.
    ///
    /// # Errors
    ///
    /// Fails, leaving an exception pending, if V8 does; see [`v8::Promise`].
    pub fn resolve(self, lock: &mut Lock<'s>, value: T::Value<'s>) -> crate::Result<()> {
        let raw = T::into_raw(lock, value)?;
        self.handle.resolve(lock, raw)
    }

    /// Resolves the promise with `promise`, which it then follows, like C++ JSG's
    /// `Resolver::resolve(js, Promise&&)`.
    ///
    /// # Errors
    ///
    /// Fails, leaving an exception pending, if V8 does; see [`v8::Promise`].
    #[expect(
        clippy::needless_pass_by_value,
        reason = "takes the promise, which is used once, as C++ JSG's takes a `Promise&&`"
    )]
    pub fn resolve_with_promise(
        self,
        lock: &mut Lock<'s>,
        promise: Promise<'s, T>,
    ) -> crate::Result<()> {
        self.handle.resolve(lock, promise.handle.into())
    }

    /// Rejects the promise with `reason`.
    ///
    /// # Errors
    ///
    /// Fails, leaving an exception pending, if V8 does; see [`v8::Promise`].
    pub fn reject(self, lock: &mut Lock<'s>, reason: Local<'s, Value>) -> crate::Result<()> {
        self.handle.reject(lock, reason)
    }

    /// Rejects the promise with the exception that throwing `err` would throw; see
    /// [`Lock::exception`].
    ///
    /// # Errors
    ///
    /// Fails, leaving an exception pending, if V8 does; see [`v8::Promise`]. Returns `err` itself,
    /// rejecting nothing, if [`Error::is_pending`] is true for it.
    pub fn reject_with_error(self, lock: &mut Lock<'s>, err: &Error) -> crate::Result<()> {
        self.handle.reject_with_error(lock, err)
    }

    /// Makes the resolver persistent, to keep it beyond the current scope.
    pub fn into_traced(self, lock: &mut Lock<'s>) -> TracedPromiseResolver<T> {
        TracedPromiseResolver {
            handle: TracedGlobal::new(lock, self.handle),
            _marker: PhantomData,
        }
    }
}

/// A persistent [`Promise<'s, T>`](Promise), for a resource field, which the garbage collector
/// traces. Like a `Promise`, it is used once: [`TracedPromise::into_local`] consumes it.
pub struct TracedPromise<T: PromiseValue> {
    handle: TracedGlobal<v8::Promise>,
    handled: bool,
    _marker: PhantomData<fn() -> T>,
}

impl<T: PromiseValue> TracedPromise<T> {
    /// The promise, in the scope of `lock`.
    pub fn into_local<'s>(self, lock: &mut Lock<'s>) -> Promise<'s, T> {
        Promise {
            handle: self.handle.as_local(lock),
            handled: self.handled,
            _marker: PhantomData,
        }
    }
}

/// An edge to the promise in the V8 heap, as C++ reports a `jsg::Promise`.
impl<T: PromiseValue> crate::MemoryInfo for TracedPromise<T> {
    fn track_as_field(
        &self,
        tracker: &mut crate::MemoryTracker<'_>,
        edge_name: Option<&'static std::ffi::CStr>,
    ) {
        self.handle.track_as_field(tracker, edge_name);
    }
}

impl<T: PromiseValue> Traced for TracedPromise<T> {
    fn trace(&self, visitor: &mut crate::GcVisitor) {
        self.handle.trace(visitor);
    }
}

/// A persistent [`PromiseResolver<'s, T>`](PromiseResolver), for a resource field, which the
/// garbage collector traces.
pub struct TracedPromiseResolver<T: PromiseValue> {
    handle: TracedGlobal<v8::PromiseResolver>,
    _marker: PhantomData<fn() -> T>,
}

impl<T: PromiseValue> TracedPromiseResolver<T> {
    /// The resolver, in the scope of `lock`.
    pub fn as_local<'s>(&self, lock: &mut Lock<'s>) -> PromiseResolver<'s, T> {
        PromiseResolver::new(self.handle.as_local(lock))
    }
}

/// An edge to the resolver in the V8 heap, as C++ reports a `jsg::Promise::Resolver`.
impl<T: PromiseValue> crate::MemoryInfo for TracedPromiseResolver<T> {
    fn track_as_field(
        &self,
        tracker: &mut crate::MemoryTracker<'_>,
        edge_name: Option<&'static std::ffi::CStr>,
    ) {
        self.handle.track_as_field(tracker, edge_name);
    }
}

impl<T: PromiseValue> Traced for TracedPromiseResolver<T> {
    fn trace(&self, visitor: &mut crate::GcVisitor) {
        self.handle.trace(visitor);
    }
}

impl<'s> Lock<'s> {
    /// Runs `f` and returns its promise, or a promise rejected with what `f` failed with, like C++
    /// JSG's `Lock::evalNow()`: an API that returns a promise reports its errors by rejecting it
    /// rather than by throwing.
    ///
    /// The rejection is the exception JavaScript threw, for an error for which
    /// [`Error::is_pending`] is true, or the exception that throwing `f`'s error would throw. A
    /// panic in `f` unwinds through this, as elsewhere.
    ///
    /// # Errors
    ///
    /// Fails if execution is terminating, or if V8 fails to create the rejected promise.
    pub fn eval_now<T, F>(&mut self, f: F) -> crate::Result<Promise<'s, T>>
    where
        T: PromiseValue,
        F: FnOnce(&mut Self) -> crate::Result<Promise<'s, T>>,
    {
        match self.try_catch(f)? {
            Ok(promise) => Ok(promise),
            Err(exception) => Promise::rejected(self, exception),
        }
    }
}
