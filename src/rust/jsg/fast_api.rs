// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! V8 Fast API callbacks for `#[jsg_method]`. See the README.
//!
//! A method gets a fast callback when its arguments are [`FastApiArg`]s and it returns a
//! [`FastApiOutput`]; see [`probe`]. New C types also need a case in `cTypeOf()` in `ffi.c++`.

#![allow(
    unsafe_code,
    reason = "fast callbacks receive raw V8 pointers and resolve resources through them"
)]

use std::marker::PhantomData;
use std::panic::AssertUnwindSafe;
#[cfg(debug_assertions)]
use std::sync::atomic::AtomicBool;
#[cfg(debug_assertions)]
use std::sync::atomic::AtomicU64;
#[cfg(debug_assertions)]
use std::sync::atomic::Ordering;

use crate::Error;
use crate::Lock;
use crate::Number;
use crate::Resource;
use crate::UnitType;
use crate::scope::unit_value;
use crate::v8::ffi;
pub use crate::v8::ffi::FastApiType;
use crate::wrappable::FromNumber;

mod private {
    pub trait Sealed {}
}

/// An argument type of a fast callback. V8 passes it as the C type `Raw`, and `from_raw` converts
/// it exactly as the slow callback's `FromJS` does.
#[diagnostic::on_unimplemented(
    message = "`{Self}` cannot be an argument of a V8 Fast API callback",
    note = "the argument types a fast callback can take are listed in `jsg/fast_api.rs`"
)]
pub trait FastApiArg: private::Sealed + Sized {
    type Raw: Copy;
    const TYPE: FastApiType;
    fn from_raw(raw: Self::Raw) -> Result<Self, Error>;
}

/// A return type of a fast callback, handed back to V8 as the C type `Raw`.
#[diagnostic::on_unimplemented(
    message = "`{Self}` cannot be returned from a V8 Fast API callback",
    note = "the types a fast callback can return are listed in `jsg/fast_api.rs`"
)]
pub trait FastApiReturn: private::Sealed {
    type Raw: Default;
    const TYPE: FastApiType;
    fn into_raw(self) -> Self::Raw;
}

// Arguments

impl private::Sealed for bool {}
impl FastApiArg for bool {
    type Raw = Self;
    const TYPE: FastApiType = FastApiType::Bool;
    fn from_raw(raw: Self) -> Result<Self, Error> {
        Ok(raw)
    }
}

impl private::Sealed for Number {}
impl FastApiArg for Number {
    type Raw = f64;
    const TYPE: FastApiType = FastApiType::Float64;
    fn from_raw(raw: f64) -> Result<Self, Error> {
        Ok(Self::new(raw))
    }
}

// Integers come from V8 as doubles, so that their range checks and truncation are the slow path's.

impl private::Sealed for i32 {}
impl FastApiArg for i32 {
    type Raw = f64;
    const TYPE: FastApiType = FastApiType::Float64;
    fn from_raw(raw: f64) -> Result<Self, Error> {
        Self::from_number(raw)
    }
}

impl private::Sealed for u32 {}
impl FastApiArg for u32 {
    type Raw = f64;
    const TYPE: FastApiType = FastApiType::Float64;
    fn from_raw(raw: f64) -> Result<Self, Error> {
        Self::from_number(raw)
    }
}

// Return values

impl FastApiReturn for bool {
    type Raw = Self;
    const TYPE: FastApiType = FastApiType::Bool;
    fn into_raw(self) -> Self {
        self
    }
}

impl FastApiReturn for Number {
    type Raw = f64;
    const TYPE: FastApiType = FastApiType::Float64;
    fn into_raw(self) -> f64 {
        self.value()
    }
}

impl private::Sealed for () {}
impl FastApiReturn for () {
    type Raw = Self;
    const TYPE: FastApiType = FastApiType::Void;
    fn into_raw(self) {}
}

/// A [`FastApiReturn`], or a `Result` of one whose `Err` is thrown.
#[diagnostic::on_unimplemented(
    message = "`{Self}` cannot be returned from a V8 Fast API callback",
    note = "a fast callback returns a type listed in `jsg/fast_api.rs`, or a `Result` of one"
)]
pub trait FastApiOutput: private::Sealed {
    type Value: FastApiReturn;
    fn into_result(self) -> Result<Self::Value, Error>;
}

impl<T: FastApiReturn> FastApiOutput for T {
    type Value = Self;
    fn into_result(self) -> Result<Self, Error> {
        Ok(self)
    }
}

impl<T, E> private::Sealed for Result<T, E> {}
impl<T: FastApiReturn, E: Into<Error>> FastApiOutput for Result<T, E> {
    type Value = T;
    fn into_result(self) -> Result<T, Error> {
        self.map_err(Into::into)
    }
}

/// Picks a method's fast callback, or none, by autoref specialization: [`Eligible`] on the probe
/// wins over [`Ineligible`] on `&probe`.
#[doc(hidden)]
pub mod probe {
    use std::marker::PhantomData;

    use super::FastMethod;
    use crate::UnitType;
    use crate::v8::ffi;

    /// A closure `F` that calls a method of `R` (or [`Static`](super::Static)) with `Args`.
    pub struct Probe<R, Args, F>(PhantomData<fn(&R, Args) -> F>);

    impl<R, Args, F: UnitType> Probe<R, Args, F> {
        pub const fn new(f: F) -> Self {
            let _ = f;
            Self(PhantomData)
        }
    }

    pub trait Eligible {
        fn descriptor(&self) -> Option<ffi::FastApiDescriptor>;
    }

    impl<R, Args, F: FastMethod<R, Args>> Eligible for Probe<R, Args, F> {
        fn descriptor(&self) -> Option<ffi::FastApiDescriptor> {
            Some(F::descriptor())
        }
    }

    pub trait Ineligible {
        fn descriptor(&self) -> Option<ffi::FastApiDescriptor> {
            None
        }
    }

    impl<R, Args, F> Ineligible for &Probe<R, Args, F> {}
}

/// The fast callback for `#[jsg_method(fast)]`; fails to compile if there is none.
#[doc(hidden)]
pub fn method<R, Args, F: FastMethod<R, Args>>(f: F) -> ffi::FastApiDescriptor {
    let _ = f;
    F::descriptor()
}

/// The arguments of a method that takes `&mut Lock` before its JavaScript arguments `Args`.
pub struct WithLock<Args>(PhantomData<Args>);

/// The receiver of a static method.
pub struct Static;

/// A captureless closure calling a method of `R`, or a static one if `R` is [`Static`], with
/// `Args` (a tuple, or [`WithLock`] of one).
pub trait FastMethod<R, Args>: UnitType {
    fn descriptor() -> ffi::FastApiDescriptor;
}

// `FastMethod` for each arity, instance and static, with and without `&mut Lock`.
macro_rules! impl_fast_methods {
    ($($arg:ident: $ty:ident),*) => {
        impl<R, O, F, $($ty),*> FastMethod<R, ($($ty,)*)> for F
        where
            R: Resource,
            O: FastApiOutput,
            F: UnitType + Fn(&R, $($ty),*) -> O,
            $($ty: FastApiArg,)*
        {
            fn descriptor() -> ffi::FastApiDescriptor {
                unsafe extern "C" fn callback<R, O, F, $($ty),*>(
                    receiver: ffi::Local,
                    $($arg: $ty::Raw,)*
                    options: *mut ffi::FastApiCallbackOptions,
                ) -> <O::Value as FastApiReturn>::Raw
                where
                    R: Resource,
                    O: FastApiOutput,
                    F: UnitType + Fn(&R, $($ty),*) -> O,
                    $($ty: FastApiArg,)*
                {
                    record_call();
                    // SAFETY: V8 passes the call's receiver and options.
                    unsafe {
                        with_receiver(options, receiver, |this: &R| {
                            run(options, || {
                                // SAFETY: `method()` was handed a value of `F`.
                                let f = unit_value::<F>();
                                f(this, $($ty::from_raw($arg)?),*).into_result()
                            })
                        })
                    }
                }
                descriptor::<O::Value>(callback::<R, O, F, $($ty),*> as *const () as usize, vec![$($ty::TYPE),*])
            }
        }

        impl<R, O, F, $($ty),*> FastMethod<R, WithLock<($($ty,)*)>> for F
        where
            R: Resource,
            O: FastApiOutput,
            F: UnitType + for<'s> Fn(&R, &mut Lock<'s>, $($ty),*) -> O,
            $($ty: FastApiArg,)*
        {
            fn descriptor() -> ffi::FastApiDescriptor {
                unsafe extern "C" fn callback<R, O, F, $($ty),*>(
                    receiver: ffi::Local,
                    $($arg: $ty::Raw,)*
                    options: *mut ffi::FastApiCallbackOptions,
                ) -> <O::Value as FastApiReturn>::Raw
                where
                    R: Resource,
                    O: FastApiOutput,
                    F: UnitType + for<'s> Fn(&R, &mut Lock<'s>, $($ty),*) -> O,
                    $($ty: FastApiArg,)*
                {
                    record_call();
                    // SAFETY: V8 passes the call's receiver and options.
                    unsafe {
                        with_receiver(options, receiver, |this: &R| {
                            with_lock(options, |lock| {
                                // SAFETY: `method()` was handed a value of `F`.
                                let f = unit_value::<F>();
                                f(this, lock, $($ty::from_raw($arg)?),*).into_result()
                            })
                        })
                    }
                }
                descriptor::<O::Value>(callback::<R, O, F, $($ty),*> as *const () as usize, vec![$($ty::TYPE),*])
            }
        }

        impl<O, F, $($ty),*> FastMethod<Static, ($($ty,)*)> for F
        where
            O: FastApiOutput,
            F: UnitType + Fn($($ty),*) -> O,
            $($ty: FastApiArg,)*
        {
            fn descriptor() -> ffi::FastApiDescriptor {
                unsafe extern "C" fn callback<O, F, $($ty),*>(
                    _receiver: ffi::Local,
                    $($arg: $ty::Raw,)*
                    options: *mut ffi::FastApiCallbackOptions,
                ) -> <O::Value as FastApiReturn>::Raw
                where
                    O: FastApiOutput,
                    F: UnitType + Fn($($ty),*) -> O,
                    $($ty: FastApiArg,)*
                {
                    record_call();
                    // SAFETY: V8 passes the call's options.
                    unsafe {
                        run(options, || {
                            // SAFETY: `method()` was handed a value of `F`.
                            let f = unit_value::<F>();
                            f($($ty::from_raw($arg)?),*).into_result()
                        })
                    }
                }
                descriptor::<O::Value>(callback::<O, F, $($ty),*> as *const () as usize, vec![$($ty::TYPE),*])
            }
        }

        impl<O, F, $($ty),*> FastMethod<Static, WithLock<($($ty,)*)>> for F
        where
            O: FastApiOutput,
            F: UnitType + for<'s> Fn(&mut Lock<'s>, $($ty),*) -> O,
            $($ty: FastApiArg,)*
        {
            fn descriptor() -> ffi::FastApiDescriptor {
                unsafe extern "C" fn callback<O, F, $($ty),*>(
                    _receiver: ffi::Local,
                    $($arg: $ty::Raw,)*
                    options: *mut ffi::FastApiCallbackOptions,
                ) -> <O::Value as FastApiReturn>::Raw
                where
                    O: FastApiOutput,
                    F: UnitType + for<'s> Fn(&mut Lock<'s>, $($ty),*) -> O,
                    $($ty: FastApiArg,)*
                {
                    record_call();
                    // SAFETY: V8 passes the call's options.
                    unsafe {
                        with_lock(options, |lock| {
                            // SAFETY: `method()` was handed a value of `F`.
                            let f = unit_value::<F>();
                            f(lock, $($ty::from_raw($arg)?),*).into_result()
                        })
                    }
                }
                descriptor::<O::Value>(callback::<O, F, $($ty),*> as *const () as usize, vec![$($ty::TYPE),*])
            }
        }
    };
}

impl_fast_methods!();
impl_fast_methods!(a0: A0);
impl_fast_methods!(a0: A0, a1: A1);
impl_fast_methods!(a0: A0, a1: A1, a2: A2);
impl_fast_methods!(a0: A0, a1: A1, a2: A2, a3: A3);
impl_fast_methods!(a0: A0, a1: A1, a2: A2, a3: A3, a4: A4);
impl_fast_methods!(a0: A0, a1: A1, a2: A2, a3: A3, a4: A4, a5: A5);
impl_fast_methods!(a0: A0, a1: A1, a2: A2, a3: A3, a4: A4, a5: A5, a6: A6);
impl_fast_methods!(a0: A0, a1: A1, a2: A2, a3: A3, a4: A4, a5: A5, a6: A6, a7: A7);

fn descriptor<T: FastApiReturn>(
    callback: usize,
    arg_types: Vec<FastApiType>,
) -> ffi::FastApiDescriptor {
    ffi::FastApiDescriptor {
        callback,
        return_type: T::TYPE,
        arg_types,
    }
}

/// Calls `f` with the resource `receiver` wraps, or throws if it wraps none of type `R`.
unsafe fn with_receiver<R: Resource, T: Default>(
    options: *mut ffi::FastApiCallbackOptions,
    receiver: ffi::Local,
    f: impl FnOnce(&R) -> T,
) -> T {
    // SAFETY: `options` and `receiver` come from V8 (caller contract).
    let trait_ptr = unsafe { ffi::fast_api_unwrap_receiver(options, receiver) };
    let Some(resource) = trait_ptr.resolve::<R>() else {
        // SAFETY: as above.
        return unsafe { throw(options, &Error::new_type_error("Illegal invocation")) };
    };
    // SAFETY: The receiver keeps the resource alive for the call. A fast callback cannot run
    // JavaScript, so nothing else can reach the resource while this borrow lives.
    f(unsafe { resource.as_ref() })
}

/// Runs `f` with a lock in a `HandleScope` where JavaScript can't run.
unsafe fn with_lock<T, F>(options: *mut ffi::FastApiCallbackOptions, f: F) -> T::Raw
where
    T: FastApiReturn,
    F: for<'s> FnOnce(&mut Lock<'s>) -> Result<T, Error>,
{
    // SAFETY: `options` comes from V8 (caller contract).
    unsafe {
        crate::scope::in_fast_call_scope(options, |lock| match f(&mut *lock) {
            Ok(value) => value.into_raw(),
            Err(err) => {
                lock.throw_exception(&err);
                T::Raw::default()
            }
        })
    }
    .unwrap_or_default()
}

/// Mirrors [`crate::catch_panic`]: a panic becomes an internal error and terminates execution.
unsafe fn run<T: FastApiReturn>(
    options: *mut ffi::FastApiCallbackOptions,
    f: impl FnOnce() -> Result<T, Error>,
) -> T::Raw {
    // AssertUnwindSafe: nothing observes the closure's state after a panic.
    match std::panic::catch_unwind(AssertUnwindSafe(f)) {
        Ok(Ok(value)) => value.into_raw(),
        // SAFETY: `options` comes from V8 (caller contract).
        Ok(Err(err)) => unsafe { throw(options, &err) },
        Err(payload) => {
            // SAFETY: as above.
            unsafe {
                ffi::fast_api_throw_internal_error(options, crate::panic_message(&*payload));
                ffi::fast_api_terminate_execution(options);
            }
            T::Raw::default()
        }
    }
}

/// Throws `err` as [`crate::Lock::throw_exception`] does, and returns the value V8 ignores.
unsafe fn throw<T: Default>(options: *mut ffi::FastApiCallbackOptions, err: &Error) -> T {
    // SAFETY: `options` comes from V8 (caller contract).
    unsafe {
        if err.is_internal {
            ffi::fast_api_throw_internal_error(options, &err.message);
        } else {
            ffi::fast_api_throw_exception(options, err.name, &err.message);
        }
    }
    T::default()
}

#[cfg(debug_assertions)]
static COUNT_CALLS: AtomicBool = AtomicBool::new(false);
#[cfg(debug_assertions)]
static CALL_COUNT: AtomicU64 = AtomicU64::new(0);

#[inline]
fn record_call() {
    #[cfg(debug_assertions)]
    if COUNT_CALLS.load(Ordering::Relaxed) {
        CALL_COUNT.fetch_add(1, Ordering::Relaxed);
    }
}

/// Fast callbacks run since the first call to this function, on any thread. For tests, which must
/// not run in parallel to rely on it. Only builds with debug assertions count calls.
#[cfg(debug_assertions)]
#[doc(hidden)]
pub fn call_count() -> u64 {
    COUNT_CALLS.store(true, Ordering::Relaxed);
    CALL_COUNT.load(Ordering::Relaxed)
}
