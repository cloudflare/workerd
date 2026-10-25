// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#![allow(unsafe_code, reason = "converts values through the V8 FFI")]
#![allow(
    clippy::allow_attributes,
    reason = "`impl_integer_from_js!` uses `allow`, since its cast lints fire for only some integer types"
)]

//! Traits for converting between Rust and JavaScript values.
//!
//! # Supported Types
//!
//! ## Primitive Types
//!
//! | Rust Type | JavaScript Type |
//! |-----------|-----------------|
//! | `()` | `undefined` |
//! | `String` | `string` |
//! | `&str` | `string` |
//! | `bool` | `boolean` |
//! | `Number` | `number` |
//! | `i64` | `bigint` or `number` (parameter only) |
//! | `SystemTime` | `Date` or `number` of milliseconds (parameter only) |
//! | `Option<T>` | `T` or `undefined` |
//! | `Nullable<T>` | `T`, `null`, or `undefined` |
//! | `Lenient<T>` | `T`, `null`, or `undefined` (shallow type mismatches silently become `undefined`) |
//! | `Result<T, E>` | `T` or throws |
//! | `NonCoercible<T>` | `T` (strict type checking) |
//! | `T: Struct` | `object` |
//! | `Vec<T>` | `Array<T>` |
//! | `&[T]` | `Array<T>` (parameter only) |
//!
//! ## `TypedArray` Types
//!
//! These specialized `Vec` and slice types map directly to JavaScript `TypedArray`s
//! for efficient binary data transfer:
//!
//! | Rust Type | JavaScript Type |
//! |-----------|-----------------|
//! | `Vec<u8>` | `Uint8Array` |
//! | `Vec<u16>` | `Uint16Array` |
//! | `Vec<u32>` | `Uint32Array` |
//! | `Vec<i8>` | `Int8Array` |
//! | `Vec<i16>` | `Int16Array` |
//! | `Vec<i32>` | `Int32Array` |
//! | `Vec<f32>` | `Float32Array` |
//! | `Vec<f64>` | `Float64Array` |
//! | `Vec<i64>` | `BigInt64Array` |
//! | `Vec<u64>` | `BigUint64Array` |
//! | `&[u8]` | `Uint8Array` (parameter only) |
//! | `&[u16]` | `Uint16Array` (parameter only) |
//! | `&[u32]` | `Uint32Array` (parameter only) |
//! | `&[i8]` | `Int8Array` (parameter only) |
//! | `&[i16]` | `Int16Array` (parameter only) |
//! | `&[i32]` | `Int32Array` (parameter only) |
//! | `&[f32]` | `Float32Array` (parameter only) |
//! | `&[f64]` | `Float64Array` (parameter only) |
//! | `&[i64]` | `BigInt64Array` (parameter only) |
//! | `&[u64]` | `BigUint64Array` (parameter only) |
//!
//! ## Integer Parameter Types
//!
//! Integer types (`u8`, `u16`, `u32`, `i8`, `i16`, `i32`) can be used as method
//! parameters. They convert like C++ JSG's integers:
//!
//! - Values out of the type's range, checked before truncation, are a
//!   `TypeError` (e.g., `256` or `255.5` for `u8`)
//! - Non-finite values are a `TypeError`, except that `i32` reads them as `0`
//! - Other values are truncated toward zero (e.g., `3.7` → `3`, `-2.9` → `-2`)
//!
//! For strict validation, wrap parameters in `NonCoercible<T>` or validate manually.
//!
//! `i64` converts like C++ JSG's `int64_t`: a `BigInt` that does not fit is a
//! `TypeError`, and any other value converts as above.

use crate::Error;
use crate::Lenient;
use crate::Lock;
use crate::NonCoercible;
use crate::Nullable;
use crate::Number;
use crate::Type;
use crate::v8;

// =============================================================================
// Traced trait — GC tracing for field types
// =============================================================================

/// Trait for types that can be visited during V8 garbage collection tracing.
///
/// Every type that can appear as a field in a `#[jsg_resource]` struct must
/// implement `Traced`. The generated `Traced::trace` body calls
/// `Traced::trace` on every field — types with no GC-visible references
/// use a no-op implementation, such as the one `jsg_traced!` generates.
///
/// # Built-in implementations
///
/// - **Primitives** (`bool`, `String`, integers, floats, `()`) — no-op.
/// - **`Option<T>`**, **`Nullable<T>`** — delegates to the inner value if present.
/// - **Collections** (`Vec<T>`, `HashMap<K,V>`, `BTreeMap<K,V>`, `HashSet<T>`,
///   `BTreeSet<T>`) — iterates elements/values and traces each.
/// - **`Cell<T>`** — reads through `as_ptr()` (sound under single-threaded GC).
/// - **`RefCell<T>`** — traces through `try_borrow()`, so a mutably borrowed cell is skipped;
///   debug builds abort if it can hold GC references.
/// - **`jsg::Rc<T>`** — visits the reference via `GcVisitor::visit_rc`.
/// - **`jsg::Weak<T>`** — no-op (weak refs don't keep targets alive).
/// - **`jsg::v8::TracedGlobal<T>`** — visits via `GcVisitor::visit_traced_global`.
pub trait Traced {
    /// Whether a value of this type can hold GC-visible references. Tracing a `RefCell` checks
    /// it. It defaults to `true`, and wrappers and collections take it from what they hold. A
    /// type whose `trace` is a no-op should return `false`, as `jsg_traced!` does.
    fn holds_gc_references() -> bool
    where
        Self: Sized,
    {
        true
    }

    /// Visit GC-visible references held by this value.
    ///
    /// The default implementation is a no-op, suitable for types with no
    /// GC-visible references (primitives, plain data structs, etc.). Such a type
    /// should also return `false` from `holds_gc_references`; `jsg_traced!`
    /// generates both.
    ///
    /// A hand-written implementation must visit every field that can hold a GC reference
    /// (`TracedGlobal`, `jsg::Rc`, and types holding them). Nothing checks this. The type must
    /// not hold a [`v8::Global`](crate::v8::Global), which is always strong, so a cycle through
    /// it is never collected. Don't keep a type that holds GC references behind an
    /// `std::rc::Rc`, since a clone held by another GC owner would trace it too; see
    /// `README.md` §Safety rules, rule 2.
    fn trace(&self, _visitor: &mut crate::v8::GcVisitor) {}
}

/// Generates no-op `Traced` implementations for types with no GC-visible references.
macro_rules! impl_traced_noop {
    ($($t:ty),* $(,)?) => {
        $(impl Traced for $t {
            fn holds_gc_references() -> bool {
                false
            }
        })*
    };
}

impl_traced_noop!(
    (),
    bool,
    String,
    crate::Error,
    u8,
    u16,
    u32,
    u64,
    usize,
    i8,
    i16,
    i32,
    i64,
    isize,
    f32,
    f64,
    crate::v8::ArrayBuffer,
    crate::v8::ArrayBufferView,
    crate::v8::BackingStore,
    crate::v8::BigInt64Array,
    crate::v8::BigUint64Array,
    crate::v8::Float32Array,
    crate::v8::Float64Array,
    crate::v8::Int8Array,
    crate::v8::Int16Array,
    crate::v8::Int32Array,
    crate::v8::Uint8Array,
    crate::v8::Uint16Array,
    crate::v8::Uint32Array,
    &str,
    Number,
);

impl<T: Traced> Traced for Option<T> {
    fn holds_gc_references() -> bool {
        T::holds_gc_references()
    }

    fn trace(&self, visitor: &mut crate::v8::GcVisitor) {
        if let Some(inner) = self {
            inner.trace(visitor);
        }
    }
}

impl<T: Traced> Traced for Lenient<T> {
    // An unconvertable value is a `TracedGlobal`, whatever `T` is.
    fn holds_gc_references() -> bool {
        true
    }

    fn trace(&self, visitor: &mut crate::v8::GcVisitor) {
        if let Self::Some(inner) = self {
            inner.trace(visitor);
        }
        if let Self::Unconvertable(inner) = self {
            inner.trace(visitor);
        }
    }
}

impl<T: Traced> Traced for Nullable<T> {
    fn holds_gc_references() -> bool {
        T::holds_gc_references()
    }

    fn trace(&self, visitor: &mut crate::v8::GcVisitor) {
        if let Self::Some(inner) = self {
            inner.trace(visitor);
        }
    }
}

impl<T: Traced> Traced for NonCoercible<T> {
    fn holds_gc_references() -> bool {
        T::holds_gc_references()
    }

    fn trace(&self, visitor: &mut crate::v8::GcVisitor) {
        self.as_ref().trace(visitor);
    }
}

impl<T: Traced> Traced for Vec<T> {
    fn holds_gc_references() -> bool {
        T::holds_gc_references()
    }

    fn trace(&self, visitor: &mut crate::v8::GcVisitor) {
        for item in self {
            item.trace(visitor);
        }
    }
}

impl<K, V: Traced, S: std::hash::BuildHasher> Traced for std::collections::HashMap<K, V, S> {
    fn holds_gc_references() -> bool {
        V::holds_gc_references()
    }

    fn trace(&self, visitor: &mut crate::v8::GcVisitor) {
        for value in self.values() {
            value.trace(visitor);
        }
    }
}

impl<K, V: Traced> Traced for std::collections::BTreeMap<K, V> {
    fn holds_gc_references() -> bool {
        V::holds_gc_references()
    }

    fn trace(&self, visitor: &mut crate::v8::GcVisitor) {
        for value in self.values() {
            value.trace(visitor);
        }
    }
}

impl<T: Traced, S: std::hash::BuildHasher> Traced for std::collections::HashSet<T, S> {
    fn holds_gc_references() -> bool {
        T::holds_gc_references()
    }

    fn trace(&self, visitor: &mut crate::v8::GcVisitor) {
        for item in self {
            item.trace(visitor);
        }
    }
}

impl<T: Traced> Traced for std::collections::BTreeSet<T> {
    fn holds_gc_references() -> bool {
        T::holds_gc_references()
    }

    fn trace(&self, visitor: &mut crate::v8::GcVisitor) {
        for item in self {
            item.trace(visitor);
        }
    }
}

impl<T: Traced> Traced for std::cell::Cell<T> {
    fn holds_gc_references() -> bool {
        T::holds_gc_references()
    }

    fn trace(&self, visitor: &mut crate::v8::GcVisitor) {
        // SAFETY: V8 GC tracing is single-threaded within an isolate and never
        // re-entrant on the same object during a single GC cycle. We only read
        // through the pointer.
        unsafe {
            (*self.as_ptr()).trace(visitor);
        }
    }
}

impl<T: Traced> Traced for std::cell::RefCell<T> {
    fn holds_gc_references() -> bool {
        T::holds_gc_references()
    }

    fn trace(&self, visitor: &mut crate::v8::GcVisitor) {
        // A mutable borrow is active here only if the code holding it calls something that visits
        // the resource: an allocation on the V8 heap, which can start a GC, or wrapping the
        // resource, or creating its first or dropping its last strong `jsg::Rc` while it has no
        // wrapper, which switch its references between strong and traced. The contents cannot be
        // read then, so the visit misses their references. A missed GC visit can cost a weak
        // `TracedGlobal` its value and a `jsg::Rc` the JS wrapper of its resource; a missed switch
        // can leave a reference weak that should be strong, for a later GC to free, or strong that
        // should be weak, which leaks. Debug builds abort when that can happen (a panic cannot
        // unwind out of a visit made from C++); release builds skip the cell.
        //
        // The real fix is a JSG cell type whose mutable borrow takes the lock and makes the
        // references it holds strong until the borrow ends, so that a visit can skip a borrowed
        // cell safely. That also covers a `TracedGlobal` taken out of a `Cell`, which nothing
        // traces.
        let Ok(inner) = self.try_borrow() else {
            debug_assert!(
                !T::holds_gc_references(),
                "tracing found a mutably borrowed RefCell that can hold GC references, which it \
                 cannot visit; do not hold a RefCell borrow_mut() across a call that can allocate \
                 on the V8 heap, wrap the resource, or create or drop a jsg::Rc to it"
            );
            return;
        };
        inner.trace(visitor);
    }
}

impl<T: Traced> Traced for std::rc::Rc<T> {
    fn holds_gc_references() -> bool {
        T::holds_gc_references()
    }

    fn trace(&self, visitor: &mut crate::v8::GcVisitor) {
        (**self).trace(visitor);
    }
}

// =============================================================================
// ToJS trait (Rust → JavaScript)
// =============================================================================

/// Trait for converting Rust values to JavaScript.
///
/// Provides Rust → JavaScript conversion.
pub trait ToJS<'s>: Sized {
    /// Converts this Rust value into a JavaScript value in the scope of `lock`.
    fn to_js(self, lock: &mut Lock<'s>) -> v8::Local<'s, v8::Value>;
}

// =============================================================================
// FromJS trait (JavaScript → Rust)
// =============================================================================

/// Trait for converting JavaScript values to Rust.
///
/// `from_js()` is the normal, required conversion path. `try_from_js()` is an
/// internal shallow-probing path used by [`Lenient`]. Implementations only need
/// to override `try_from_js()` when their top-level type can fail to match.
pub trait FromJS<'s>: Sized {
    type ResultType;

    /// Converts a JavaScript value into this Rust type.
    fn from_js(
        lock: &mut Lock<'s>,
        value: v8::Local<'s, v8::Value>,
    ) -> Result<Self::ResultType, Error>;

    /// The type's name in argument errors, as C++ JSG's `getName()` reports
    /// it. Defaults to the Rust type's name without its path or generics.
    #[doc(hidden)]
    fn type_name() -> String {
        let name = std::any::type_name::<Self>();
        let name = name.split('<').next().unwrap_or(name);
        name.rsplit("::").next().unwrap_or(name).to_owned()
    }

    /// Whether a method may omit this argument, like C++ JSG's optional types.
    #[doc(hidden)]
    fn is_optional() -> bool {
        false
    }

    /// Performs a shallow conversion probe.
    ///
    /// `Ok(None)` indicates a top-level type mismatch. Once the top-level type
    /// matches, nested conversions must use `from_js()` so malformed contents
    /// remain hard errors.
    #[doc(hidden)]
    fn try_from_js(
        lock: &mut Lock<'s>,
        value: v8::Local<'s, v8::Value>,
    ) -> Result<Option<Self::ResultType>, Error> {
        Self::from_js(lock, value).map(Some)
    }

    /// Tries to convert only if the JavaScript type matches exactly.
    /// Returns `None` if the type doesn't match, `Some(result)` if conversion was attempted.
    /// Used by `#[jsg_oneof]` macro to try each variant without coercion.
    fn try_from_js_exact(
        lock: &mut Lock<'s>,
        value: v8::Local<'s, v8::Value>,
    ) -> Option<Result<Self::ResultType, Error>>
    where
        Self: Type,
    {
        if !Self::is_exact(value) {
            return None;
        }

        match Self::try_from_js(lock, value) {
            Ok(Some(result)) => Some(Ok(result)),
            Ok(None) => None,
            Err(err) => Some(Err(err)),
        }
    }
}

// =============================================================================
// Primitive type implementations
// =============================================================================

// Boolean implementation for JavaScript booleans.
impl Type for bool {
    fn class_name() -> &'static str {
        "boolean"
    }

    fn is_exact(value: v8::Local<'_, v8::Value>) -> bool {
        value.is_boolean()
    }
}

impl<'s> ToJS<'s> for bool {
    fn to_js(self, lock: &mut Lock<'s>) -> v8::Local<'s, v8::Value> {
        let isolate = lock.isolate();
        // SAFETY: `lock` proves the isolate is locked and a HandleScope for `'s` is open.
        unsafe { v8::Local::from_ffi(v8::ffi::local_new_boolean(isolate.as_ffi(), self)) }
    }
}

impl<'s> FromJS<'s> for bool {
    type ResultType = Self;

    fn type_name() -> String {
        "boolean".to_owned()
    }

    fn from_js(
        lock: &mut Lock<'s>,
        value: v8::Local<'s, v8::Value>,
    ) -> Result<Self::ResultType, Error> {
        // SAFETY: The isolate is locked and value is a valid V8 local handle.
        Ok(unsafe { v8::ffi::unwrap_boolean(lock.isolate().as_ffi(), value.into_ffi()) })
    }
}

impl Type for std::string::String {
    fn class_name() -> &'static str {
        "string"
    }

    fn is_exact(value: v8::Local<'_, v8::Value>) -> bool {
        value.is_string()
    }
}

impl<'s> ToJS<'s> for std::string::String {
    fn to_js(self, lock: &mut Lock<'s>) -> v8::Local<'s, v8::Value> {
        self.as_str().to_js(lock)
    }
}

impl<'s> ToJS<'s> for &str {
    fn to_js(self, lock: &mut Lock<'s>) -> v8::Local<'s, v8::Value> {
        let isolate = lock.isolate();
        // SAFETY: `lock` proves the isolate is locked and a HandleScope for `'s` is open.
        unsafe { v8::Local::from_ffi(v8::ffi::local_new_string(isolate.as_ffi(), self)) }
    }
}

impl<'s> FromJS<'s> for std::string::String {
    type ResultType = Self;

    fn type_name() -> String {
        "string".to_owned()
    }

    fn from_js(
        lock: &mut Lock<'s>,
        value: v8::Local<'s, v8::Value>,
    ) -> Result<Self::ResultType, Error> {
        unwrap_string(lock, value)
    }
}

// Number implementation for JavaScript numbers
impl Type for Number {
    fn class_name() -> &'static str {
        "number"
    }

    fn is_exact(value: v8::Local<'_, v8::Value>) -> bool {
        value.is_number()
    }
}

impl<'s> ToJS<'s> for Number {
    fn to_js(self, lock: &mut Lock<'s>) -> v8::Local<'s, v8::Value> {
        let isolate = lock.isolate();
        // SAFETY: `lock` proves the isolate is locked and a HandleScope for `'s` is open.
        unsafe { v8::Local::from_ffi(v8::ffi::local_new_number(isolate.as_ffi(), self.value())) }
    }
}

impl<'s> FromJS<'s> for Number {
    type ResultType = Self;

    fn type_name() -> String {
        "number".to_owned()
    }

    fn from_js(
        lock: &mut Lock<'s>,
        value: v8::Local<'s, v8::Value>,
    ) -> Result<Self::ResultType, Error> {
        let f64_value = unwrap_number(lock, value)?;
        Ok(Self::new(f64_value))
    }
}

// Special implementation for &str - allows functions to accept &str parameters
// by converting JavaScript strings to owned Strings, then borrowing.
// The macro handles passing &arg instead of arg for reference types.
impl Type for &str {
    fn class_name() -> &'static str {
        "string"
    }

    fn is_exact(value: v8::Local<'_, v8::Value>) -> bool {
        value.is_string()
    }
}

impl<'s> FromJS<'s> for &str {
    type ResultType = String;

    fn type_name() -> String {
        "string".to_owned()
    }

    fn from_js(
        lock: &mut Lock<'s>,
        value: v8::Local<'s, v8::Value>,
    ) -> Result<Self::ResultType, Error> {
        unwrap_string(lock, value)
    }
}

impl<'s, T: FromJS<'s, ResultType = T>> FromJS<'s> for &T {
    type ResultType = T;

    fn type_name() -> String {
        T::type_name()
    }

    fn from_js(
        lock: &mut Lock<'s>,
        value: v8::Local<'s, v8::Value>,
    ) -> Result<Self::ResultType, Error> {
        T::from_js(lock, value)
    }

    fn try_from_js(
        lock: &mut Lock<'s>,
        value: v8::Local<'s, v8::Value>,
    ) -> Result<Option<Self::ResultType>, Error> {
        T::try_from_js(lock, value)
    }
}

// Slice type - allows functions to accept &[T] parameters.
// JavaScript arrays are converted to Vec<T>, then borrowed as &[T] by the macro.
impl<T: Type> Type for &[T] {
    fn class_name() -> &'static str {
        "Array"
    }

    fn is_exact(value: v8::Local<'_, v8::Value>) -> bool {
        value.is_array()
    }
}

impl<'s, T: Type + FromJS<'s, ResultType = T>> FromJS<'s> for &[T] {
    type ResultType = Vec<T>;

    fn type_name() -> String {
        "Array".to_owned()
    }

    fn from_js(
        lock: &mut Lock<'s>,
        value: v8::Local<'s, v8::Value>,
    ) -> Result<Self::ResultType, Error> {
        Vec::<T>::from_js(lock, value)
    }

    fn try_from_js(
        lock: &mut Lock<'s>,
        value: v8::Local<'s, v8::Value>,
    ) -> Result<Option<Self::ResultType>, Error> {
        Vec::<T>::try_from_js(lock, value)
    }
}

// Integer types follow C++ JSG (jsg/value.h): a value out of the type's
// range, checked before truncation, is a TypeError, and so is a non-finite
// one, except that i32 reads it as 0. In-range values truncate toward zero.
/// Converts a JavaScript number to an integer type. Fast callbacks use it too, so that both paths
/// convert alike.
pub trait FromNumber: Sized {
    fn from_number(num: f64) -> Result<Self, Error>;
}

macro_rules! impl_integer_from_js {
    ($($type:ty => $name:literal, $non_finite:expr, $range:expr);* $(;)?) => {
        $(
            impl FromNumber for $type {
                #[allow(clippy::cast_possible_truncation, clippy::cast_sign_loss, clippy::cast_lossless, reason = "range is checked first")]
                fn from_number(num: f64) -> Result<Self, Error> {
                    if !num.is_finite() {
                        return $non_finite;
                    }
                    if <$type>::MIN == 0 && num < 0.0 {
                        return Err(Error::new_type_error(
                            "The value cannot be converted because it is negative and this API \
                             expects a positive number.",
                        ));
                    }
                    if num < <$type>::MIN as f64 || num > <$type>::MAX as f64 {
                        return Err(Error::new_type_error($range));
                    }
                    Ok(num as $type)
                }
            }

            impl<'s> FromJS<'s> for $type {
                type ResultType = Self;

                fn type_name() -> String {
                    $name.to_owned()
                }

                fn from_js(lock: &mut Lock<'s>, value: v8::Local<'s, v8::Value>) -> Result<Self::ResultType, Error> {
                    let num = unwrap_number(lock, value)?;
                    Self::from_number(num)
                }
            }
        )*
    };
}

/// Converts `value` with V8's `ToString` (which, unlike JavaScript's `String()`, rejects a
/// Symbol). If the coercion throws, or execution is terminating, fails with an error for which
/// [`Error::is_pending`] is true.
fn unwrap_string<'s>(
    lock: &mut Lock<'s>,
    value: v8::Local<'s, v8::Value>,
) -> Result<String, Error> {
    v8::call_into_js(lock, |lock| {
        let mut result = String::new();
        // SAFETY: The isolate is locked and value is a valid V8 local handle.
        let ok = unsafe {
            v8::ffi::unwrap_string(lock.isolate().as_ffi(), value.into_ffi(), &mut result)
        }?;
        Ok(ok.then_some(result))
    })
}

/// Converts `value` with V8's `ToNumber` (which, unlike JavaScript's `Number()`, rejects a
/// `BigInt`). If the coercion throws, or execution is terminating, fails with an error for which
/// [`Error::is_pending`] is true.
fn unwrap_number<'s>(lock: &mut Lock<'s>, value: v8::Local<'s, v8::Value>) -> Result<f64, Error> {
    v8::call_into_js(lock, |lock| {
        let mut result = 0.0;
        // SAFETY: The isolate is locked and value is a valid V8 local handle.
        let ok = unsafe {
            v8::ffi::unwrap_number(lock.isolate().as_ffi(), value.into_ffi(), &mut result)
        };
        Ok(ok.then_some(result))
    })
}

fn not_an_integer<T>() -> Result<T, Error> {
    Err(Error::new_type_error(
        "The value cannot be converted because it is not an integer.",
    ))
}

fn unsigned_range<T: std::fmt::Display>(max: T) -> String {
    format!("Value out of range. Must be less than or equal to {max}.")
}

fn signed_range<T: std::fmt::Display>(min: T, max: T) -> String {
    format!("Value out of range. Must be between {min} and {max} (inclusive).")
}

impl_integer_from_js!(
    u8 => "octet", not_an_integer(), unsigned_range(u8::MAX);
    u16 => "unsigned short integer", not_an_integer(), unsigned_range(u16::MAX);
    u32 => "unsigned integer", not_an_integer(), unsigned_range(u32::MAX);
    i8 => "byte", not_an_integer(), signed_range(i8::MIN, i8::MAX);
    i16 => "short integer", not_an_integer(), signed_range(i16::MIN, i16::MAX);
    i32 => "integer", Ok(0), signed_range(i32::MIN, i32::MAX);
);

// Like C++ JSG's int64_t (jsg/value.h), a BigInt must fit, and any other value is coerced to a
// number, which must be finite and in range, and truncates toward zero.
impl<'s> FromJS<'s> for i64 {
    type ResultType = Self;

    fn type_name() -> String {
        "bigint".to_owned()
    }

    #[expect(
        clippy::cast_possible_truncation,
        clippy::cast_precision_loss,
        reason = "like C++ JSG, the range is checked in doubles, where i64::MAX rounds up to 2^63, \
                  which then saturates"
    )]
    fn from_js(lock: &mut Lock<'s>, value: v8::Local<'s, v8::Value>) -> Result<Self, Error> {
        if let Ok(big_int) = v8::Local::<v8::BigInt>::try_from(value) {
            return big_int.int64_value().ok_or_else(|| {
                Error::new_type_error(
                    "The value cannot be converted because it would be truncated.",
                )
            });
        }
        let num = unwrap_number(lock, value)?;
        if !num.is_finite() {
            return not_an_integer();
        }
        if num < Self::MIN as f64 || num > Self::MAX as f64 {
            return Err(Error::new_type_error(signed_range(Self::MIN, Self::MAX)));
        }
        Ok(num as Self)
    }
}

// Like C++ JSG's kj::Date (jsg/value.h), a Date or a number of milliseconds since the epoch, which
// must be finite and within the range of `kj::Date`, `i64` nanoseconds either side of the epoch.
// Fractions of a millisecond are dropped.
impl<'s> FromJS<'s> for std::time::SystemTime {
    type ResultType = Self;

    fn type_name() -> String {
        "date".to_owned()
    }

    fn from_js(lock: &mut Lock<'s>, value: v8::Local<'s, v8::Value>) -> Result<Self, Error> {
        let type_name = value.type_of(lock);
        Self::try_from_js(lock, value)?
            .ok_or_else(|| Error::new_type_error(format!("Expected date but got {type_name}")))
    }

    #[expect(
        clippy::cast_possible_truncation,
        clippy::cast_precision_loss,
        reason = "the bounds are compared in doubles as in C++ JSG, and the milliseconds are then \
                  in range"
    )]
    fn try_from_js(
        lock: &mut Lock<'s>,
        value: v8::Local<'s, v8::Value>,
    ) -> Result<Option<Self>, Error> {
        let millis = if let Ok(date) = v8::Local::<v8::Date>::try_from(value) {
            date.value_of()
        } else if value.is_number() {
            Number::from_js(lock, value)?.value()
        } else {
            return Ok(None);
        };
        if !millis.is_finite() {
            return Err(Error::new_type_error(
                "The value cannot be converted because it is not a valid Date.",
            ));
        }
        let nanos = millis * 1_000_000.0;
        if nanos >= i64::MAX as f64 {
            return Err(Error::new_type_error(
                "This API doesn't support dates after 2189.",
            ));
        }
        if nanos <= i64::MIN as f64 {
            return Err(Error::new_type_error(
                "This API doesn't support dates before 1687.",
            ));
        }
        let millis = millis as i64;
        let offset = std::time::Duration::from_millis(millis.unsigned_abs());
        Ok(Some(if millis < 0 {
            std::time::UNIX_EPOCH - offset
        } else {
            std::time::UNIX_EPOCH + offset
        }))
    }
}

// =============================================================================
// Wrapper type implementations
// =============================================================================

impl<'s> ToJS<'s> for () {
    fn to_js(self, lock: &mut Lock<'s>) -> v8::Local<'s, v8::Value> {
        lock.undefined()
    }
}

impl<'s, T: ToJS<'s>> ToJS<'s> for Option<T> {
    fn to_js(self, lock: &mut Lock<'s>) -> v8::Local<'s, v8::Value> {
        match self {
            Some(value) => value.to_js(lock),
            None => lock.undefined(),
        }
    }
}

impl<'s, T: Type + ToJS<'s>> ToJS<'s> for NonCoercible<T> {
    fn to_js(self, lock: &mut Lock<'s>) -> v8::Local<'s, v8::Value> {
        self.into_inner().to_js(lock)
    }
}

impl<'s, T: ToJS<'s>> ToJS<'s> for Nullable<T> {
    fn to_js(self, lock: &mut Lock<'s>) -> v8::Local<'s, v8::Value> {
        match self {
            Self::Some(value) => value.to_js(lock),
            Self::Null => lock.null(),
            Self::Undefined => lock.undefined(),
        }
    }
}

impl<'s, T: ToJS<'s>> ToJS<'s> for Lenient<T> {
    fn to_js(self, lock: &mut Lock<'s>) -> v8::Local<'s, v8::Value> {
        match self {
            Self::Some(value) => value.to_js(lock),
            Self::Null => lock.null(),
            Self::Undefined | Self::Unconvertable(_) => lock.undefined(),
        }
    }
}

// Like C++ `jsg::Optional<T>`, only `undefined` is absent; `null` goes to `T`.
impl<'s, T: FromJS<'s>> FromJS<'s> for Option<T> {
    type ResultType = Option<T::ResultType>;

    fn type_name() -> String {
        T::type_name()
    }

    fn is_optional() -> bool {
        true
    }

    fn from_js(
        lock: &mut Lock<'s>,
        value: v8::Local<'s, v8::Value>,
    ) -> Result<Self::ResultType, Error> {
        if value.is_undefined() {
            return Ok(None);
        }
        Ok(Some(T::from_js(lock, value)?))
    }

    fn try_from_js(
        lock: &mut Lock<'s>,
        value: v8::Local<'s, v8::Value>,
    ) -> Result<Option<Self::ResultType>, Error> {
        if value.is_undefined() {
            return Ok(Some(None));
        }
        Ok(T::try_from_js(lock, value)?.map(Some))
    }
}

impl<'s, T: Type + FromJS<'s>> FromJS<'s> for NonCoercible<T> {
    type ResultType = NonCoercible<T::ResultType>;

    fn type_name() -> String {
        T::type_name()
    }

    fn from_js(
        lock: &mut Lock<'s>,
        value: v8::Local<'s, v8::Value>,
    ) -> Result<Self::ResultType, Error> {
        if !T::is_exact(value) {
            let error_msg = format!(
                "Expected a {} value but got {}",
                T::class_name(),
                value.type_of(lock)
            );
            return Err(Error::new_type_error(error_msg));
        }
        Ok(<Self::ResultType>::new(T::from_js(lock, value)?))
    }

    fn try_from_js(
        lock: &mut Lock<'s>,
        value: v8::Local<'s, v8::Value>,
    ) -> Result<Option<Self::ResultType>, Error> {
        if !T::is_exact(value) {
            return Ok(None);
        }
        Ok(T::try_from_js(lock, value)?.map(<Self::ResultType>::new))
    }
}

impl<'s, T: FromJS<'s>> FromJS<'s> for Nullable<T> {
    type ResultType = Nullable<T::ResultType>;

    fn type_name() -> String {
        T::type_name()
    }

    fn from_js(
        lock: &mut Lock<'s>,
        value: v8::Local<'s, v8::Value>,
    ) -> Result<Self::ResultType, Error> {
        if value.is_null() {
            Ok(Nullable::Null)
        } else if value.is_undefined() {
            Ok(Nullable::Undefined)
        } else {
            Ok(Nullable::Some(T::from_js(lock, value)?))
        }
    }

    fn try_from_js(
        lock: &mut Lock<'s>,
        value: v8::Local<'s, v8::Value>,
    ) -> Result<Option<Self::ResultType>, Error> {
        if value.is_null() {
            Ok(Some(Nullable::Null))
        } else if value.is_undefined() {
            Ok(Some(Nullable::Undefined))
        } else {
            Ok(T::try_from_js(lock, value)?.map(Nullable::Some))
        }
    }
}

impl<'s, T: FromJS<'s>> FromJS<'s> for Lenient<T> {
    type ResultType = Lenient<T::ResultType>;

    fn type_name() -> String {
        T::type_name()
    }

    fn is_optional() -> bool {
        true
    }

    fn from_js(
        lock: &mut Lock<'s>,
        value: v8::Local<'s, v8::Value>,
    ) -> Result<Self::ResultType, Error> {
        if value.is_null() {
            Ok(Lenient::Null)
        } else if value.is_undefined() {
            Ok(Lenient::Undefined)
        } else {
            match T::try_from_js(lock, value)? {
                Some(v) => Ok(Lenient::Some(v)),
                None => Ok(Lenient::Unconvertable(v8::TracedGlobal::new(lock, value))),
            }
        }
    }
}

// =============================================================================
// Array type implementations (Vec<T>)
// =============================================================================

impl<T: Type> Type for Vec<T> {
    fn class_name() -> &'static str {
        "Array"
    }

    fn is_exact(value: v8::Local<'_, v8::Value>) -> bool {
        value.is_array()
    }
}

impl<'s, T: ToJS<'s>> ToJS<'s> for Vec<T> {
    fn to_js(self, lock: &mut Lock<'s>) -> v8::Local<'s, v8::Value> {
        let array = v8::Array::new(lock, self.len());
        for (i, item) in self.into_iter().enumerate() {
            let value = item.to_js(lock);
            array.set(lock, i, value);
        }
        array.into()
    }
}

impl<'s, T: Type + FromJS<'s, ResultType = T>> FromJS<'s> for Vec<T> {
    type ResultType = Self;

    fn type_name() -> String {
        "Array".to_owned()
    }

    fn from_js(
        lock: &mut Lock<'s>,
        value: v8::Local<'s, v8::Value>,
    ) -> Result<Self::ResultType, Error> {
        let type_name = value.type_of(lock);
        Self::try_from_js(lock, value)?
            .ok_or_else(|| Error::new_type_error(format!("Expected Array but got {type_name}")))
    }

    fn try_from_js(
        lock: &mut Lock<'s>,
        value: v8::Local<'s, v8::Value>,
    ) -> Result<Option<Self::ResultType>, Error> {
        let Ok(array) = v8::Local::<v8::Array>::try_from(value) else {
            return Ok(None);
        };

        let globals = array.iterate(lock)?;
        let mut result = Self::with_capacity(globals.len());
        for global in globals {
            let local = global.as_local(lock);
            result.push(T::from_js(lock, local)?);
        }
        Ok(Some(result))
    }
}

// =============================================================================
// TypedArray implementations
// =============================================================================
//
// This macro generates implementations for TypedArray types:
// - `v8::*Array` marker types: `Type` trait for type-safe V8 handles
// - `Vec<T>`: `Type`, `ToJS`, `FromJS` for bidirectional conversion
// - `&[T]`: `Type`, `FromJS` for parameter-only conversion (delegates to Vec<T>)
//
// Supported mappings:
// - `Vec<u8>` / `&[u8]`   <-> `Uint8Array`
// - `Vec<u16>` / `&[u16]` <-> `Uint16Array`
// - `Vec<u32>` / `&[u32]` <-> `Uint32Array`
// - `Vec<i8>` / `&[i8]`   <-> `Int8Array`
// - `Vec<i16>` / `&[i16]` <-> `Int16Array`
// - `Vec<i32>` / `&[i32]` <-> `Int32Array`
// - `Vec<f32>` / `&[f32]` <-> `Float32Array`
// - `Vec<f64>` / `&[f64]` <-> `Float64Array`
// - `Vec<i64>` / `&[i64]` <-> `BigInt64Array`
// - `Vec<u64>` / `&[u64]` <-> `BigUint64Array`

macro_rules! impl_typed_array {
    ($elem:ty, $marker:ident, $is_check:ident, $new_fn:ident, $unwrap_fn:ident) => {
        impl Type for v8::$marker {
            fn class_name() -> &'static str {
                stringify!($marker)
            }
            fn is_exact(value: v8::Local<'_, v8::Value>) -> bool {
                value.$is_check()
            }
        }

        impl Type for Vec<$elem> {
            fn class_name() -> &'static str {
                stringify!($marker)
            }
            fn is_exact(value: v8::Local<'_, v8::Value>) -> bool {
                value.$is_check()
            }
        }

        impl Type for &[$elem] {
            fn class_name() -> &'static str {
                stringify!($marker)
            }
            fn is_exact(value: v8::Local<'_, v8::Value>) -> bool {
                value.$is_check()
            }
        }

        impl<'s> ToJS<'s> for Vec<$elem> {
            fn to_js(self, lock: &mut Lock<'s>) -> v8::Local<'s, v8::Value> {
                let isolate = lock.isolate();
                // SAFETY: Lock guarantees the isolate is locked and a HandleScope is active.
                unsafe {
                    v8::Local::from_ffi(v8::ffi::$new_fn(
                        isolate.as_ffi(),
                        self.as_ptr(),
                        self.len(),
                    ))
                }
            }
        }

        impl<'s> FromJS<'s> for Vec<$elem> {
            type ResultType = Self;

            fn type_name() -> String {
                stringify!($marker).to_owned()
            }

            fn from_js(
                lock: &mut Lock<'s>,
                value: v8::Local<'s, v8::Value>,
            ) -> Result<Self, Error> {
                if !value.$is_check() {
                    return Err(Error::new_type_error(format!(
                        "Expected {} but got {}",
                        stringify!($marker),
                        value.type_of(lock)
                    )));
                }
                // SAFETY: The isolate is locked and value is a valid V8 local handle of the correct TypedArray type.
                Ok(unsafe { v8::ffi::$unwrap_fn(lock.isolate().as_ffi(), value.into_ffi()) }?)
            }

            fn try_from_js(
                lock: &mut Lock<'s>,
                value: v8::Local<'s, v8::Value>,
            ) -> Result<Option<Self>, Error> {
                if !value.$is_check() {
                    return Ok(None);
                }
                // SAFETY: The isolate is locked and value is a valid V8 local handle of the correct TypedArray type.
                Ok(Some(unsafe {
                    v8::ffi::$unwrap_fn(lock.isolate().as_ffi(), value.into_ffi())
                }?))
            }
        }

        impl<'s> FromJS<'s> for &[$elem] {
            type ResultType = Vec<$elem>;

            fn type_name() -> String {
                stringify!($marker).to_owned()
            }

            fn from_js(
                lock: &mut Lock<'s>,
                value: v8::Local<'s, v8::Value>,
            ) -> Result<Vec<$elem>, Error> {
                Vec::<$elem>::from_js(lock, value)
            }

            fn try_from_js(
                lock: &mut Lock<'s>,
                value: v8::Local<'s, v8::Value>,
            ) -> Result<Option<Vec<$elem>>, Error> {
                Vec::<$elem>::try_from_js(lock, value)
            }
        }
    };
}

impl_typed_array!(
    u8,
    Uint8Array,
    is_uint8_array,
    local_new_uint8_array,
    unwrap_uint8_array
);
impl_typed_array!(
    u16,
    Uint16Array,
    is_uint16_array,
    local_new_uint16_array,
    unwrap_uint16_array
);
impl_typed_array!(
    u32,
    Uint32Array,
    is_uint32_array,
    local_new_uint32_array,
    unwrap_uint32_array
);
impl_typed_array!(
    i8,
    Int8Array,
    is_int8_array,
    local_new_int8_array,
    unwrap_int8_array
);
impl_typed_array!(
    i16,
    Int16Array,
    is_int16_array,
    local_new_int16_array,
    unwrap_int16_array
);
impl_typed_array!(
    i32,
    Int32Array,
    is_int32_array,
    local_new_int32_array,
    unwrap_int32_array
);
impl_typed_array!(
    f32,
    Float32Array,
    is_float32_array,
    local_new_float32_array,
    unwrap_float32_array
);
impl_typed_array!(
    f64,
    Float64Array,
    is_float64_array,
    local_new_float64_array,
    unwrap_float64_array
);
impl_typed_array!(
    i64,
    BigInt64Array,
    is_bigint64_array,
    local_new_bigint64_array,
    unwrap_bigint64_array
);
impl_typed_array!(
    u64,
    BigUint64Array,
    is_biguint64_array,
    local_new_biguint64_array,
    unwrap_biguint64_array
);

// =============================================================================
// ArrayBuffer, ArrayBufferView, SharedArrayBuffer
// =============================================================================
//
// These three types only implement `Type` (used by `NonCoercible<T>` for type
// checking).  They do not have element-typed `ToJS`/`FromJS` conversions and
// do not fit the `impl_typed_array!` macro.

impl Type for v8::ArrayBuffer {
    fn class_name() -> &'static str {
        "ArrayBuffer"
    }
    fn is_exact(value: v8::Local<'_, v8::Value>) -> bool {
        value.is_array_buffer()
    }
}

impl Type for v8::ArrayBufferView {
    fn class_name() -> &'static str {
        "ArrayBufferView"
    }
    fn is_exact(value: v8::Local<'_, v8::Value>) -> bool {
        value.is_array_buffer_view()
    }
}

// =============================================================================
// Handle pass-through
// =============================================================================

impl<'s, T> ToJS<'s> for v8::Local<'s, T>
where
    Self: Into<v8::Local<'s, v8::Value>>,
{
    fn to_js(self, _lock: &mut Lock<'s>) -> v8::Local<'s, v8::Value> {
        self.into()
    }
}

impl<'s> FromJS<'s> for v8::Local<'s, v8::Value> {
    type ResultType = Self;

    fn type_name() -> String {
        // C++ JSG names `v8::Local<v8::Value>` after `v8::Value`.
        "Value".to_owned()
    }

    fn from_js(_lock: &mut Lock<'s>, value: Self) -> Result<Self, Error> {
        Ok(value)
    }
}

#[cfg(test)]
#[path = "wrappable-test.rs"]
mod tests;
