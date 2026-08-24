// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Typed persistent handles to JavaScript functions.

use std::marker::PhantomData;

use crate::Error;
use crate::FromJS;
use crate::Lock;
use crate::ToJS;
use crate::Traced;
use crate::Type;
use crate::v8;

/// A persistent, GC-traced handle to a JavaScript function with a typed Rust
/// call signature — the Rust counterpart of unwrapping a C++
/// `jsg::Function<Ret(Args...)>` from a JS function value.
///
/// `Args` is a tuple of [`ToJS`] argument types (use `()` for none, `(T,)` for
/// one); `R` is the [`FromJS`] return type (`()` discards the result).
///
/// Obtained via [`FromJS`], e.g. as a `#[jsg_method]` parameter, a
/// `#[jsg_struct]` field, or a `#[jsg_resource]` field. The handle is
/// persistent — safe to store in a resource and call later — but must be
/// traced for GC; resource fields get this automatically via the [`Traced`]
/// impl. Like all V8 handles it is bound to its isolate's thread
/// (`!Send + !Sync`).
///
/// # Example
///
/// ```ignore
/// fn each(&self, lock: &mut Lock, callback: Function<(Number,), Number>) -> Result<(), Error> {
///     let doubled = callback.call(lock, (Number::new(21.0),))?;
///     ...
/// }
/// ```
pub struct Function<Args = (), R = ()> {
    handle: v8::Global<v8::Function>,
    /// `fn`-pointer phantom: no drop obligations and no `Send`/`Sync`
    /// inherited from `Args`/`R` (thread affinity is already enforced by the
    /// raw-pointer phantom inside [`v8::Global`]).
    _signature: PhantomData<fn(Args) -> R>,
}

impl<Args, R> Function<Args, R> {
    /// Returns the underlying function handle in the current `HandleScope`.
    pub fn as_local<'a>(&self, lock: &mut Lock) -> v8::Local<'a, v8::Function> {
        self.handle.as_local(lock)
    }

    /// Creates an independent persistent handle to the same JS function.
    ///
    /// Not the std `Clone` trait because cloning a V8 persistent handle
    /// requires the isolate (see [`v8::Global::clone`]).
    #[must_use]
    pub fn clone(&self, lock: &mut Lock) -> Self {
        Self {
            handle: self.handle.clone(lock),
            _signature: PhantomData,
        }
    }
}

impl<Args: FunctionArgs, R: FromJS> Function<Args, R> {
    /// Calls the function with `undefined` as the receiver, converting
    /// arguments via [`ToJS`] and the result via [`FromJS`].
    ///
    /// If the callee throws, the exception is returned as an [`Error`]
    /// preserving the JS error type and message. Isolate termination during the
    /// call is returned as an [`Error`] with [`Error::is_termination`] set —
    /// stop calling into JS when you see it (see the accessor's docs).
    pub fn call(&self, lock: &mut Lock, args: Args) -> Result<R::ResultType, Error> {
        self.call_with_receiver(lock, None::<v8::Local<'_, v8::Value>>, args)
    }

    /// Like [`Function::call`] with an explicit `this` receiver.
    pub fn call_with_receiver<'a, Recv: Into<v8::Local<'a, v8::Value>>>(
        &self,
        lock: &mut Lock,
        receiver: Option<Recv>,
        args: Args,
    ) -> Result<R::ResultType, Error> {
        let func = self.handle.as_local(lock);
        let arg_locals = args.to_js_args(lock);
        func.call::<R, _>(lock, receiver, &arg_locals)
    }
}

impl<Args, R> std::fmt::Debug for Function<Args, R> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("Function")
    }
}

impl<Args, R> Type for Function<Args, R> {
    fn class_name() -> &'static str {
        "function"
    }

    fn is_exact(value: &v8::Local<v8::Value>) -> bool {
        value.is_function()
    }
}

impl<Args, R> FromJS for Function<Args, R> {
    type ResultType = Self;

    fn from_js(_lock: &mut Lock, value: v8::Local<v8::Value>) -> Result<Self, Error> {
        value.clone().try_as::<v8::Function>().map_or_else(
            || {
                Err(Error::new_type_error(format!(
                    "expected function, got {}",
                    value.type_of()
                )))
            },
            |func| {
                Ok(Self {
                    handle: func.into(),
                    _signature: PhantomData,
                })
            },
        )
    }
}

impl<Args, R> ToJS for Function<Args, R> {
    fn to_js<'a, 'b>(self, lock: &'a mut Lock) -> v8::Local<'b, v8::Value>
    where
        'b: 'a,
    {
        self.handle.as_local(lock).into()
    }
}

/// By-ref conversion, used by `#[jsg_struct]` field wrapping.
impl<Args, R> v8::ToLocalValue for Function<Args, R> {
    fn to_local<'a>(&self, lock: &mut Lock) -> v8::Local<'a, v8::Value> {
        self.handle.as_local(lock).into()
    }
}

/// Delegates to the inner [`v8::Global`] — a strong handle that participates
/// in cycle collection once the owning resource is only reachable from JS.
impl<Args, R> Traced for Function<Args, R> {
    fn trace(&self, visitor: &mut v8::GcVisitor) {
        self.handle.trace(visitor);
    }
}

/// Argument tuples accepted by [`Function::call`].
///
/// Implemented for tuples of [`ToJS`] types up to 8 elements.
pub trait FunctionArgs {
    /// Converts each element to a JS value in the current `HandleScope`.
    fn to_js_args<'a>(self, lock: &mut Lock) -> Vec<v8::Local<'a, v8::Value>>;
}

impl FunctionArgs for () {
    fn to_js_args<'a>(self, _lock: &mut Lock) -> Vec<v8::Local<'a, v8::Value>> {
        Vec::new()
    }
}

macro_rules! impl_function_args {
    ($($arg:ident),+) => {
        impl<$($arg: ToJS),+> FunctionArgs for ($($arg,)+) {
            #[expect(non_snake_case)]
            fn to_js_args<'a>(self, lock: &mut Lock) -> Vec<v8::Local<'a, v8::Value>> {
                let ($($arg,)+) = self;
                vec![$($arg.to_js(lock)),+]
            }
        }
    };
}

impl_function_args!(A1);
impl_function_args!(A1, A2);
impl_function_args!(A1, A2, A3);
impl_function_args!(A1, A2, A3, A4);
impl_function_args!(A1, A2, A3, A4, A5);
impl_function_args!(A1, A2, A3, A4, A5, A6);
impl_function_args!(A1, A2, A3, A4, A5, A6, A7);
impl_function_args!(A1, A2, A3, A4, A5, A6, A7, A8);
