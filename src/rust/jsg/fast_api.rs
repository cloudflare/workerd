// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! V8 Fast API callbacks for `#[jsg_method]`. See the README.
//!
//! To support another type, implement [`FastApiArg`] or [`FastApiReturn`] for it below and add
//! its name to the lists in `jsg-macros/utils.rs`. If V8 must pass it as a C type not in
//! [`FastApiType`] yet, add a variant and map it to a `v8::CTypeInfo` in `cTypeOf()` in `ffi.c++`.

#![allow(
    unsafe_code,
    reason = "fast callbacks receive raw V8 pointers and resolve resources through them"
)]

use std::panic::AssertUnwindSafe;
#[cfg(debug_assertions)]
use std::sync::atomic::AtomicBool;
#[cfg(debug_assertions)]
use std::sync::atomic::AtomicU64;
#[cfg(debug_assertions)]
use std::sync::atomic::Ordering;

use crate::Error;
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
pub trait FastApiArg: private::Sealed + Sized {
    type Raw: Copy;
    const TYPE: FastApiType;
    fn from_raw(raw: Self::Raw) -> Result<Self, Error>;
}

/// A return type of a fast callback, handed back to V8 as the C type `Raw`.
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

/// Returns the fast callback of a method of `R`, which `#[jsg_method]` passes as `f`.
#[doc(hidden)]
pub fn method<R, Args, F: FastMethod<R, Args>>(f: F) -> ffi::FastApiDescriptor {
    let _ = f;
    F::descriptor()
}

/// Returns the fast callback of a static method, which `#[jsg_method]` passes as `f`.
#[doc(hidden)]
pub fn static_method<Args, F: FastStaticMethod<Args>>(f: F) -> ffi::FastApiDescriptor {
    let _ = f;
    F::descriptor()
}

/// A closure that captures nothing and calls a method of `R` with the arguments `Args`.
pub trait FastMethod<R, Args>: UnitType {
    fn descriptor() -> ffi::FastApiDescriptor;
}

/// A closure that captures nothing and calls a static method with the arguments `Args`.
pub trait FastStaticMethod<Args>: UnitType {
    fn descriptor() -> ffi::FastApiDescriptor;
}

// Implements `FastMethod` and `FastStaticMethod` for closures taking these arguments. V8 calls
// a fast callback with the receiver, each argument as its C type, then the callback options.
macro_rules! impl_fast_methods {
    ($($arg:ident: $ty:ident),*) => {
        impl<R, T, F, $($ty),*> FastMethod<R, ($($ty,)*)> for F
        where
            R: Resource,
            T: FastApiReturn,
            F: UnitType + Fn(&R, $($ty),*) -> T,
            $($ty: FastApiArg,)*
        {
            fn descriptor() -> ffi::FastApiDescriptor {
                unsafe extern "C" fn callback<R, T, F, $($ty),*>(
                    receiver: ffi::Local,
                    $($arg: $ty::Raw,)*
                    options: *mut ffi::FastApiCallbackOptions,
                ) -> T::Raw
                where
                    R: Resource,
                    T: FastApiReturn,
                    F: UnitType + Fn(&R, $($ty),*) -> T,
                    $($ty: FastApiArg,)*
                {
                    // SAFETY: V8 passes the call's receiver and options.
                    unsafe {
                        call_method(options, receiver, |this: &R| {
                            // SAFETY: `method()` was handed a value of `F`.
                            let f = unit_value::<F>();
                            Ok(f(this, $($ty::from_raw($arg)?),*))
                        })
                    }
                }
                ffi::FastApiDescriptor {
                    callback: callback::<R, T, F, $($ty),*> as *const () as usize,
                    return_type: T::TYPE,
                    arg_types: vec![$($ty::TYPE),*],
                }
            }
        }

        impl<T, F, $($ty),*> FastStaticMethod<($($ty,)*)> for F
        where
            T: FastApiReturn,
            F: UnitType + Fn($($ty),*) -> T,
            $($ty: FastApiArg,)*
        {
            fn descriptor() -> ffi::FastApiDescriptor {
                unsafe extern "C" fn callback<T, F, $($ty),*>(
                    _receiver: ffi::Local,
                    $($arg: $ty::Raw,)*
                    options: *mut ffi::FastApiCallbackOptions,
                ) -> T::Raw
                where
                    T: FastApiReturn,
                    F: UnitType + Fn($($ty),*) -> T,
                    $($ty: FastApiArg,)*
                {
                    record_call();
                    // SAFETY: V8 passes the call's options.
                    unsafe {
                        run(options, || {
                            // SAFETY: `static_method()` was handed a value of `F`.
                            let f = unit_value::<F>();
                            Ok(f($($ty::from_raw($arg)?),*))
                        })
                    }
                }
                ffi::FastApiDescriptor {
                    callback: callback::<T, F, $($ty),*> as *const () as usize,
                    return_type: T::TYPE,
                    arg_types: vec![$($ty::TYPE),*],
                }
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

/// Calls `f` with the resource `receiver` wraps, or throws if it wraps none of type `R`.
unsafe fn call_method<R: Resource, T: FastApiReturn>(
    options: *mut ffi::FastApiCallbackOptions,
    receiver: ffi::Local,
    f: impl FnOnce(&R) -> Result<T, Error>,
) -> T::Raw {
    record_call();
    // SAFETY: `options` and `receiver` come from V8 (caller contract).
    let trait_ptr = unsafe { ffi::fast_api_unwrap_receiver(options, receiver) };
    let Some(resource) = trait_ptr.resolve::<R>() else {
        // SAFETY: as above.
        return unsafe { throw(options, &Error::new_type_error("Illegal invocation")) };
    };
    // SAFETY: The receiver keeps the resource alive for the call. A fast callback cannot run
    // JavaScript, so nothing else can reach the resource while this borrow lives.
    let resource = unsafe { resource.as_ref() };
    // SAFETY: as above.
    unsafe { run(options, || f(resource)) }
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
