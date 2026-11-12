// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#![allow(unsafe_code, reason = "converts values through the V8 FFI")]
#![allow(
    clippy::allow_attributes,
    reason = "`impl_integer!` uses `allow`, since its cast lints fire for only some integer types"
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
//! | `JsString<'s>` | `string` (coerces without copying out of the V8 heap) |
//! | `&str` | `string` |
//! | `bool` | `boolean` |
//! | `f64` | `number`; from any value, with `ToNumber` |
//! | `f32` | `number` (to JavaScript only) |
//! | `u8`, `u16`, `u32`, `i8`, `i16`, `i32` | `number` |
//! | `i64`, `u64` | `bigint`; from a `bigint` or a `number` |
//! | `SystemTime` | `Date`; from a `Date` or `number` of milliseconds |
//! | `Option<T>` | `T` or `undefined` |
//! | `Nullable<T>` | `T` or `null` (`undefined` coerces to `null` on input) |
//! | `NullOrUndefined<T>` | `T`, `null`, or `undefined` |
//! | `Lenient<T>` | `T`, `null`, or `undefined` (shallow type mismatches silently become `undefined`) |
//! | `Result<T, E>` | `T` or throws |
//! | `NonCoercible<T>` | `T` (strict type checking) |
//! | `T: Struct` | `object` |
//! | `#[jsg_oneof]` enum | any of its variants' types (see `src/rust/jsg/README.md`) |
//! | `jsg::Rc<R>` | the resource's JavaScript object |
//! | `Vec<T>` | `Array<T>` |
//! | `&[T]` | `Array<T>` (parameter only) |
//! | `Dict<V>` | `Record<string, V>`: an object other than an array |
//! | `v8::Local<'s, M>` | the value itself, of `M`'s type |
//! | `v8::TracedGlobal<M>` | as `v8::Local<'s, M>`, held past the scope (C++ JSG's `jsg::JsRef<T>`) |
//!
//! [`Option`], [`NullOrUndefined`], and [`Lenient`] method arguments may be omitted.
//! [`Nullable`] arguments remain required even though explicitly passing `undefined` converts to `Null`.
//! Prefer [`NullOrUndefined<T>`] to `Option<Nullable<T>>` to preserve all three states.
//!
//! ## Arrays, Typed Arrays and `ArrayBuffer`s
//!
//! A `Vec<T>` converts to an `Array` of the converted elements, numbers included:
//! `vec![1u8, 2, 3]` becomes `[1, 2, 3]`, and a `Vec<i64>` an `Array` of `BigInt`s. A typed array
//! or an `ArrayBuffer` is an explicit conversion of a slice:
//!
//! | Call | JavaScript Type |
//! |------|-----------------|
//! | `numbers.to_js_typed_array(lock)` ([`ToJsTypedArray`](crate::ToJsTypedArray)) | the typed array of the element type: `Uint8Array` for `[u8]`, `Float64Array` for `[f64]`, `BigInt64Array` for `[i64]`, ... |
//! | `bytes.to_js_array_buffer(lock)` ([`ToJsArrayBuffer`](crate::ToJsArrayBuffer)) | `ArrayBuffer` holding the bytes; `[u8]` only |
//!
//! From JavaScript, a `Vec<T>` or `&[T]` parameter reads an `Array` whose elements convert to
//! `T`. A typed array parameter is a handle, `v8::Local<'s, v8::Uint8Array>` and so on, which
//! reads its memory in place with `as_slice(lock)`. An `ArrayBuffer` is a
//! `v8::Local<'s, v8::ArrayBuffer>`, and `v8::Local<'s, v8::ArrayBufferView>` accepts any view.
//!
//! ## Integers
//!
//! The integer types (`u8`, `u16`, `u32`, `i8`, `i16`, `i32`) convert to a `number` with
//! `Integer::New`, as C++ JSG's do, and from JavaScript like C++ JSG's integers:
//!
//! - Non-finite values are a `TypeError`, except that `i32` reads them as `0`
//! - Other values are truncated toward zero (e.g., `3.7` → `3`, `-2.9` → `-2`)
//! - Values out of the type's range after truncation are a `TypeError` (e.g., `256` for `u8`)
//!
//! Truncating before the range check is what Web IDL's `[EnforceRange]` and V8's
//! `kEnforceRangeBit` fast API arguments do, so a fraction just past a range limit converts:
//! `255.5` is `255` and `-0.5` is `0` for `u8`.
//!
//! For strict validation, wrap parameters in `NonCoercible<T>` or validate manually.
//!
//! `i64` and `u64` convert to a `BigInt`, and from JavaScript like C++ JSG's `int64_t` and
//! `uint64_t`: a `BigInt` that does not fit is a `TypeError`, and any other value converts as
//! above, except that the range check sees the untruncated value.

use crate::Coercible;
use crate::Dict;
use crate::Error;
use crate::Lenient;
use crate::Lock;
use crate::NonCoercible;
use crate::NullOrUndefined;
use crate::Nullable;
use crate::Type;
use crate::UnionMember;
use crate::UnionTypes;
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
/// `Traced` requires `Send`, which keeps resources free of data races. Resources, function data
/// and reaction data, including the values of typed promises, hold only `Traced` types, and the
/// isolate lock is their mutex: one thread uses them at a time, but the lock passes between
/// threads, and a thread that holds it can move a value out and keep it after releasing the lock.
/// So they must be `Send`, for the reason `Mutex<T>: Sync` needs `T: Send`, but need not be
/// `Sync`. [`jsg::Rc`](crate::Rc) and [`jsg::Weak`](crate::Weak) are `Send` because they touch
/// their counts and their resource only under the lock of its isolate. `std::rc::Rc` is not
/// `Send`, so it has no `Traced` impl. See `src/rust/jsg/README.md`, safety rule 5.
///
/// A hand-written impl must also visit every GC reference the value holds, keep traced state with
/// its one GC owner, hold no [`v8::Global`] (safety rule 2), and change nothing but through the
/// visitor. Breaking those leaks, panics or aborts, but can't cause undefined behaviour, so the
/// trait is safe.
///
/// # Built-in implementations
///
/// - **Primitives** (`bool`, `String`, integers, floats, atomics, `()`) — no-op.
/// - **`Option<T>`**, **`Nullable<T>`**, **`NullOrUndefined<T>`** — delegates to the inner value if present.
/// - **Collections** (`Vec<T>`, `Dict<V>`, `HashMap<K,V>`, `BTreeMap<K,V>`, `HashSet<T>`,
///   `BTreeSet<T>`) — iterates elements/values and traces each. Map keys are not visited, so
///   they must be [`HoldsNoGcReferences`].
/// - **`std::cell::Cell<T>`**, **`std::cell::RefCell<T>`**, **`std::sync::Mutex<T>`**,
///   **`std::sync::Arc<T>`** — only for `T: HoldsNoGcReferences` (and `Sync`, for `Arc`), so
///   there is nothing in them to visit; see [`HoldsNoGcReferences`].
/// - **[`jsg::Cell`](crate::Cell)**, **[`jsg::RefCell`](crate::RefCell)** — delegate to the
///   value, and detach it from the cell's owner as it leaves the cell; see
///   [`GcVisitor::is_detaching`](crate::v8::GcVisitor::is_detaching).
/// - **`jsg::Rc<T>`** — visits the reference via `GcVisitor::visit_rc`.
/// - **`jsg::Weak<T>`** — no-op (weak refs don't keep targets alive).
/// - **`jsg::v8::TracedGlobal<T>`** — visits via `GcVisitor::visit_traced_global`.
pub trait Traced: Send {
    /// Visit GC-visible references held by this value.
    ///
    /// It has no default, so that an impl can't skip a field by leaving it out. A type with no
    /// GC-visible references (primitives, plain data structs, etc.) uses `jsg_traced!`, which
    /// generates a no-op `trace` and implements [`HoldsNoGcReferences`].
    ///
    /// A hand-written implementation must visit every field that can hold a GC reference
    /// (`TracedGlobal`, `jsg::Rc`, and types holding them). Nothing checks this. The type must
    /// not hold a [`v8::Global`](crate::v8::Global), which is always strong, so a cycle through
    /// it is never collected. Other than through `visitor`, it must not change the value or
    /// anything that holds it: a [`crate::Cell`] visits its value in place, so writing into any
    /// `jsg::Cell` during a visit panics.
    fn trace(&self, visitor: &mut crate::v8::GcVisitor);
}

/// Marks a `Traced` type that holds no GC references: no `jsg::Rc` and no `TracedGlobal` that
/// its `trace` visits, directly or inside another type. (The keys of a map are not visited.)
///
/// `std::cell::Cell<T>` and `std::cell::RefCell<T>` are `Traced` only for
/// `T: HoldsNoGcReferences`. A reference that GC has visited in a field is weak and depends on
/// the field's owner to trace it, and a move out of a std cell is bitwise and leaves it that
/// way. A resource is reached only through `&self`, except in its `Drop` impl, so a cell is the
/// only way to move a reference out of a field of a live resource. With the std cells closed to
/// them, every `jsg::Rc` and `TracedGlobal` that leaves a traced field of a live resource is
/// strong, unless a hand-written `impl Traced` reaches it through a std cell. A field that holds one and changes
/// after construction is a [`crate::Cell`] or [`crate::RefCell`], which take any `Traced` type.
///
/// Every built-in `Traced` type implements it, except those that can hold GC references:
/// `jsg::Rc`, `TracedGlobal`, `Lenient`, `TracedPromise`, `TracedPromiseResolver` and
/// `TracedAsyncContextFrame`. Wrappers and collections implement it when their contents do.
/// `jsg_traced!` implements it for the types it covers, and `#[jsg_struct]` and `#[jsg_oneof]`
/// implement it when every field or variant does, so for those types it is derived rather than
/// claimed, and an impl by hand conflicts with it. Implement it for a hand-written `Traced` type
/// only if its `trace` visits nothing.
pub trait HoldsNoGcReferences: Traced {}

/// Generates no-op `Traced` implementations for types with no GC-visible references.
macro_rules! impl_traced_noop {
    ($($t:ty),* $(,)?) => {
        $(impl Traced for $t {
            fn trace(&self, _visitor: &mut crate::v8::GcVisitor) {}
        }

        impl HoldsNoGcReferences for $t {})*
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
    std::sync::atomic::AtomicBool,
    std::sync::atomic::AtomicU8,
    std::sync::atomic::AtomicU16,
    std::sync::atomic::AtomicU32,
    std::sync::atomic::AtomicU64,
    std::sync::atomic::AtomicUsize,
    std::sync::atomic::AtomicI8,
    std::sync::atomic::AtomicI16,
    std::sync::atomic::AtomicI32,
    std::sync::atomic::AtomicI64,
    std::sync::atomic::AtomicIsize,
    std::time::SystemTime,
    crate::v8::BackingStore,
    &str,
);

impl<T: Traced> Traced for Option<T> {
    fn trace(&self, visitor: &mut crate::v8::GcVisitor) {
        if let Some(inner) = self {
            inner.trace(visitor);
        }
    }
}

impl<T: HoldsNoGcReferences> HoldsNoGcReferences for Option<T> {}

impl<T: Traced> Traced for Lenient<T> {
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
    fn trace(&self, visitor: &mut crate::v8::GcVisitor) {
        if let Self::Some(inner) = self {
            inner.trace(visitor);
        }
    }
}

impl<T: HoldsNoGcReferences> HoldsNoGcReferences for Nullable<T> {}

impl<T: Traced> Traced for NullOrUndefined<T> {
    fn trace(&self, visitor: &mut crate::v8::GcVisitor) {
        if let Self::Some(inner) = self {
            inner.trace(visitor);
        }
    }
}

impl<T: HoldsNoGcReferences> HoldsNoGcReferences for NullOrUndefined<T> {}

impl<T: Traced> Traced for NonCoercible<T> {
    fn trace(&self, visitor: &mut crate::v8::GcVisitor) {
        self.as_ref().trace(visitor);
    }
}

impl<T: HoldsNoGcReferences> HoldsNoGcReferences for NonCoercible<T> {}

impl<T: Traced> Traced for Vec<T> {
    fn trace(&self, visitor: &mut crate::v8::GcVisitor) {
        for item in self {
            item.trace(visitor);
        }
    }
}

impl<T: HoldsNoGcReferences> HoldsNoGcReferences for Vec<T> {}

impl<V: Traced> Traced for Dict<V> {
    fn trace(&self, visitor: &mut crate::v8::GcVisitor) {
        for (_, value) in self.iter() {
            value.trace(visitor);
        }
    }
}

impl<V: HoldsNoGcReferences> HoldsNoGcReferences for Dict<V> {}

impl<K: HoldsNoGcReferences, V: Traced, S: std::hash::BuildHasher + Send> Traced
    for std::collections::HashMap<K, V, S>
{
    fn trace(&self, visitor: &mut crate::v8::GcVisitor) {
        for value in self.values() {
            value.trace(visitor);
        }
    }
}

impl<K: HoldsNoGcReferences, V: HoldsNoGcReferences, S: std::hash::BuildHasher + Send>
    HoldsNoGcReferences for std::collections::HashMap<K, V, S>
{
}

impl<K: HoldsNoGcReferences, V: Traced> Traced for std::collections::BTreeMap<K, V> {
    fn trace(&self, visitor: &mut crate::v8::GcVisitor) {
        for value in self.values() {
            value.trace(visitor);
        }
    }
}

impl<K: HoldsNoGcReferences, V: HoldsNoGcReferences> HoldsNoGcReferences
    for std::collections::BTreeMap<K, V>
{
}

impl<T: Traced, S: std::hash::BuildHasher + Send> Traced for std::collections::HashSet<T, S> {
    fn trace(&self, visitor: &mut crate::v8::GcVisitor) {
        for item in self {
            item.trace(visitor);
        }
    }
}

impl<T: HoldsNoGcReferences, S: std::hash::BuildHasher + Send> HoldsNoGcReferences
    for std::collections::HashSet<T, S>
{
}

impl<T: Traced> Traced for std::collections::BTreeSet<T> {
    fn trace(&self, visitor: &mut crate::v8::GcVisitor) {
        for item in self {
            item.trace(visitor);
        }
    }
}

impl<T: HoldsNoGcReferences> HoldsNoGcReferences for std::collections::BTreeSet<T> {}

/// Only for `T: HoldsNoGcReferences`: a `jsg::Rc` or `TracedGlobal` taken out of a std `Cell`
/// would be neither strong nor traced. A cell that holds one is a [`crate::Cell`]. So there is
/// nothing in it to visit.
impl<T: HoldsNoGcReferences> Traced for std::cell::Cell<T> {
    fn trace(&self, _visitor: &mut crate::v8::GcVisitor) {}
}

impl<T: HoldsNoGcReferences> HoldsNoGcReferences for std::cell::Cell<T> {}

/// Only for `T: HoldsNoGcReferences`: a `jsg::Rc` or `TracedGlobal` moved out through a std
/// `RefCell`'s mutable borrow would be neither strong nor traced. A cell that holds one is a
/// [`crate::RefCell`]. So there is nothing in it to visit.
impl<T: HoldsNoGcReferences> Traced for std::cell::RefCell<T> {
    fn trace(&self, _visitor: &mut crate::v8::GcVisitor) {}
}

impl<T: HoldsNoGcReferences> HoldsNoGcReferences for std::cell::RefCell<T> {}

/// Only for `T: HoldsNoGcReferences`: every owner of a shared `Arc` would trace what it holds,
/// though a GC reference has a single parent. Share a resource through a `jsg::Rc` or
/// `jsg::Weak` to it instead.
impl<T: HoldsNoGcReferences + Sync> Traced for std::sync::Arc<T> {
    fn trace(&self, _visitor: &mut crate::v8::GcVisitor) {}
}

impl<T: HoldsNoGcReferences + Sync> HoldsNoGcReferences for std::sync::Arc<T> {}

/// Only for `T: HoldsNoGcReferences`, as for `std::cell::RefCell`.
impl<T: HoldsNoGcReferences> Traced for std::sync::Mutex<T> {
    fn trace(&self, _visitor: &mut crate::v8::GcVisitor) {}
}

impl<T: HoldsNoGcReferences> HoldsNoGcReferences for std::sync::Mutex<T> {}

// =============================================================================
// ToJS trait (Rust → JavaScript)
// =============================================================================

/// Trait for converting Rust values to JavaScript.
///
/// As C++ JSG's `wrap()` can throw, a conversion can fail. Pass its error on with `?`; see the
/// README's safety rules.
///
/// # Errors
///
/// A `&str` or `String` longer than [`v8::String::MAX_LENGTH`] bytes fails with
/// [`Error::invalid_string_length`], a `RangeError` that is not pending. Every other conversion
/// of a single value in this crate fails only with an error for which [`Error::is_pending`] is
/// true: creating a `Date` or a resource's wrapper fails, leaving the exception pending, when
/// V8 throws or an exception or a termination is already pending. A container passes on the
/// first error from converting an element, whichever kind it is. A conversion implemented
/// elsewhere may return any `Error`.
///
/// A `#[jsg_method]`, property getter or wildcard property throws an error from converting its
/// return value with [`Lock::throw_exception`], as it throws an `Err` the method returns. The
/// method has already run by then, so only its return value is lost. JavaScript sees:
///
/// - for a pending error, the exception V8 left pending, unchanged. While execution is
///   terminating, nothing is thrown, and the termination keeps propagating.
/// - for an internal error, an internal error with its message redacted.
/// - for any other error, a new JavaScript error of its type, with its message.
pub trait ToJS<'s>: Sized {
    /// Converts this Rust value into a JavaScript value in the scope of `lock`.
    fn to_js(self, lock: &mut Lock<'s>) -> Result<v8::Local<'s, v8::Value>, Error>;

    /// Converts this Rust value into a JavaScript value for `creator`, the object whose method or
    /// property returned it, as C++ JSG's `wrap()` takes the `creator` of a value: a value that
    /// stays pending, such as a [`Promise`](crate::Promise), keeps `creator` alive until it
    /// settles, as C++ JSG's promises do. A `#[jsg_method]` or property of a resource passes the
    /// resource's wrapper, and a static one `None`. A container passes `creator` on to what it
    /// holds; any other type converts as [`ToJS::to_js`] does, which is the default.
    fn to_js_with_creator(
        self,
        lock: &mut Lock<'s>,
        creator: Option<v8::Local<'s, v8::Object>>,
    ) -> Result<v8::Local<'s, v8::Value>, Error> {
        let _ = creator;
        self.to_js(lock)
    }
}

// =============================================================================
// FromJS trait (JavaScript → Rust)
// =============================================================================

/// Trait for converting JavaScript values to Rust.
///
/// `from_js()` is the normal, required conversion path. `try_from_js()` is an
/// internal shallow-probing path used by [`Lenient`] and by `#[jsg_oneof]`
/// unions. Implementations only need to override `try_from_js()` when their
/// top-level type can fail to match, and the override must return `Ok(None)`
/// rather than an error for a value of the wrong type, so that a union can go
/// on to its other variants.
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

    /// The type's kind in the union conversion algorithm; see [`UnionMember`].
    #[doc(hidden)]
    const UNION_MEMBER: UnionMember = UnionMember::Fallible;

    /// Whether converting an object to this type can run script, such as a getter, a Proxy trap
    /// or a `valueOf`. Converting a value that isn't an object must never run script. `Vec`
    /// reads elements ahead of their conversion only while no conversion can run script, so a
    /// type that reads nothing from an object it converts sets this to false.
    #[doc(hidden)]
    const OBJECT_CONVERSION_MAY_RUN_SCRIPT: bool = true;

    /// Converts `value` during `pass` of a union conversion, or returns
    /// `Ok(None)` if this type does not take part in `pass` or does not match.
    /// A union overrides this to try its own variants, which flattens nested
    /// unions into the enclosing union's passes.
    #[doc(hidden)]
    fn try_from_js_in_union_pass(
        lock: &mut Lock<'s>,
        value: v8::Local<'s, v8::Value>,
        pass: UnionMember,
    ) -> Result<Option<Self::ResultType>, Error> {
        if Self::UNION_MEMBER.takes_part_in(pass) {
            Self::try_from_js(lock, value)
        } else {
            Ok(None)
        }
    }
}

// =============================================================================
// Primitive type implementations
// =============================================================================

// Boolean implementation for JavaScript booleans.
impl Type for bool {
    const UNION_TYPES: UnionTypes = UnionTypes::BOOLEAN;

    fn class_name() -> &'static str {
        "boolean"
    }

    fn is_exact(value: v8::Local<'_, v8::Value>) -> bool {
        value.is_boolean()
    }
}

impl<'s> ToJS<'s> for bool {
    #[inline]
    fn to_js(self, lock: &mut Lock<'s>) -> Result<v8::Local<'s, v8::Value>, Error> {
        let isolate = lock.isolate();
        // SAFETY: `lock` proves the isolate is locked and a HandleScope for `'s` is open.
        Ok(unsafe { v8::Local::from_ffi(v8::ffi::local_new_boolean(isolate.as_ffi(), self)) })
    }
}

impl<'s> FromJS<'s> for bool {
    type ResultType = Self;

    const UNION_MEMBER: UnionMember = UnionMember::Boolean;
    const OBJECT_CONVERSION_MAY_RUN_SCRIPT: Self = false;

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
    const UNION_TYPES: UnionTypes = UnionTypes::STRING;

    fn class_name() -> &'static str {
        "string"
    }

    fn is_exact(value: v8::Local<'_, v8::Value>) -> bool {
        value.is_string()
    }
}

impl<'s> ToJS<'s> for std::string::String {
    #[inline]
    fn to_js(self, lock: &mut Lock<'s>) -> Result<v8::Local<'s, v8::Value>, Error> {
        self.as_str().to_js(lock)
    }
}

impl<'s> ToJS<'s> for &str {
    /// Fails with [`Error::invalid_string_length`] if the string is longer than
    /// [`v8::String::MAX_LENGTH`] bytes.
    #[inline]
    fn to_js(self, lock: &mut Lock<'s>) -> Result<v8::Local<'s, v8::Value>, Error> {
        v8::String::new_from_str(lock, self)
            .map(Into::into)
            .ok_or_else(Error::invalid_string_length)
    }
}

impl<'s> FromJS<'s> for std::string::String {
    type ResultType = Self;

    const UNION_MEMBER: UnionMember = UnionMember::String;

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

// Like C++ JSG's double, an f64 is a number. From JavaScript, any value converts with `ToNumber`.
impl Type for f64 {
    const UNION_TYPES: UnionTypes = UnionTypes::NUMERIC;

    fn class_name() -> &'static str {
        "number"
    }

    fn is_exact(value: v8::Local<'_, v8::Value>) -> bool {
        value.is_number()
    }
}

impl<'s> ToJS<'s> for f64 {
    #[inline]
    fn to_js(self, lock: &mut Lock<'s>) -> Result<v8::Local<'s, v8::Value>, Error> {
        Ok(v8::Number::new(lock, self).into())
    }
}

impl<'s> FromJS<'s> for f64 {
    type ResultType = Self;

    const UNION_MEMBER: UnionMember = UnionMember::Numeric;

    fn type_name() -> String {
        "number".to_owned()
    }

    fn from_js(
        lock: &mut Lock<'s>,
        value: v8::Local<'s, v8::Value>,
    ) -> Result<Self::ResultType, Error> {
        unwrap_number(lock, value)
    }
}

// An f32 is a number too, but only to JavaScript.
impl<'s> ToJS<'s> for f32 {
    #[inline]
    fn to_js(self, lock: &mut Lock<'s>) -> Result<v8::Local<'s, v8::Value>, Error> {
        f64::from(self).to_js(lock)
    }
}

// Special implementation for &str - allows functions to accept &str parameters
// by converting JavaScript strings to owned Strings, then borrowing.
// The macro handles passing &arg instead of arg for reference types.
impl Type for &str {
    const UNION_TYPES: UnionTypes = UnionTypes::STRING;

    fn class_name() -> &'static str {
        "string"
    }

    fn is_exact(value: v8::Local<'_, v8::Value>) -> bool {
        value.is_string()
    }
}

impl<'s> FromJS<'s> for &str {
    type ResultType = String;

    const UNION_MEMBER: UnionMember = UnionMember::String;

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

    const UNION_MEMBER: UnionMember = T::UNION_MEMBER;
    const OBJECT_CONVERSION_MAY_RUN_SCRIPT: bool = T::OBJECT_CONVERSION_MAY_RUN_SCRIPT;

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

    fn try_from_js_in_union_pass(
        lock: &mut Lock<'s>,
        value: v8::Local<'s, v8::Value>,
        pass: UnionMember,
    ) -> Result<Option<Self::ResultType>, Error> {
        T::try_from_js_in_union_pass(lock, value, pass)
    }
}

// Slice type - allows functions to accept &[T] parameters.
// JavaScript arrays are converted to Vec<T>, then borrowed as &[T] by the macro.
impl<T: Type> Type for &[T] {
    const UNION_TYPES: UnionTypes = UnionTypes::SEQUENCE_LIKE;

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

// Integer types follow C++ JSG (jsg/value.h). To JavaScript, an integer of up
// to 32 bits is a number, made with `Integer::New` as in C++ JSG, and an i64 or
// u64 is a BigInt. From JavaScript, a non-finite value is a TypeError, except
// that i32 reads it as 0, and so is a value out of the type's range. Finite
// values truncate toward zero, before the range check for the types of up to 32
// bits and after it for the 64-bit types.
/// Converts a JavaScript number to an integer type. Fast callbacks use it too, so that both paths
/// convert alike.
pub trait FromNumber: Sized {
    fn from_number(num: f64) -> Result<Self, Error>;
}

macro_rules! impl_integer {
    ($($type:ty => $name:literal, $new:ident, $non_finite:expr, $range:expr);* $(;)?) => {
        $(
            impl Type for $type {
                const UNION_TYPES: UnionTypes = UnionTypes::NUMERIC;

                fn class_name() -> &'static str {
                    "number"
                }

                fn is_exact(value: v8::Local<'_, v8::Value>) -> bool {
                    value.is_number()
                }
            }

            impl<'s> ToJS<'s> for $type {
                #[inline]
                fn to_js(self, lock: &mut Lock<'s>) -> Result<v8::Local<'s, v8::Value>, Error> {
                    Ok(v8::Integer::$new(lock, self.into()).into())
                }
            }

            impl FromNumber for $type {
                #[allow(clippy::cast_possible_truncation, clippy::cast_sign_loss, clippy::cast_lossless, reason = "range is checked first")]
                fn from_number(num: f64) -> Result<Self, Error> {
                    if !num.is_finite() {
                        return $non_finite;
                    }
                    let num = num.trunc();
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

                const UNION_MEMBER: UnionMember = UnionMember::Numeric;

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
/// Symbol). If the coercion throws, or an exception or a termination is already pending, fails
/// with an error for which [`Error::is_pending`] is true.
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
/// `BigInt`). If the coercion throws, or an exception or a termination is already pending, fails
/// with an error for which [`Error::is_pending`] is true.
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

impl_integer!(
    u8 => "octet", new_from_unsigned, not_an_integer(), unsigned_range(u8::MAX);
    u16 => "unsigned short integer", new_from_unsigned, not_an_integer(), unsigned_range(u16::MAX);
    u32 => "unsigned integer", new_from_unsigned, not_an_integer(), unsigned_range(u32::MAX);
    i8 => "byte", new, not_an_integer(), signed_range(i8::MIN, i8::MAX);
    i16 => "short integer", new, not_an_integer(), signed_range(i16::MIN, i16::MAX);
    i32 => "integer", new, Ok(0), signed_range(i32::MIN, i32::MAX);
);

impl Type for i64 {
    const UNION_TYPES: UnionTypes = UnionTypes::NUMERIC;

    fn class_name() -> &'static str {
        "bigint"
    }

    fn is_exact(value: v8::Local<'_, v8::Value>) -> bool {
        value.is_big_int()
    }
}

// Like C++ JSG's int64_t (jsg/value.h), an i64 is a BigInt.
impl<'s> ToJS<'s> for i64 {
    #[inline]
    fn to_js(self, lock: &mut Lock<'s>) -> Result<v8::Local<'s, v8::Value>, Error> {
        Ok(v8::BigInt::new(lock, self).into())
    }
}

// Like C++ JSG's int64_t (jsg/value.h), a BigInt must fit, and any other value is coerced to a
// number, which must be finite and in range, and truncates toward zero.
impl<'s> FromJS<'s> for i64 {
    type ResultType = Self;

    const UNION_MEMBER: UnionMember = UnionMember::Numeric;

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

impl Type for u64 {
    const UNION_TYPES: UnionTypes = UnionTypes::NUMERIC;

    fn class_name() -> &'static str {
        "bigint"
    }

    fn is_exact(value: v8::Local<'_, v8::Value>) -> bool {
        value.is_big_int()
    }
}

// Like C++ JSG's uint64_t (jsg/value.h), a u64 is a BigInt.
impl<'s> ToJS<'s> for u64 {
    #[inline]
    fn to_js(self, lock: &mut Lock<'s>) -> Result<v8::Local<'s, v8::Value>, Error> {
        Ok(v8::BigInt::from_unsigned(lock, self).into())
    }
}

// Like C++ JSG's uint64_t (jsg/value.h), a BigInt must fit, and any other value is coerced to a
// number, which must be finite, not negative and in range, and truncates toward zero.
impl<'s> FromJS<'s> for u64 {
    type ResultType = Self;

    const UNION_MEMBER: UnionMember = UnionMember::Numeric;

    fn type_name() -> String {
        "bigint".to_owned()
    }

    #[expect(
        clippy::cast_possible_truncation,
        clippy::cast_precision_loss,
        clippy::cast_sign_loss,
        reason = "like C++ JSG, the range is checked in doubles, where u64::MAX rounds up to 2^64, \
                  which then saturates"
    )]
    fn from_js(lock: &mut Lock<'s>, value: v8::Local<'s, v8::Value>) -> Result<Self, Error> {
        if let Ok(big_int) = v8::Local::<v8::BigInt>::try_from(value) {
            return big_int.uint64_value().ok_or_else(|| {
                Error::new_type_error(
                    "The value cannot be converted because it is either negative and this API \
                     expects a positive bigint, or the value would be truncated.",
                )
            });
        }
        let num = unwrap_number(lock, value)?;
        if !num.is_finite() {
            return not_an_integer();
        }
        if num < 0.0 {
            return Err(Error::new_type_error(
                "The value cannot be converted because it is negative and this API expects a \
                 positive bigint.",
            ));
        }
        if num > Self::MAX as f64 {
            return Err(Error::new_type_error(unsigned_range(Self::MAX)));
        }
        Ok(num as Self)
    }
}

// Like C++ JSG's `kj::Date`, which is in none of Web IDL's categories, so a union tries it in
// the pass for types that aren't strings, numbers or booleans, where it also takes a number.
impl Type for std::time::SystemTime {
    const UNION_NAMES: &'static [&'static str] = &["SystemTime"];

    fn class_name() -> &'static str {
        "Date"
    }

    fn is_exact(value: v8::Local<'_, v8::Value>) -> bool {
        value.is_date()
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
            f64::from_js(lock, value)?
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

// Like C++ JSG's kj::Date (jsg/value.h), a time converts to a Date of the milliseconds since the
// epoch, truncated toward zero. A time beyond the range of JavaScript dates, which `kj::Date`
// cannot hold, gives an invalid date.
impl<'s> ToJS<'s> for std::time::SystemTime {
    #[expect(
        clippy::cast_precision_loss,
        reason = "a time of more than 2^53 milliseconds is beyond the range of JavaScript dates \
                  either way"
    )]
    #[inline]
    fn to_js(self, lock: &mut Lock<'s>) -> Result<v8::Local<'s, v8::Value>, Error> {
        let millis = match self.duration_since(std::time::UNIX_EPOCH) {
            Ok(after) => after.as_millis() as f64,
            Err(before) => -(before.duration().as_millis() as f64),
        };
        Ok(v8::Date::new(lock, millis)?.into())
    }
}

// =============================================================================
// Wrapper type implementations
// =============================================================================

impl<'s> ToJS<'s> for () {
    #[inline]
    fn to_js(self, lock: &mut Lock<'s>) -> Result<v8::Local<'s, v8::Value>, Error> {
        Ok(lock.undefined())
    }
}

impl<'s, T: ToJS<'s>> ToJS<'s> for Option<T> {
    fn to_js(self, lock: &mut Lock<'s>) -> Result<v8::Local<'s, v8::Value>, Error> {
        self.to_js_with_creator(lock, None)
    }

    fn to_js_with_creator(
        self,
        lock: &mut Lock<'s>,
        creator: Option<v8::Local<'s, v8::Object>>,
    ) -> Result<v8::Local<'s, v8::Value>, Error> {
        match self {
            Some(value) => value.to_js_with_creator(lock, creator),
            None => Ok(lock.undefined()),
        }
    }
}

impl<'s, T: Coercible + ToJS<'s>> ToJS<'s> for NonCoercible<T> {
    fn to_js(self, lock: &mut Lock<'s>) -> Result<v8::Local<'s, v8::Value>, Error> {
        self.to_js_with_creator(lock, None)
    }

    fn to_js_with_creator(
        self,
        lock: &mut Lock<'s>,
        creator: Option<v8::Local<'s, v8::Object>>,
    ) -> Result<v8::Local<'s, v8::Value>, Error> {
        self.into_inner().to_js_with_creator(lock, creator)
    }
}

impl<'s, T: ToJS<'s>> ToJS<'s> for Nullable<T> {
    fn to_js(self, lock: &mut Lock<'s>) -> Result<v8::Local<'s, v8::Value>, Error> {
        self.to_js_with_creator(lock, None)
    }

    fn to_js_with_creator(
        self,
        lock: &mut Lock<'s>,
        creator: Option<v8::Local<'s, v8::Object>>,
    ) -> Result<v8::Local<'s, v8::Value>, Error> {
        match self {
            Self::Some(value) => value.to_js_with_creator(lock, creator),
            Self::Null => Ok(lock.null()),
        }
    }
}

impl<'s, T: ToJS<'s>> ToJS<'s> for NullOrUndefined<T> {
    fn to_js(self, lock: &mut Lock<'s>) -> Result<v8::Local<'s, v8::Value>, Error> {
        self.to_js_with_creator(lock, None)
    }

    fn to_js_with_creator(
        self,
        lock: &mut Lock<'s>,
        creator: Option<v8::Local<'s, v8::Object>>,
    ) -> Result<v8::Local<'s, v8::Value>, Error> {
        match self {
            Self::Some(value) => value.to_js_with_creator(lock, creator),
            Self::Null => Ok(lock.null()),
            Self::Undefined => Ok(lock.undefined()),
        }
    }
}

impl<'s, T: ToJS<'s>> ToJS<'s> for Lenient<T> {
    fn to_js(self, lock: &mut Lock<'s>) -> Result<v8::Local<'s, v8::Value>, Error> {
        self.to_js_with_creator(lock, None)
    }

    fn to_js_with_creator(
        self,
        lock: &mut Lock<'s>,
        creator: Option<v8::Local<'s, v8::Object>>,
    ) -> Result<v8::Local<'s, v8::Value>, Error> {
        match self {
            Self::Some(value) => value.to_js_with_creator(lock, creator),
            Self::Null => Ok(lock.null()),
            Self::Undefined | Self::Unconvertable(_) => Ok(lock.undefined()),
        }
    }
}

// Like C++ `jsg::Optional<T>`, only `undefined` is absent; `null` goes to `T`.
impl<'s, T: FromJS<'s>> FromJS<'s> for Option<T> {
    type ResultType = Option<T::ResultType>;

    const OBJECT_CONVERSION_MAY_RUN_SCRIPT: bool = T::OBJECT_CONVERSION_MAY_RUN_SCRIPT;

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

impl<T: Coercible> Type for NonCoercible<T> {
    const UNION_TYPES: UnionTypes = T::UNION_TYPES;

    fn class_name() -> &'static str {
        T::class_name()
    }

    fn is_exact(value: v8::Local<'_, v8::Value>) -> bool {
        T::is_exact(value)
    }
}

// Like C++ `NonCoercible<T>` (jsg/web-idl.h), this is the same kind of union
// member as `T`, but converts only a value of exactly that type.
impl<'s, T: Coercible + FromJS<'s>> FromJS<'s> for NonCoercible<T> {
    type ResultType = NonCoercible<T::ResultType>;

    const UNION_MEMBER: UnionMember = T::UNION_MEMBER;
    // Converts only a value that `T::is_exact()` accepts, which is never an object.
    const OBJECT_CONVERSION_MAY_RUN_SCRIPT: bool = false;

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

// Like C++ JSG's `kj::Maybe<T>`: a union with it as a variant counts it as nullable, and as `T`.
impl<T: Type> Type for Nullable<T> {
    const UNION_TYPES: UnionTypes = T::UNION_TYPES.nullable();
    const UNION_NAMES: &'static [&'static str] = T::UNION_NAMES;

    fn class_name() -> &'static str {
        T::class_name()
    }

    fn is_exact(value: v8::Local<'_, v8::Value>) -> bool {
        value.is_null_or_undefined() || T::is_exact(value)
    }
}

// Like C++ `kj::Maybe<T>`, Web IDL nullable conversion reads `undefined` as `null`.
// Nullable arguments remain required; use `NullOrUndefined` to permit omission.
impl<'s, T: FromJS<'s>> FromJS<'s> for Nullable<T> {
    type ResultType = Nullable<T::ResultType>;

    const OBJECT_CONVERSION_MAY_RUN_SCRIPT: bool = T::OBJECT_CONVERSION_MAY_RUN_SCRIPT;

    fn type_name() -> String {
        T::type_name()
    }

    fn from_js(
        lock: &mut Lock<'s>,
        value: v8::Local<'s, v8::Value>,
    ) -> Result<Self::ResultType, Error> {
        if value.is_null_or_undefined() {
            Ok(Nullable::Null)
        } else {
            Ok(Nullable::Some(T::from_js(lock, value)?))
        }
    }

    fn try_from_js(
        lock: &mut Lock<'s>,
        value: v8::Local<'s, v8::Value>,
    ) -> Result<Option<Self::ResultType>, Error> {
        if value.is_null_or_undefined() {
            Ok(Some(Nullable::Null))
        } else {
            Ok(T::try_from_js(lock, value)?.map(Nullable::Some))
        }
    }
}

impl<'s, T: FromJS<'s>> FromJS<'s> for NullOrUndefined<T> {
    type ResultType = NullOrUndefined<T::ResultType>;

    const OBJECT_CONVERSION_MAY_RUN_SCRIPT: bool = T::OBJECT_CONVERSION_MAY_RUN_SCRIPT;

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
            Ok(NullOrUndefined::Null)
        } else if value.is_undefined() {
            Ok(NullOrUndefined::Undefined)
        } else {
            Ok(NullOrUndefined::Some(T::from_js(lock, value)?))
        }
    }

    fn try_from_js(
        lock: &mut Lock<'s>,
        value: v8::Local<'s, v8::Value>,
    ) -> Result<Option<Self::ResultType>, Error> {
        if value.is_null() {
            Ok(Some(NullOrUndefined::Null))
        } else if value.is_undefined() {
            Ok(Some(NullOrUndefined::Undefined))
        } else {
            Ok(T::try_from_js(lock, value)?.map(NullOrUndefined::Some))
        }
    }
}

impl<'s, T: FromJS<'s>> FromJS<'s> for Lenient<T> {
    type ResultType = Lenient<T::ResultType>;

    const OBJECT_CONVERSION_MAY_RUN_SCRIPT: bool = T::OBJECT_CONVERSION_MAY_RUN_SCRIPT;

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
    const UNION_TYPES: UnionTypes = UnionTypes::SEQUENCE_LIKE;

    fn class_name() -> &'static str {
        "Array"
    }

    fn is_exact(value: v8::Local<'_, v8::Value>) -> bool {
        value.is_array()
    }
}

impl<'s, T: ToJS<'s>> ToJS<'s> for Vec<T> {
    fn to_js(self, lock: &mut Lock<'s>) -> Result<v8::Local<'s, v8::Value>, Error> {
        self.to_js_with_creator(lock, None)
    }

    fn to_js_with_creator(
        self,
        lock: &mut Lock<'s>,
        creator: Option<v8::Local<'s, v8::Object>>,
    ) -> Result<v8::Local<'s, v8::Value>, Error> {
        // As C++ JSG does, this builds the array from its elements instead of setting them
        // one by one, so no setter a script put on Array.prototype runs. Converting stops at
        // the first element that fails.
        let elements = self
            .into_iter()
            .map(|item| item.to_js_with_creator(lock, creator))
            .collect::<Result<Vec<_>, _>>()?;
        Ok(v8::Array::with_elements(lock, &elements).into())
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
        Self::try_from_js(lock, value)?.ok_or_else(|| {
            Error::new_type_error(format!("Expected Array but got {}", value.type_of(lock)))
        })
    }

    fn try_from_js(
        lock: &mut Lock<'s>,
        value: v8::Local<'s, v8::Value>,
    ) -> Result<Option<Self::ResultType>, Error> {
        let Ok(array) = v8::Local::<v8::Array>::try_from(value) else {
            return Ok(None);
        };

        // Web IDL and C++ JSG convert each element before reading the next. The elements are read
        // ahead here instead, up to the first object if converting one can run script: only a
        // conversion that runs script can change what a later read returns. The difference shows
        // when a conversion throws, such as a Symbol's to a string: a getter on a later element
        // has already run, and if it threw, its exception is the one that propagates.
        let until = if T::OBJECT_CONVERSION_MAY_RUN_SCRIPT {
            v8::ArrayIterateUntil::Object
        } else {
            v8::ArrayIterateUntil::End
        };
        let v8::ArrayElements { length, elements } = array.iterate(lock, until)?;
        // Script sets the length, and a sparse array's can be up to 2^32 - 1 with no elements, so
        // a failure to allocate is an error, not an abort.
        let too_large = |length: usize| {
            Error::new_range_error(format!("Array of length {length} is too large to convert"))
        };
        let mut result = Self::new();
        result
            .try_reserve_exact(length)
            .map_err(|_| too_large(length))?;
        // If reading stopped at an object, it is the last element read.
        let stopped = until == v8::ArrayIterateUntil::Object
            && elements
                .last()
                .is_some_and(|(_, element)| element.is_object());
        for (index, element) in elements {
            while result.len() < index {
                let undefined = lock.undefined();
                result.push(T::from_js(lock, undefined)?);
            }
            result.push(T::from_js(lock, element)?);
        }
        if stopped {
            // The conversion can have changed the array, so the rest is read one element at a
            // time, each after the previous one converts.
            while result.len() < array.len() {
                let Ok(index) = u32::try_from(result.len()) else {
                    unreachable!("an array's length is a u32");
                };
                let element = array.get(lock, index)?;
                let element = T::from_js(lock, element)?;
                // A conversion can lengthen the array past the space reserved.
                result.try_reserve(1).map_err(|_| too_large(array.len()))?;
                result.push(element);
            }
        } else {
            while result.len() < length {
                let undefined = lock.undefined();
                result.push(T::from_js(lock, undefined)?);
            }
        }
        Ok(Some(result))
    }
}

// =============================================================================
// Record type implementations (Dict<V>)
// =============================================================================

// Like C++ JSG's DictWrapper (jsg/value.h): any object other than an array.
impl<V> Type for Dict<V> {
    const UNION_TYPES: UnionTypes = UnionTypes::RECORD;

    fn class_name() -> &'static str {
        "object"
    }

    fn is_exact(value: v8::Local<'_, v8::Value>) -> bool {
        value.is_object() && !value.is_array()
    }
}

impl<'s, V: ToJS<'s>> ToJS<'s> for Dict<V> {
    fn to_js(self, lock: &mut Lock<'s>) -> Result<v8::Local<'s, v8::Value>, Error> {
        self.to_js_with_creator(lock, None)
    }

    fn to_js_with_creator(
        self,
        lock: &mut Lock<'s>,
        creator: Option<v8::Local<'s, v8::Object>>,
    ) -> Result<v8::Local<'s, v8::Value>, Error> {
        let entries = self
            .into_iter()
            .map(|(key, value)| Ok((key, value.to_js_with_creator(lock, creator)?)))
            .collect::<Result<Vec<_>, Error>>()?;
        let properties: Vec<(&str, v8::Local<'s, v8::Value>)> = entries
            .iter()
            .map(|(key, value)| (key.as_str(), *value))
            .collect();
        // As Web IDL does for a record, defines each entry as an own data property, unlike C++
        // JSG's DictWrapper, which sets it: no setter on `Object.prototype` runs, and a
        // `__proto__` entry is an own property rather than replacing the prototype.
        Ok(v8::Object::with_data_properties(lock, &properties)?.into())
    }
}

impl<'s, V: FromJS<'s, ResultType = V>> FromJS<'s> for Dict<V> {
    type ResultType = Self;

    fn type_name() -> String {
        "object".to_owned()
    }

    fn from_js(
        lock: &mut Lock<'s>,
        value: v8::Local<'s, v8::Value>,
    ) -> Result<Self::ResultType, Error> {
        if let Some(dict) = Self::try_from_js(lock, value)? {
            return Ok(dict);
        }
        let type_name = if value.is_array() {
            "array".to_owned()
        } else {
            value.type_of(lock)
        };
        Err(Error::new_type_error(format!(
            "Expected object but got {type_name}"
        )))
    }

    // As C++ JSG's DictWrapper does, reads and converts the values one at a time, and reports
    // a value that is not a `V` as the value of its entry, while an error from converting one
    // passes through.
    fn try_from_js(
        lock: &mut Lock<'s>,
        value: v8::Local<'s, v8::Value>,
    ) -> Result<Option<Self::ResultType>, Error> {
        let Ok(object) = v8::Local::<v8::Object>::try_from(value) else {
            return Ok(None);
        };
        if value.is_array() {
            return Ok(None);
        }
        let keys = object.keys(lock)?;
        let mut entries = Vec::with_capacity(keys.len());
        for key in keys {
            let value = object.get_by_name(lock, key.into())?;
            // Like a string argument: the name is a string, so this runs no JavaScript.
            let name = String::from_js(lock, key.into())?;
            let Some(value) = V::try_from_js(lock, value)? else {
                return Err(Error::new_type_error(format!(
                    "Incorrect type for map entry '{name}': the provided value is not of type \
                     '{}'.",
                    V::type_name()
                )));
            };
            entries.push((name, value));
        }
        Ok(Some(Self::from(entries)))
    }
}

// =============================================================================
// TypedArray implementations
// =============================================================================
//
// The conversions of typed arrays are those of their handles, `Local<'s, T>`, in `v8.rs`. The
// marker types implement `Type`, which names their JavaScript class.

macro_rules! impl_typed_array_type {
    ($($marker:ident => $is_check:ident),* $(,)?) => {
        $(
            impl Type for v8::$marker {
                const UNION_NAMES: &'static [&'static str] = &[concat!("v8::", stringify!($marker))];

                fn class_name() -> &'static str {
                    stringify!($marker)
                }
                fn is_exact(value: v8::Local<'_, v8::Value>) -> bool {
                    value.$is_check()
                }
            }
        )*
    };
}

impl_typed_array_type!(
    Uint8Array => is_uint8_array,
    Uint8ClampedArray => is_uint8clamped_array,
    Uint16Array => is_uint16_array,
    Uint32Array => is_uint32_array,
    Int8Array => is_int8_array,
    Int16Array => is_int16_array,
    Int32Array => is_int32_array,
    Float32Array => is_float32_array,
    Float64Array => is_float64_array,
    BigInt64Array => is_bigint64_array,
    BigUint64Array => is_biguint64_array,
);

// =============================================================================
// ArrayBuffer, ArrayBufferView, SharedArrayBuffer
// =============================================================================
//
// These markers implement `Type` so that their handles can be `#[jsg_oneof]` variants; their
// conversions are those of their handles, in `v8.rs`.

impl Type for v8::ArrayBuffer {
    const UNION_NAMES: &'static [&'static str] = &["v8::ArrayBuffer"];

    fn class_name() -> &'static str {
        "ArrayBuffer"
    }
    fn is_exact(value: v8::Local<'_, v8::Value>) -> bool {
        value.is_array_buffer()
    }
}

impl Type for v8::ArrayBufferView {
    const UNION_NAMES: &'static [&'static str] = &["v8::ArrayBufferView"];

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
    fn to_js(self, _lock: &mut Lock<'s>) -> Result<v8::Local<'s, v8::Value>, Error> {
        Ok(self.into())
    }
}

impl<'s> FromJS<'s> for v8::Local<'s, v8::Value> {
    type ResultType = Self;

    const OBJECT_CONVERSION_MAY_RUN_SCRIPT: bool = false;

    fn type_name() -> String {
        // C++ JSG names `v8::Local<v8::Value>` after `v8::Value`.
        "Value".to_owned()
    }

    fn from_js(_lock: &mut Lock<'s>, value: Self) -> Result<Self, Error> {
        Ok(value)
    }
}

// A handle is the type its marker is, as C++ JSG's `v8::Local<T>` is.
impl<M: Type> Type for v8::Local<'_, M> {
    const UNION_TYPES: UnionTypes = M::UNION_TYPES;
    const UNION_NAMES: &'static [&'static str] = M::UNION_NAMES;

    fn class_name() -> &'static str {
        M::class_name()
    }

    fn is_exact(value: v8::Local<'_, v8::Value>) -> bool {
        M::is_exact(value)
    }
}

/// Implements `Type` for handle markers. Their Web IDL categories are those that C++ JSG's
/// `webidl` traits (jsg/web-idl.h) give the corresponding `v8::Local<T>`.
macro_rules! impl_marker_type {
    ($($marker:ident: $name:literal, $is_exact:ident, $types:expr;)*) => {
        $(impl Type for v8::$marker {
            const UNION_TYPES: UnionTypes = $types;
            const UNION_NAMES: &'static [&'static str] = &[concat!("v8::", stringify!($marker))];

            fn class_name() -> &'static str {
                $name
            }

            fn is_exact(value: v8::Local<'_, v8::Value>) -> bool {
                value.$is_exact()
            }
        })*
    };
}

impl_marker_type! {
    String: "String", is_string, UnionTypes::STRING;
    BigInt: "BigInt", is_big_int, UnionTypes::NUMERIC;
    Object: "Object", is_object, UnionTypes::OBJECT;
    Function: "Function", is_function, UnionTypes::OTHER;
    Promise: "Promise", is_promise, UnionTypes::OTHER;
    TypedArray: "TypedArray", is_typed_array, UnionTypes::OTHER;
}

// Any value is a `v8::Value`.
impl Type for v8::Value {
    const UNION_NAMES: &'static [&'static str] = &["v8::Value"];

    fn class_name() -> &'static str {
        "Value"
    }

    fn is_exact(_value: v8::Local<'_, v8::Value>) -> bool {
        true
    }
}

/// Implements `FromJS` for handles of markers that C++ JSG converts a value to without coercion
/// (`V8HandleWrapper` in jsg/type-wrapper.h): a value of the marker's type, and nothing else.
macro_rules! impl_handle_from_js {
    ($($marker:ident: $member:expr;)*) => {
        $(impl<'s> FromJS<'s> for v8::Local<'s, v8::$marker> {
            type ResultType = Self;

            const UNION_MEMBER: UnionMember = $member;
            const OBJECT_CONVERSION_MAY_RUN_SCRIPT: bool = false;

            fn type_name() -> String {
                // C++ JSG names `v8::Local<T>` after `T`.
                <v8::$marker as Type>::class_name().to_owned()
            }

            fn from_js(lock: &mut Lock<'s>, value: v8::Local<'s, v8::Value>) -> Result<Self, Error> {
                let type_name = value.type_of(lock);
                Self::try_from_js(lock, value)?.ok_or_else(|| {
                    Error::new_type_error(format!(
                        "Expected {} but got {type_name}",
                        <Self as FromJS<'s>>::type_name()
                    ))
                })
            }

            fn try_from_js(
                _lock: &mut Lock<'s>,
                value: v8::Local<'s, v8::Value>,
            ) -> Result<Option<Self>, Error> {
                Ok(Self::try_from(value).ok())
            }
        })*
    };
}

impl_handle_from_js! {
    String: UnionMember::String;
    BigInt: UnionMember::Numeric;
    Object: UnionMember::Fallible;
    Function: UnionMember::Fallible;
    TypedArray: UnionMember::Fallible;
}

// =============================================================================
// Persistent handles
// =============================================================================

// A `TracedGlobal<M>` is C++ JSG's `jsg::JsRef<T>` / `jsg::V8Ref<T>`: it converts as a
// `Local<'s, M>` does, and outlives the scope, so it can be a field of a `#[jsg_struct]` that
// a resource keeps or a typed promise carries.

impl<M: Type> Type for v8::TracedGlobal<M> {
    const UNION_TYPES: UnionTypes = M::UNION_TYPES;
    const UNION_NAMES: &'static [&'static str] = M::UNION_NAMES;

    fn class_name() -> &'static str {
        M::class_name()
    }

    fn is_exact(value: v8::Local<'_, v8::Value>) -> bool {
        M::is_exact(value)
    }
}

impl<'s, M> ToJS<'s> for v8::TracedGlobal<M>
where
    v8::Local<'s, M>: Into<v8::Local<'s, v8::Value>>,
{
    fn to_js(self, lock: &mut Lock<'s>) -> Result<v8::Local<'s, v8::Value>, Error> {
        Ok(self.as_local(lock).into())
    }
}

impl<'s, M> FromJS<'s> for v8::TracedGlobal<M>
where
    v8::Local<'s, M>: FromJS<'s, ResultType = v8::Local<'s, M>>,
{
    type ResultType = Self;

    const UNION_MEMBER: UnionMember = <v8::Local<'s, M> as FromJS<'s>>::UNION_MEMBER;
    const OBJECT_CONVERSION_MAY_RUN_SCRIPT: bool =
        <v8::Local<'s, M> as FromJS<'s>>::OBJECT_CONVERSION_MAY_RUN_SCRIPT;

    fn type_name() -> String {
        <v8::Local<'s, M> as FromJS<'s>>::type_name()
    }

    fn from_js(lock: &mut Lock<'s>, value: v8::Local<'s, v8::Value>) -> Result<Self, Error> {
        let local = <v8::Local<'s, M> as FromJS<'s>>::from_js(lock, value)?;
        Ok(Self::new(lock, local))
    }

    fn try_from_js(
        lock: &mut Lock<'s>,
        value: v8::Local<'s, v8::Value>,
    ) -> Result<Option<Self>, Error> {
        Ok(<v8::Local<'s, M> as FromJS<'s>>::try_from_js(lock, value)?
            .map(|local| Self::new(lock, local)))
    }
}

#[cfg(test)]
#[path = "wrappable-test.rs"]
mod tests;
