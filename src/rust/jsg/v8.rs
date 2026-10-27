// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! V8 JavaScript engine bindings and garbage collector integration.
//!
//! This module provides Rust wrappers for V8 types and integration with the C++ `Wrappable`
//! garbage collection system used by workerd.
//!
//! # Core Types
//!
//! - [`IsolatePtr`] - Non-null wrapper around `v8::Isolate*`; callers must still
//!   ensure the isolate is alive and the current thread holds the isolate lock
//! - [`Local<'a, T>`] - Stack-allocated handle to a V8 value, tied to a `HandleScope`
//! - [`Global<T>`] - Persistent handle that outlives `HandleScope`s; GC never visits it
//! - [`TracedGlobal<T>`] - Persistent handle for a resource field, which GC traces so that a
//!   cycle through it can be collected
//! - [`BackingStore`] - Owned handle to the raw memory backing an `ArrayBuffer`;
//!   keeps the memory alive independently of any JS handle
//!
//! # Garbage Collection
//!
//! Rust resources integrate with V8's GC through the C++ `Wrappable` base class. Each Rust
//! resource is wrapped in `Rc<R>` with a `Wrappable` on the KJ heap that bridges to cppgc
//! via a `CppgcShim`. The Wrappable's `data[0..1]` stores a fat pointer to
//! `dyn GarbageCollected` — the data part is the `Rc::into_raw` pointer (pointing to `R`
//! inside the `Rc` allocation) and the vtable part carries `R`'s `GarbageCollected` impl.
//! On destruction, `wrappable_invoke_drop` reconstructs the `Rc` via `Rc::from_raw` and
//! drops it, which may drop the resource. The Rust `jsg::Rc<R>` smart pointer holds its own
//! `std::Rc<R>` plus a `WrappableRc` (`KjRc<Wrappable>`) for reference-counted ownership. On
//! drop, `wrappable_remove_strong_ref()` handles GC cleanup via `maybeDeferDestruction()`,
//! then the `WrappableRc` drop decrements the `kj::Rc` refcount. A function created by
//! [`Function::new`] owns its data the same way, through a Wrappable whose opaque wrapper is the
//! function's V8 data.

#![allow(
    unsafe_code,
    reason = "wraps raw V8 handles and the FFI that operates on them"
)]

use std::any::TypeId;
use std::cell::UnsafeCell;
use std::marker::PhantomData;
use std::mem::ManuallyDrop;
use std::num::NonZeroUsize;
use std::pin::Pin;
use std::ptr::NonNull;
use std::rc::Rc;
use std::sync::Arc;

pub use self::ffi::PromiseState;
use crate::Error;
use crate::FromJS;
use crate::GarbageCollected;
use crate::Lock;
use crate::Resource;
use crate::Traced;
use crate::UnitType;
use crate::isolate_state::IsolateState;
pub use crate::promise::Promise;
pub use crate::promise::PromiseResolver;
#[expect(
    clippy::missing_safety_doc,
    reason = "bridge declarations of raw V8 FFI functions; their contracts are V8's own"
)]
#[cxx::bridge(namespace = "workerd::rust::jsg")]
pub mod ffi {
    #[derive(Debug, Clone, Copy)]
    struct Local {
        ptr: usize,
    }

    /// Mirrors `v8::MaybeLocal<T>`: a single pointer-sized word where `ptr == 0` means empty.
    /// This matches V8's internal layout exactly — `v8::MaybeLocal<T>` holds one `Local<T>`
    /// field which is itself one `internal::Address*` (one pointer word).
    ///
    /// Rust code uses `Option<Local<'s, T>>`, which has the same layout. This struct exists only
    /// because CXX bridge signatures cannot name `Option<Local>`; convert with
    /// [`Local::from_ffi_maybe`](super::Local::from_ffi_maybe) and
    /// [`Local::into_ffi_maybe`](super::Local::into_ffi_maybe).
    #[derive(Debug, Clone, Copy, Default)]
    struct MaybeLocal {
        ptr: usize,
    }

    #[derive(Debug)]
    struct Global {
        /// Strong `v8::Global<v8::Value>` handle. Always valid when non-zero.
        ptr: usize,
    }

    #[derive(Debug)]
    struct TracedReference {
        /// Weak `v8::TracedReference<v8::Data>` handle used during GC tracing.
        /// Zero/null when inactive (strong mode).
        ptr: usize,
    }

    #[derive(Debug)]
    struct GcVisitor {
        ptr: usize,
    }

    #[derive(Debug)]
    struct Utf8Value {
        ptr: usize,
    }

    /// Mirrors `v8::BackingStoreInitializationMode`. Must be kept in sync with
    /// the V8-defined enum in `v8-array-buffer.h`.
    #[derive(Debug, PartialEq, Eq, Copy, Clone)]
    pub enum BackingStoreInitializationMode {
        ZeroInitialized = 0,
        Uninitialized = 1,
    }

    #[derive(Debug, PartialEq, Eq, Copy, Clone)]
    pub enum ExceptionType {
        OperationError,
        DataError,
        DataCloneError,
        InvalidAccessError,
        InvalidStateError,
        InvalidCharacterError,
        NotSupportedError,
        SyntaxError,
        TimeoutError,
        TypeMismatchError,
        AbortError,
        NotFoundError,
        TypeError,
        Error,
        RangeError,
        ReferenceError,
    }

    /// Mirrors `v8::Promise::PromiseState`; `ffi.c++` asserts the values match.
    #[derive(Debug, PartialEq, Eq, Copy, Clone)]
    pub enum PromiseState {
        Pending = 0,
        Fulfilled = 1,
        Rejected = 2,
    }

    extern "Rust" {
        /// Called from C++ Wrappable destructor to drop the Rust object.
        /// Reconstructs the `Rc<dyn GarbageCollected>` from `data[0..1]` and drops it.
        unsafe fn wrappable_invoke_drop(wrappable: Pin<&mut Wrappable>);

        /// Called from C++ Wrappable::jsgVisitForGc to trace nested handles.
        /// Reconstructs `&dyn GarbageCollected` from `data[0..1]` and calls `trace()`.
        unsafe fn wrappable_invoke_trace(wrappable: &Wrappable, visitor: *mut GcVisitor);

        /// Called from C++ `Wrappable::jsgGetMemoryName`.
        /// Returns the NUL-terminated class name for heap snapshots as a `rust::Str` view
        /// into a `'static` string literal. The C++ side constructs a `kj::StringPtr`
        /// directly from `.data()` / `.size()` — no allocation needed.
        unsafe fn wrappable_invoke_get_name(wrappable: &Wrappable) -> &'static str;
    }

    unsafe extern "C++" {
        include!("workerd/rust/jsg/ffi.h");

        type Isolate;
        type FunctionCallbackInfo;
        type FastApiCallbackOptions;
        type PropertyCallbackInfo;
        type PropertySetterCallbackInfo;
        type PropertyQueryCallbackInfo;
        type Wrappable;

        // BackingStore
        pub unsafe fn backing_store_drop(store: usize);
        pub unsafe fn backing_store_data(store: usize) -> *mut u8;
        pub unsafe fn backing_store_byte_length(store: usize) -> usize;
        pub unsafe fn backing_store_max_byte_length(store: usize) -> usize;
        pub unsafe fn backing_store_is_shared(store: usize) -> bool;
        pub unsafe fn backing_store_is_resizable_by_user_javascript(store: usize) -> bool;
        pub unsafe fn backing_store_new_resizable(
            byte_length: usize,
            max_byte_length: usize,
        ) -> usize;

        // Local<T>
        pub unsafe fn local_to_global(isolate: *mut Isolate, value: Local) -> Global;
        pub unsafe fn local_new_number(isolate: *mut Isolate, value: f64) -> Local;
        pub unsafe fn local_new_string(isolate: *mut Isolate, value: &str) -> Local;
        pub unsafe fn local_new_boolean(isolate: *mut Isolate, value: bool) -> Local;
        pub unsafe fn local_new_object(isolate: *mut Isolate) -> Local;
        pub unsafe fn local_new_null(isolate: *mut Isolate) -> Local;
        pub unsafe fn local_new_undefined(isolate: *mut Isolate) -> Local;
        pub unsafe fn local_new_array(isolate: *mut Isolate, length: usize) -> Local;
        pub unsafe fn local_new_uint8_array(
            isolate: *mut Isolate,
            data: *const u8,
            length: usize,
        ) -> Local;
        pub unsafe fn local_new_uint16_array(
            isolate: *mut Isolate,
            data: *const u16,
            length: usize,
        ) -> Local;
        pub unsafe fn local_new_uint32_array(
            isolate: *mut Isolate,
            data: *const u32,
            length: usize,
        ) -> Local;
        pub unsafe fn local_new_int8_array(
            isolate: *mut Isolate,
            data: *const i8,
            length: usize,
        ) -> Local;
        pub unsafe fn local_new_int16_array(
            isolate: *mut Isolate,
            data: *const i16,
            length: usize,
        ) -> Local;
        pub unsafe fn local_new_int32_array(
            isolate: *mut Isolate,
            data: *const i32,
            length: usize,
        ) -> Local;
        pub unsafe fn local_new_float32_array(
            isolate: *mut Isolate,
            data: *const f32,
            length: usize,
        ) -> Local;
        pub unsafe fn local_new_float64_array(
            isolate: *mut Isolate,
            data: *const f64,
            length: usize,
        ) -> Local;
        pub unsafe fn local_new_bigint64_array(
            isolate: *mut Isolate,
            data: *const i64,
            length: usize,
        ) -> Local;
        pub unsafe fn local_new_biguint64_array(
            isolate: *mut Isolate,
            data: *const u64,
            length: usize,
        ) -> Local;
        pub unsafe fn local_new_array_buffer(
            isolate: *mut Isolate,
            data: *const u8,
            length: usize,
        ) -> Local;
        pub unsafe fn local_new_array_buffer_empty(
            isolate: *mut Isolate,
            byte_length: usize,
        ) -> Local;
        pub unsafe fn array_buffer_new_with_mode(
            isolate: *mut Isolate,
            byte_length: usize,
            mode: BackingStoreInitializationMode,
        ) -> MaybeLocal;
        pub unsafe fn array_buffer_from_backing_store(isolate: *mut Isolate, store: usize)
        -> Local;
        pub unsafe fn local_array_buffer_byte_length(buffer: &Local) -> usize;
        pub unsafe fn local_array_buffer_data(buffer: &Local) -> *mut u8;
        pub unsafe fn local_array_buffer_get_backing_store(buffer: &Local) -> usize;

        // Local<ArrayBufferView>
        pub unsafe fn local_array_buffer_view_byte_offset(view: &Local) -> usize;
        pub unsafe fn local_array_buffer_view_byte_length(view: &Local) -> usize;
        pub unsafe fn local_array_buffer_view_buffer_data(view: &Local) -> *mut u8;
        pub unsafe fn local_array_buffer_view_get_buffer(
            isolate: *mut Isolate,
            view: &Local,
        ) -> Local;
        pub unsafe fn local_array_buffer_view_element_size(view: &Local) -> usize;
        pub unsafe fn local_array_buffer_view_is_integer_type(view: &Local) -> bool;

        pub unsafe fn local_eq(lhs: &Local, rhs: &Local) -> bool;
        /// Whether `value` can be used in `isolate`: Smis in any isolate, values in read-only or
        /// writable shared space in any isolate of their isolate group, and other values only in
        /// the isolate whose heap holds them. Reads only the value's page header.
        pub unsafe fn local_is_compatible_with_isolate(
            isolate: *mut Isolate,
            value: &Local,
        ) -> bool;
        pub unsafe fn local_is_string(value: &Local) -> bool;
        pub unsafe fn local_is_boolean(value: &Local) -> bool;
        pub unsafe fn local_is_number(value: &Local) -> bool;
        pub unsafe fn local_is_null(value: &Local) -> bool;
        pub unsafe fn local_is_undefined(value: &Local) -> bool;
        pub unsafe fn local_is_null_or_undefined(value: &Local) -> bool;
        pub unsafe fn local_is_object(value: &Local) -> bool;
        pub unsafe fn local_is_native_error(value: &Local) -> bool;
        pub unsafe fn local_is_array(value: &Local) -> bool;
        pub unsafe fn local_is_uint8_array(value: &Local) -> bool;
        pub unsafe fn local_is_uint16_array(value: &Local) -> bool;
        pub unsafe fn local_is_uint32_array(value: &Local) -> bool;
        pub unsafe fn local_is_int8_array(value: &Local) -> bool;
        pub unsafe fn local_is_int16_array(value: &Local) -> bool;
        pub unsafe fn local_is_int32_array(value: &Local) -> bool;
        pub unsafe fn local_is_float32_array(value: &Local) -> bool;
        pub unsafe fn local_is_float64_array(value: &Local) -> bool;
        pub unsafe fn local_is_bigint64_array(value: &Local) -> bool;
        pub unsafe fn local_is_biguint64_array(value: &Local) -> bool;
        pub unsafe fn local_is_float16_array(value: &Local) -> bool;
        pub unsafe fn local_is_uint8clamped_array(value: &Local) -> bool;
        pub unsafe fn local_is_typed_array(value: &Local) -> bool;
        pub unsafe fn local_is_array_buffer(value: &Local) -> bool;
        pub unsafe fn local_is_array_buffer_view(value: &Local) -> bool;
        pub unsafe fn local_is_shared_array_buffer(value: &Local) -> bool;
        pub unsafe fn local_is_function(value: &Local) -> bool;
        pub unsafe fn local_is_promise(value: &Local) -> bool;
        pub unsafe fn local_is_symbol(value: &Local) -> bool;
        pub unsafe fn local_is_name(value: &Local) -> bool;
        pub unsafe fn local_is_big_int(value: &Local) -> bool;
        pub unsafe fn local_is_date(value: &Local) -> bool;
        pub unsafe fn local_type_of(isolate: *mut Isolate, value: &Local) -> String;

        // Local<String>
        pub unsafe fn local_string_length(value: &Local) -> i32;
        pub unsafe fn local_string_is_one_byte(value: &Local) -> bool;
        pub unsafe fn local_string_contains_only_one_byte(value: &Local) -> bool;
        pub unsafe fn local_string_utf8_length(isolate: *mut Isolate, value: &Local) -> usize;
        pub unsafe fn local_string_write_v2(
            isolate: *mut Isolate,
            value: &Local,
            offset: u32,
            length: u32,
            buffer: *mut u16,
            flags: i32,
        );
        pub unsafe fn local_string_write_one_byte_v2(
            isolate: *mut Isolate,
            value: &Local,
            offset: u32,
            length: u32,
            buffer: *mut u8,
            flags: i32,
        );
        pub unsafe fn local_string_write_utf8_v2(
            isolate: *mut Isolate,
            value: &Local,
            buffer: *mut u8,
            capacity: usize,
            flags: i32,
        ) -> usize;
        pub unsafe fn local_string_empty(isolate: *mut Isolate) -> Local;
        pub unsafe fn local_string_equals(lhs: &Local, rhs: &Local) -> bool;
        pub unsafe fn local_string_is_flat(value: &Local) -> bool;
        pub unsafe fn local_string_concat(
            isolate: *mut Isolate,
            left: Local,
            right: Local,
        ) -> MaybeLocal;
        pub unsafe fn local_string_internalize(isolate: *mut Isolate, value: &Local) -> Local;
        pub unsafe fn local_string_new_from_utf8(
            isolate: *mut Isolate,
            data: *const u8,
            length: i32,
            internalized: bool,
        ) -> MaybeLocal;
        pub unsafe fn local_string_new_from_one_byte(
            isolate: *mut Isolate,
            data: *const u8,
            length: i32,
            internalized: bool,
        ) -> MaybeLocal;
        pub unsafe fn local_string_new_from_two_byte(
            isolate: *mut Isolate,
            data: *const u16,
            length: i32,
            internalized: bool,
        ) -> MaybeLocal;

        // Local<Name>
        pub unsafe fn local_name_get_identity_hash(value: &Local) -> i32;

        // Local<Symbol>
        pub unsafe fn local_symbol_new(isolate: *mut Isolate) -> Local;
        pub unsafe fn local_symbol_new_with_description(
            isolate: *mut Isolate,
            description: Local,
        ) -> Local;
        pub unsafe fn local_symbol_description(isolate: *mut Isolate, value: &Local) -> MaybeLocal;

        // Local<Boolean>
        pub unsafe fn local_boolean_value(value: &Local) -> bool;

        // Local<Number>
        pub unsafe fn local_number_value(value: &Local) -> f64;

        // Local<BigInt>
        pub unsafe fn big_int_new(isolate: *mut Isolate, value: i64) -> Local;
        pub unsafe fn big_int_new_from_unsigned(isolate: *mut Isolate, value: u64) -> Local;
        pub unsafe fn local_big_int_int64_value(value: &Local, lossless: &mut bool) -> i64;
        pub unsafe fn local_big_int_uint64_value(value: &Local, lossless: &mut bool) -> u64;

        // Local<Date>
        pub unsafe fn date_new(isolate: *mut Isolate, time: f64) -> MaybeLocal;
        pub unsafe fn local_date_value_of(value: &Local) -> f64;

        // Local<Function>
        pub unsafe fn local_function_call(
            isolate: *mut Isolate,
            function: &Local,
            recv: &Local,
            args: &[Local],
        ) -> MaybeLocal;
        pub unsafe fn function_new(
            isolate: *mut Isolate,
            callback: usize,
            data: &Local,
        ) -> MaybeLocal;

        // Local<Object>
        // Fallible: interning a key fails if it is longer than V8's string length limit or V8
        // fails to create the string, and there must be as many keys as values.
        pub unsafe fn object_new_with_data_properties(
            isolate: *mut Isolate,
            keys: &[&str],
            values: &[Local],
        ) -> Result<Local>;
        // Leave what a setter, getter or Proxy trap throws pending; see `call_into_js()`.
        // Fallible: interning the key fails if it is longer than V8's string length limit or V8
        // fails to create the string.
        pub unsafe fn local_object_set_property(
            isolate: *mut Isolate,
            object: &mut Local,
            key: &str,
            value: Local,
        ) -> Result<bool>;
        pub unsafe fn local_object_has_property(
            isolate: *mut Isolate,
            object: &Local,
            key: &str,
            has: &mut bool,
        ) -> Result<bool>;
        pub unsafe fn local_object_get_property(
            isolate: *mut Isolate,
            object: &Local,
            key: &str,
        ) -> Result<MaybeLocal>;

        // Local<Promise>
        pub unsafe fn local_promise_state(promise: &Local) -> PromiseState;
        pub unsafe fn local_promise_result(promise: &Local) -> Local;
        pub unsafe fn local_promise_mark_as_handled(promise: &Local);
        pub unsafe fn local_promise_then(
            isolate: *mut Isolate,
            promise: &Local,
            on_fulfilled: &Local,
        ) -> MaybeLocal;
        pub unsafe fn local_promise_catch(
            isolate: *mut Isolate,
            promise: &Local,
            on_rejected: &Local,
        ) -> MaybeLocal;
        pub unsafe fn local_promise_then_or_catch(
            isolate: *mut Isolate,
            promise: &Local,
            on_fulfilled: &Local,
            on_rejected: &Local,
        ) -> MaybeLocal;

        // Local<PromiseResolver>
        pub unsafe fn promise_resolver_new(isolate: *mut Isolate) -> MaybeLocal;
        pub unsafe fn local_promise_resolver_promise(resolver: &Local) -> Local;
        pub unsafe fn local_promise_resolver_resolve(
            isolate: *mut Isolate,
            resolver: &Local,
            value: &Local,
        ) -> bool;
        pub unsafe fn local_promise_resolver_reject(
            isolate: *mut Isolate,
            resolver: &Local,
            reason: &Local,
        ) -> bool;

        // Local<Array>
        pub unsafe fn local_array_length(array: &Local) -> u32;
        pub unsafe fn array_new_with_elements(isolate: *mut Isolate, elements: &[Local]) -> Local;
        // Leaves what a setter throws pending; see `call_into_js()`.
        pub unsafe fn local_array_set(
            isolate: *mut Isolate,
            array: &mut Local,
            index: u32,
            value: Local,
        ) -> bool;
        // Leaves what a getter or Proxy trap on an element throws pending; see
        // `call_into_js()`. Fallible: KJ_REQUIREs the value is an array.
        pub unsafe fn local_array_iterate(
            isolate: *mut Isolate,
            value: Local,
            result: &mut Vec<Global>,
        ) -> Result<bool>;

        // Local<TypedArray>
        // Fallible: KJ_REQUIREs the view is within the buffer's bounds.
        pub unsafe fn uint8_array_from_buffer(
            isolate: *mut Isolate,
            buffer: &Local,
            byte_offset: usize,
            length: usize,
        ) -> Result<Local>;
        pub unsafe fn local_typed_array_length(array: &Local) -> usize;
        pub unsafe fn local_typed_array_buffer_data(array: &Local) -> usize;
        pub unsafe fn local_typed_array_byte_offset(array: &Local) -> usize;
        pub unsafe fn local_typed_array_byte_length(array: &Local) -> usize;
        pub unsafe fn local_uint8_array_get(array: &Local, index: usize) -> u8;
        pub unsafe fn local_uint16_array_get(array: &Local, index: usize) -> u16;
        pub unsafe fn local_uint32_array_get(array: &Local, index: usize) -> u32;
        pub unsafe fn local_int8_array_get(array: &Local, index: usize) -> i8;
        pub unsafe fn local_int16_array_get(array: &Local, index: usize) -> i16;
        pub unsafe fn local_int32_array_get(array: &Local, index: usize) -> i32;
        pub unsafe fn local_float32_array_get(array: &Local, index: usize) -> f32;
        pub unsafe fn local_float64_array_get(array: &Local, index: usize) -> f64;
        pub unsafe fn local_bigint64_array_get(array: &Local, index: usize) -> i64;
        pub unsafe fn local_biguint64_array_get(array: &Local, index: usize) -> u64;
        pub unsafe fn local_uint8clamped_array_get(array: &Local, index: usize) -> u8;

        // Global<T>
        pub unsafe fn global_reset(value: Pin<&mut Global>);
        pub unsafe fn global_clone(isolate: *mut Isolate, value: &Global) -> Global;
        pub unsafe fn global_to_local(isolate: *mut Isolate, value: &Global) -> Local;

        // Wrappable - data access
        pub unsafe fn wrappable_get_trait_object<'a>(
            wrappable: &'a Wrappable,
        ) -> &'a TraitObjectPtr;
        pub unsafe fn wrappable_clear_trait_object(wrappable: Pin<&mut Wrappable>);
        pub unsafe fn wrappable_strong_refcount(wrappable: &Wrappable) -> u32;

        // Wrappable lifecycle — KjRc<Wrappable> for reference-counted ownership
        pub unsafe fn wrappable_new(ptr: TraitObjectPtr) -> KjRc<Wrappable>;

        pub unsafe fn wrappable_to_rc(wrappable: Pin<&mut Wrappable>) -> KjRc<Wrappable>;
        pub unsafe fn wrappable_add_strong_ref(wrappable: Pin<&mut Wrappable>);
        pub unsafe fn wrappable_remove_strong_ref(wrappable: Pin<&mut Wrappable>, is_strong: bool);
        pub unsafe fn wrappable_visit_ref(
            wrappable: Pin<&mut Wrappable>,
            ref_parent: *mut usize,
            ref_strong: *mut bool,
            visitor: *mut GcVisitor,
        );
        /// Visit a `v8::Global` field during GC tracing, implementing the same
        /// strong↔traced dual-mode switching that `jsg::Data` / `jsg::V8Ref<T>`
        /// use in C++.
        ///
        /// `global` points to the `ptr` field of `ffi::Global` (the strong handle).
        /// `traced` points to the `traced_ptr` field (the weak traced handle).
        /// Both are mutated in-place to reflect the new handle state after the visit.
        pub unsafe fn wrappable_visit_global(
            visitor: *mut GcVisitor,
            global: *mut usize,
            traced: &mut TracedReference,
        );
        /// The isolate of the object that `visitor` is visiting: the one its wrapper was
        /// attached in, or that of the object whose trace reached it. Null if neither has
        /// happened yet, or the isolate is gone; a visit made by a garbage collection always
        /// has one.
        pub unsafe fn gc_visitor_v8_isolate(visitor: *mut GcVisitor) -> *mut Isolate;
        /// Makes a weak `v8::Global` strong again (`ClearWeak`), if it is not empty. `global`
        /// points to the `ptr` field of the `ffi::Global`.
        pub unsafe fn global_clear_weak(global: *mut usize);

        // Unwrappers
        //
        // Shims that can throw a C++ exception (building rust::String/Vec from V8
        // data, or KJ_REQUIRE) MUST return Result<T>: workerd-cxx then catches the
        // throw in C++ instead of aborting across the nounwind FFI frame. Shims that
        // provably can't throw stay bare (see unwrap_boolean, unwrap_number).
        //
        // unwrap_string and unwrap_number leave what a coercion throws pending; see
        // `call_into_js()`.
        pub unsafe fn unwrap_string(
            isolate: *mut Isolate,
            value: Local,
            result: &mut String,
        ) -> Result<bool>;
        // Infallible: ToBoolean can't throw and builds no rust::String/Vec.
        pub unsafe fn unwrap_boolean(isolate: *mut Isolate, value: Local) -> bool;
        // Infallible: NumberValue reports what JavaScript threw in its result.
        pub unsafe fn unwrap_number(isolate: *mut Isolate, value: Local, result: &mut f64) -> bool;

        // ArrayBuffer detach/detachable/was-detached
        pub unsafe fn local_array_buffer_detach(isolate: *mut Isolate, buffer: &mut Local);
        pub unsafe fn local_array_buffer_was_detached(buffer: &Local) -> bool;
        pub unsafe fn local_array_buffer_is_detachable(buffer: &Local) -> bool;

        // Value-level shared check
        pub fn local_array_buffer_is_shared(value: &Local) -> bool;

        // FunctionCallbackInfo
        pub unsafe fn fci_get_isolate(args: *mut FunctionCallbackInfo) -> *mut Isolate;
        pub unsafe fn fci_get_this(args: *mut FunctionCallbackInfo) -> Local;
        pub unsafe fn fci_get_length(args: *mut FunctionCallbackInfo) -> usize;
        pub unsafe fn fci_is_construct_call(args: *mut FunctionCallbackInfo) -> bool;
        pub unsafe fn fci_get_arg(args: *mut FunctionCallbackInfo, index: usize) -> Local;
        pub unsafe fn fci_set_return_value(args: *mut FunctionCallbackInfo, value: Local);
        pub unsafe fn fci_get_data_wrappable(
            args: *mut FunctionCallbackInfo,
        ) -> KjMaybe<KjRc<Wrappable>>;

        // PropertyCallbackInfo, for native data properties
        pub unsafe fn pci_get_isolate(info: *mut PropertyCallbackInfo) -> *mut Isolate;
        pub unsafe fn pci_get_this(info: *mut PropertyCallbackInfo) -> Local;
        pub unsafe fn pci_set_return_value(info: *mut PropertyCallbackInfo, value: Local);
        pub unsafe fn psci_get_isolate(info: *mut PropertySetterCallbackInfo) -> *mut Isolate;
        pub unsafe fn psci_get_this(info: *mut PropertySetterCallbackInfo) -> Local;

        // PropertyCallbackInfo<v8::Integer>, for wildcard property queries
        pub unsafe fn pqci_get_isolate(info: *mut PropertyQueryCallbackInfo) -> *mut Isolate;
        pub unsafe fn pqci_get_this(info: *mut PropertyQueryCallbackInfo) -> Local;

        // FastApiCallbackOptions
        pub unsafe fn fast_api_unwrap_receiver(
            options: *mut FastApiCallbackOptions,
            receiver: Local,
        ) -> TraitObjectPtr;
        pub unsafe fn fast_api_throw_exception(
            options: *mut FastApiCallbackOptions,
            exception_type: ExceptionType,
            message: &str,
        );
        pub unsafe fn fast_api_throw_internal_error(
            options: *mut FastApiCallbackOptions,
            internal_message: &str,
        );
        pub unsafe fn fast_api_terminate_execution(options: *mut FastApiCallbackOptions);

        // Serialization
        // Leaves what serialization throws pending; see `call_into_js()`. Fallible: the result
        // is a rust::Vec.
        pub unsafe fn local_serialize(
            isolate: *mut Isolate,
            value: &Local,
            errors_are_host_objects: bool,
            result: &mut Vec<u8>,
        ) -> Result<bool>;
        // Leaves what deserialization throws pending. Fallible: a missing or unsupported header
        // throws.
        pub unsafe fn value_deserialize(isolate: *mut Isolate, data: &[u8]) -> Result<MaybeLocal>;

        // Errors
        pub unsafe fn exception_create_from_bytes(
            isolate: *mut Isolate,
            exception_type: ExceptionType,
            data: *const u8,
            length: i32,
        ) -> Local;
        pub unsafe fn exception_create_internal(
            isolate: *mut Isolate,
            internal_message: &str,
        ) -> Local;

        // TryCatch
        pub unsafe fn isolate_try_catch(
            isolate: *mut Isolate,
            data: usize,
            callback: unsafe fn(usize) -> bool,
            exception: &mut MaybeLocal,
        );

        // Isolate
        pub unsafe fn isolate_throw_exception(isolate: *mut Isolate, exception: Local);
        pub unsafe fn isolate_throw_error(isolate: *mut Isolate, message: &str);
        pub unsafe fn isolate_throw_internal_error(isolate: *mut Isolate, internal_message: &str);
        pub unsafe fn isolate_terminate_execution(isolate: *mut Isolate);
        pub unsafe fn isolate_is_execution_terminating(isolate: *mut Isolate) -> bool;
        pub unsafe fn isolate_has_pending_exception(isolate: *mut Isolate) -> bool;
        pub unsafe fn isolate_is_locked(isolate: *mut Isolate) -> bool;
        pub unsafe fn isolate_in_context(isolate: *mut Isolate) -> bool;

        /// Tells LeakSanitizer, if enabled, that the heap allocation at `ptr` is leaked on
        /// purpose, along with everything reachable from it.
        pub unsafe fn ignore_leak(ptr: usize);
    }

    /// Fat pointer to a `dyn GarbageCollected` trait object plus its TypeId.
    ///
    /// Stored inside the C++ `Wrappable` struct. Contains everything needed to
    /// reconstruct the Rust trait object and verify its type.
    pub struct TraitObjectPtr {
        /// `Rc::into_raw(*const R)` — data pointer to the resource.
        pub data_ptr: usize,
        /// Vtable pointer for `R as dyn GarbageCollected`.
        pub vtable_ptr: usize,
        /// `TypeId::of::<R>()` low 64 bits.
        pub type_id_lo: usize,
        /// `TypeId::of::<R>()` high 64 bits.
        pub type_id_hi: usize,
    }

    pub struct ConstructorDescriptor {
        callback: usize,
    }

    #[derive(Debug, PartialEq, Eq, Copy, Clone)]
    pub enum FastApiType {
        Void,
        Bool,
        Float64,
    }

    pub struct FastApiDescriptor {
        callback: usize,
        return_type: FastApiType,
        arg_types: Vec<FastApiType>,
    }

    pub struct MethodDescriptor {
        name: String,
        callback: usize,
        fast: KjMaybe<FastApiDescriptor>,
    }

    pub struct StaticConstantDescriptor {
        pub name: String,
        pub value: f64, /* number */
    }

    // Canonical definition of `jsg::PropertyKind` (re-exported from `jsg::lib` as
    // `pub use v8::ffi::PropertyKind`).  jsg-macros uses its own compile-time copy
    // because proc-macro crates cannot link against CXX-bridge runtime crates.
    enum PropertyKind {
        /// Accessor on the prototype chain; enumerable.
        Prototype = 0,
        /// Own accessor on every instance; enumerable.
        Instance = 1,
        /// Registered under a unique symbol on the prototype; invisible to normal
        /// enumeration and string-key lookup; surfaced by `node:util` `inspect()`.
        Inspect = 2,
        /// Own data property on every instance whose getter runs on first access;
        /// the result then replaces it as a plain writable data property.
        LazyInstance = 3,
        /// As `LazyInstance`, but the property is read-only.
        LazyReadonlyInstance = 4,
    }

    /// Descriptor for a single accessor property. `setter_callback` is `None` for
    /// read-only properties; ignored entirely for `Inspect` and the lazy kinds.
    pub struct PropertyDescriptor {
        pub name: String,
        pub kind: PropertyKind,
        pub getter_callback: usize,
        /// `None` means read-only (no setter).
        pub setter_callback: KjMaybe<usize>,
    }

    /// The callbacks of a wildcard property: a named-property interceptor on every instance
    /// (`v8::NamedPropertyGetterCallback` and `v8::NamedPropertyQueryCallback`).
    pub struct WildcardPropertyDescriptor {
        pub getter_callback: usize,
        pub query_callback: usize,
    }

    pub struct ResourceDescriptor {
        pub name: String,
        pub constructor: KjMaybe<ConstructorDescriptor>,
        pub methods: Vec<MethodDescriptor>,
        pub properties: Vec<PropertyDescriptor>,
        pub static_methods: Vec<MethodDescriptor>,
        pub static_constants: Vec<StaticConstantDescriptor>,
        pub wildcard_property: KjMaybe<WildcardPropertyDescriptor>,
    }

    // Resources
    unsafe extern "C++" {
        unsafe fn create_resource_template(
            isolate: *mut Isolate,
            descriptor: &ResourceDescriptor,
        ) -> Global /* v8::Global<FunctionTemplate> */;

        // Leaves what creating the wrapper throws pending, and checks for termination itself,
        // rather than through `call_into_js()`; see `ffi.h`.
        pub unsafe fn wrap_resource(
            isolate: *mut Isolate,
            wrappable: KjRc<Wrappable>,
            constructor: &Global, /* v8::Global<FunctionTemplate> */
            wrapped_elsewhere: &mut bool,
        ) -> MaybeLocal /* v8::Local<Value> */;

        pub unsafe fn wrappable_attach_wrapper(
            wrappable: KjRc<Wrappable>,
            args: Pin<&mut FunctionCallbackInfo>,
        );

        // Fallible: throws if the Wrappable already has a wrapper, or if V8 cannot create one,
        // which happens only when execution is terminating.
        pub unsafe fn wrappable_attach_opaque_wrapper(
            isolate: *mut Isolate,
            wrappable: KjRc<Wrappable>,
        ) -> Result<Local>;

        // Infallible: only defensive tag checks; returns kj::none for non-Rust
        // wrappables rather than throwing.
        pub unsafe fn unwrap_resource(
            isolate: *mut Isolate,
            value: Local, /* v8::LocalValue */
        ) -> KjMaybe<KjRc<Wrappable>>;

        pub unsafe fn function_template_get_function(
            isolate: *mut Isolate,
            constructor: &Global, /* v8::Global<FunctionTemplate> */
        ) -> Local /* v8::Local<Function> */;

        unsafe fn function_template_set_nested_type(
            isolate: *mut Isolate,
            constructor: &Global, /* v8::Global<FunctionTemplate> */
            name: &str,
            nested: &Global, /* v8::Global<FunctionTemplate> */
        );

        pub unsafe fn utf8_value_new(isolate: *mut Isolate, value: Local) -> Utf8Value;
        pub unsafe fn utf8_value_drop(value: Utf8Value);
        pub unsafe fn utf8_value_length(value: &Utf8Value) -> usize;
        pub unsafe fn utf8_value_data(value: &Utf8Value) -> *const u8;
    }

    /// Module visibility level, mirroring workerd::jsg::ModuleType from modules.capnp.
    ///
    /// CXX shared enums cannot reference existing C++ enums, so we define matching values here.
    /// The conversion to workerd::jsg::ModuleType happens in jsg.h's RustModuleRegistry.
    enum ModuleType {
        Bundle = 0,
        Builtin = 1,
        Internal = 2,
    }

    unsafe extern "C++" {
        type ModuleRegistry;

        pub unsafe fn register_add_builtin_module(
            registry: Pin<&mut ModuleRegistry>,
            specifier: &str,
            callback: unsafe fn(*mut Isolate) -> MaybeLocal,
            module_type: ModuleType,
        );
    }
}

impl std::fmt::Display for ffi::ExceptionType {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        let name = match *self {
            Self::OperationError => "OperationError",
            Self::DataError => "DataError",
            Self::DataCloneError => "DataCloneError",
            Self::InvalidAccessError => "InvalidAccessError",
            Self::InvalidStateError => "InvalidStateError",
            Self::InvalidCharacterError => "InvalidCharacterError",
            Self::NotSupportedError => "NotSupportedError",
            Self::SyntaxError => "SyntaxError",
            Self::TimeoutError => "TimeoutError",
            Self::TypeMismatchError => "TypeMismatchError",
            Self::AbortError => "AbortError",
            Self::NotFoundError => "NotFoundError",
            Self::TypeError => "TypeError",
            Self::RangeError => "RangeError",
            Self::ReferenceError => "ReferenceError",
            _ => "Error",
        };
        write!(f, "{name}")
    }
}

// Marker types for Local<T>, Global<T> and TracedGlobal<T>. Each mirrors the V8 class of the
// same name, and a handle of it is used as `Local<'s, T>`, as `v8::Local<T>` is in C++. The casts
// between them are in `impl_local_cast!`.
//
// Deliberately left unimplemented because we didn't need them yet:
// - Primitive, Numeric, Integer, Int32, Uint32
// - NumberObject, BooleanObject, StringObject, SymbolObject, BigIntObject
// - Map, Set, Proxy, RegExp, External, DataView, SharedArrayBuffer, Float16Array
// - WasmModuleObject, WasmMemoryObject
// - Context, Template, ObjectTemplate, Signature, Module, Script, Private, PrimitiveArray
//
// `Promise` and `PromiseResolver` (`v8::Promise::Resolver`) are in `promise.rs`.
#[derive(Debug)]
pub struct Value;
/// Marker for `v8::Boolean` handles.
#[derive(Debug)]
pub struct Boolean;
/// Marker for `v8::Number` handles. A number that Rust code holds as a plain value is an `f64`;
/// this is the handle to one on the V8 heap.
#[derive(Debug)]
pub struct Number;
/// Marker for `v8::Name` handles (supertype of `String` and `Symbol`).
#[derive(Debug)]
pub struct Name;
/// Marker for `v8::String` handles.
#[derive(Debug)]
pub struct String;
/// Marker for `v8::Symbol` handles.
#[derive(Debug)]
pub struct Symbol;
/// Marker for `v8::BigInt` handles.
#[derive(Debug)]
pub struct BigInt;
/// Marker for `v8::Date` handles.
#[derive(Debug)]
pub struct Date;

impl String {
    /// Maximum length of a V8 string in UTF-16 code units.
    ///
    /// Matches `v8::String::kMaxLength`. Attempting to create a string longer than
    /// this will cause V8 to return `None`.
    pub const MAX_LENGTH: i32 = if cfg!(target_pointer_width = "32") {
        (1 << 28) - 16
    } else {
        (1 << 29) - 24
    };

    /// Returns the empty string singleton.
    ///
    /// Corresponds to `v8::String::Empty()`.
    pub fn empty<'s>(lock: &mut crate::Lock<'s>) -> Local<'s, Self> {
        let isolate = lock.isolate();
        // SAFETY: Lock guarantees the isolate is locked and a HandleScope is active.
        unsafe { Local::from_ffi(ffi::local_string_empty(isolate.as_ffi())) }
    }

    /// Creates a new string from a `&str`.
    ///
    /// Corresponds to `v8::String::NewFromUtf8`.
    pub fn new_from_str<'s>(lock: &mut crate::Lock<'s>, data: &str) -> Option<Local<'s, Self>> {
        Self::new_from_utf8(lock, data.as_bytes())
    }

    /// Creates an internalized string from a `&str`.
    ///
    /// Equal strings will be pointer-equal after internalization, which speeds up
    /// property-key lookups at the cost of a hash-table probe on creation.
    ///
    /// Corresponds to `v8::String::NewFromUtf8` with `kInternalized`.
    pub fn new_internalized_from_str<'s>(
        lock: &mut crate::Lock<'s>,
        data: &str,
    ) -> Option<Local<'s, Self>> {
        Self::new_internalized_from_utf8(lock, data.as_bytes())
    }

    /// Creates a new string from a UTF-8 string literal.
    ///
    /// Panics at runtime if `literal.len()` exceeds [`Self::MAX_LENGTH`], matching
    /// the compile-time `static_assert` that `v8::String::NewFromUtf8Literal` performs.
    ///
    /// # Panics
    ///
    /// Panics if `literal.len()` exceeds [`Self::MAX_LENGTH`].
    ///
    /// Corresponds to `v8::String::NewFromUtf8Literal`.
    pub fn new_from_utf8_literal<'s>(
        lock: &mut crate::Lock<'s>,
        literal: &'static str,
    ) -> Option<Local<'s, Self>> {
        assert!(
            literal.len() <= Self::MAX_LENGTH as usize,
            "string literal exceeds v8::String::kMaxLength"
        );
        Self::new_from_utf8(lock, literal.as_bytes())
    }

    /// Creates a new string from UTF-8 data.
    ///
    /// Returns `None` if V8 cannot allocate the string.
    ///
    /// Strings longer than `i32::MAX` bytes are silently truncated to `i32::MAX` bytes
    /// to satisfy V8's `int`-typed length parameter; in practice V8 will reject strings
    /// that large anyway due to heap limits.
    ///
    /// Corresponds to `v8::String::NewFromUtf8`.
    pub fn new_from_utf8<'s>(lock: &mut crate::Lock<'s>, data: &[u8]) -> Option<Local<'s, Self>> {
        let isolate = lock.isolate();
        let len = i32::try_from(data.len()).unwrap_or(i32::MAX);
        // SAFETY: Lock guarantees the isolate is locked and a HandleScope is active;
        // data.as_ptr() and len describe a valid byte slice.
        let handle =
            unsafe { ffi::local_string_new_from_utf8(isolate.as_ffi(), data.as_ptr(), len, false) };
        // SAFETY: a non-empty handle is a string V8 created in the lock's scope.
        unsafe { Local::from_ffi_maybe(handle) }
    }

    /// Creates an internalized string from UTF-8 data.
    ///
    /// Equal strings will be pointer-equal after internalization, which speeds up
    /// property-key lookups at the cost of a hash-table probe on creation.
    ///
    /// Strings longer than `i32::MAX` bytes are silently truncated to `i32::MAX` bytes.
    ///
    /// Corresponds to `v8::String::NewFromUtf8` with `kInternalized`.
    pub fn new_internalized_from_utf8<'s>(
        lock: &mut crate::Lock<'s>,
        data: &[u8],
    ) -> Option<Local<'s, Self>> {
        let isolate = lock.isolate();
        let len = i32::try_from(data.len()).unwrap_or(i32::MAX);
        // SAFETY: Lock guarantees the isolate is locked and a HandleScope is active;
        // data.as_ptr() and len describe a valid byte slice.
        let handle =
            unsafe { ffi::local_string_new_from_utf8(isolate.as_ffi(), data.as_ptr(), len, true) };
        // SAFETY: a non-empty handle is a string V8 created in the lock's scope.
        unsafe { Local::from_ffi_maybe(handle) }
    }

    /// Creates a new string from Latin-1 (one-byte) data.
    ///
    /// Each byte is mapped to the Unicode code point with the same value.
    /// Returns `None` if V8 cannot allocate the string.
    ///
    /// Strings longer than `i32::MAX` bytes are silently truncated to `i32::MAX` bytes.
    ///
    /// Corresponds to `v8::String::NewFromOneByte`.
    pub fn new_from_one_byte<'s>(
        lock: &mut crate::Lock<'s>,
        data: &[u8],
    ) -> Option<Local<'s, Self>> {
        let isolate = lock.isolate();
        let len = i32::try_from(data.len()).unwrap_or(i32::MAX);
        // SAFETY: Lock guarantees the isolate is locked; data.as_ptr() and len are valid.
        let handle = unsafe {
            ffi::local_string_new_from_one_byte(isolate.as_ffi(), data.as_ptr(), len, false)
        };
        // SAFETY: a non-empty handle is a string V8 created in the lock's scope.
        unsafe { Local::from_ffi_maybe(handle) }
    }

    /// Creates an internalized string from Latin-1 (one-byte) data.
    ///
    /// Equal strings will be pointer-equal after internalization.
    /// Strings longer than `i32::MAX` bytes are silently truncated to `i32::MAX` bytes.
    ///
    /// Corresponds to `v8::String::NewFromOneByte` with `kInternalized`.
    pub fn new_internalized_from_one_byte<'s>(
        lock: &mut crate::Lock<'s>,
        data: &[u8],
    ) -> Option<Local<'s, Self>> {
        let isolate = lock.isolate();
        let len = i32::try_from(data.len()).unwrap_or(i32::MAX);
        // SAFETY: Lock guarantees the isolate is locked; data.as_ptr() and len are valid.
        let handle = unsafe {
            ffi::local_string_new_from_one_byte(isolate.as_ffi(), data.as_ptr(), len, true)
        };
        // SAFETY: a non-empty handle is a string V8 created in the lock's scope.
        unsafe { Local::from_ffi_maybe(handle) }
    }

    /// Creates a new string from UTF-16 data.
    ///
    /// Returns `None` if V8 cannot allocate the string.
    ///
    /// Strings longer than `i32::MAX` code units are silently truncated to `i32::MAX` code units.
    ///
    /// Corresponds to `v8::String::NewFromTwoByte`.
    pub fn new_from_two_byte<'s>(
        lock: &mut crate::Lock<'s>,
        data: &[u16],
    ) -> Option<Local<'s, Self>> {
        let isolate = lock.isolate();
        let len = i32::try_from(data.len()).unwrap_or(i32::MAX);
        // SAFETY: Lock guarantees the isolate is locked; data.as_ptr() and len are valid.
        let handle = unsafe {
            ffi::local_string_new_from_two_byte(isolate.as_ffi(), data.as_ptr(), len, false)
        };
        // SAFETY: a non-empty handle is a string V8 created in the lock's scope.
        unsafe { Local::from_ffi_maybe(handle) }
    }

    /// Creates an internalized string from UTF-16 data.
    ///
    /// Equal strings will be pointer-equal after internalization.
    /// Strings longer than `i32::MAX` code units are silently truncated to `i32::MAX` code units.
    ///
    /// Corresponds to `v8::String::NewFromTwoByte` with `kInternalized`.
    pub fn new_internalized_from_two_byte<'s>(
        lock: &mut crate::Lock<'s>,
        data: &[u16],
    ) -> Option<Local<'s, Self>> {
        let isolate = lock.isolate();
        let len = i32::try_from(data.len()).unwrap_or(i32::MAX);
        // SAFETY: Lock guarantees the isolate is locked; data.as_ptr() and len are valid.
        let handle = unsafe {
            ffi::local_string_new_from_two_byte(isolate.as_ffi(), data.as_ptr(), len, true)
        };
        // SAFETY: a non-empty handle is a string V8 created in the lock's scope.
        unsafe { Local::from_ffi_maybe(handle) }
    }

    /// Concatenates two strings. Returns `None` if the result would be longer than
    /// [`MAX_LENGTH`](Self::MAX_LENGTH).
    ///
    /// Corresponds to `v8::String::Concat()`.
    pub fn concat<'s>(
        lock: &mut crate::Lock<'s>,
        left: Local<'s, Self>,
        right: Local<'s, Self>,
    ) -> Option<Local<'s, Self>> {
        let isolate = lock.isolate();
        // SAFETY: Lock guarantees the isolate is locked and a HandleScope is active; a
        // non-empty handle is a string V8 created in the lock's scope.
        unsafe {
            Local::from_ffi_maybe(ffi::local_string_concat(
                isolate.as_ffi(),
                left.into_ffi(),
                right.into_ffi(),
            ))
        }
    }
}

#[derive(Debug)]
pub struct Object;
pub struct Function;
pub struct FunctionTemplate;
pub struct Array;
pub struct TypedArray;
pub struct Uint8Array;
pub struct Uint16Array;
pub struct Uint32Array;
pub struct Int8Array;
pub struct Int16Array;
pub struct Int32Array;
pub struct Float32Array;
pub struct Float64Array;
pub struct BigInt64Array;
pub struct BigUint64Array;
pub struct Uint8ClampedArray;
pub struct ArrayBuffer;
pub struct ArrayBufferView;

// =============================================================================
// `BackingStore` — owned handle to a heap-allocated `std::shared_ptr<v8::BackingStore>`
// =============================================================================

/// Owned handle to a `v8::BackingStore`.
///
/// A `BackingStore` is the raw memory region that backs an `ArrayBuffer` or
/// `SharedArrayBuffer`. Internally this type holds a raw pointer to a
/// heap-allocated `std::shared_ptr<v8::BackingStore>`, keeping the underlying
/// memory alive even after the original JS buffer handle goes out of scope.
///
/// Obtained via [`Local<ArrayBuffer>::backing_store()`].
pub struct BackingStore {
    /// Non-zero `usize` encoding the address of a heap-allocated
    /// `std::shared_ptr<v8::BackingStore>`. Using `usize` matches the CXX FFI
    /// convention of representing opaque pointers as `size_t`.
    /// Freed by `ffi::backing_store_drop` on `Drop`.
    ptr: NonZeroUsize,
}

impl Drop for BackingStore {
    fn drop(&mut self) {
        // SAFETY: ptr was obtained from a `local_*_get_backing_store` FFI call
        // and is uniquely owned by self.
        unsafe { ffi::backing_store_drop(self.ptr.get()) }
    }
}

impl BackingStore {
    /// Creates a new resizable `BackingStore` with an initial `byte_length` and a
    /// maximum capacity of `max_byte_length`.
    ///
    /// The resulting `BackingStore` can be passed to [`ArrayBuffer::from_backing_store`]
    /// to create a resizable `ArrayBuffer`.
    pub fn new_resizable(byte_length: usize, max_byte_length: usize) -> Self {
        // SAFETY: V8 guarantees a non-null result or crashes on OOM.
        let ptr = unsafe { ffi::backing_store_new_resizable(byte_length, max_byte_length) };
        // SAFETY: ptr is a freshly allocated shared_ptr, uniquely owned.
        unsafe { Self::from_raw(ptr) }
    }

    /// Wraps a `usize` address returned by a `local_*_get_backing_store` FFI call.
    ///
    /// # Safety
    /// `ptr` must be a non-zero value returned by one of the
    /// `local_*_get_backing_store` FFI functions, and the caller must transfer
    /// unique ownership to this `BackingStore`.
    #[expect(clippy::expect_used, reason = "the caller guarantees a non-zero `ptr`")]
    unsafe fn from_raw(ptr: usize) -> Self {
        Self {
            ptr: NonZeroUsize::new(ptr).expect("backing_store pointer must be non-null"),
        }
    }

    /// Returns a raw pointer to the backing store's data.
    ///
    /// Valid for the lifetime of this `BackingStore`.
    /// May be null for zero-byte buffers.
    #[inline]
    pub fn data(&self) -> *mut u8 {
        // SAFETY: ptr is a valid heap-allocated shared_ptr owned by self.
        unsafe { ffi::backing_store_data(self.ptr.get()) }
    }

    /// Returns the current byte length of the backing store.
    #[inline]
    pub fn byte_length(&self) -> usize {
        // SAFETY: ptr is valid and owned by self.
        unsafe { ffi::backing_store_byte_length(self.ptr.get()) }
    }

    /// Returns the maximum byte length.
    ///
    /// For resizable `ArrayBuffer`s this is `>= byte_length()`. For fixed-size
    /// buffers it equals `byte_length()`.
    #[inline]
    pub fn max_byte_length(&self) -> usize {
        // SAFETY: ptr is valid and owned by self.
        unsafe { ffi::backing_store_max_byte_length(self.ptr.get()) }
    }

    /// Returns `true` if this backing store was created for a `SharedArrayBuffer`.
    #[inline]
    pub fn is_shared(&self) -> bool {
        // SAFETY: ptr is valid and owned by self.
        unsafe { ffi::backing_store_is_shared(self.ptr.get()) }
    }

    /// Returns `true` if user JavaScript code may resize this buffer.
    #[inline]
    pub fn is_resizable_by_user_javascript(&self) -> bool {
        // SAFETY: ptr is valid and owned by self.
        unsafe { ffi::backing_store_is_resizable_by_user_javascript(self.ptr.get()) }
    }

    /// Returns `true` if the backing store has zero bytes.
    #[inline]
    pub fn is_empty(&self) -> bool {
        self.byte_length() == 0
    }

    /// Returns a shared byte slice into the backing store data.
    ///
    /// The slice borrows `lock`. Everything that can run JavaScript needs
    /// `&mut Lock`, so no JavaScript can modify the buffer through a
    /// `TypedArray` view while the slice is live, which would violate the
    /// immutability of `&[u8]`.
    ///
    /// # Safety
    /// For shared backing stores (`is_shared() == true`), other agents may
    /// concurrently write to this memory. The caller must ensure no concurrent
    /// writes occur for the duration of the borrow.
    ///
    /// TODO(soon): When there is a safe aliasing model for Rust mutable access,
    /// integrate this method with that somehow.
    #[inline]
    pub unsafe fn as_slice<'b>(&'b self, _lock: &'b crate::Lock<'_>) -> &'b [u8] {
        if self.is_empty() {
            return &[];
        }
        // SAFETY: data() is non-null for non-empty stores; byte_length() bytes are valid.
        unsafe { std::slice::from_raw_parts(self.data().cast_const(), self.byte_length()) }
    }

    /// Returns a mutable byte slice into the backing store data.
    ///
    /// Requires `&mut Lock` so that no JavaScript can execute while the mutable
    /// slice is live — the borrow on the lock prevents calling `eval`, invoking
    /// functions, or any other operation that could modify or detach the buffer.
    ///
    /// # Safety
    /// The caller must ensure no other live reference (shared or mutable) to
    /// this memory region exists for the duration of the borrow.
    ///
    /// TODO(soon): Define a safe aliasing model for Rust mutable access based on
    /// interrogating the backing store shared_ptr being singular for the lifetime
    /// of the current Rust execution model.
    #[doc(hidden)]
    #[inline]
    pub unsafe fn as_mut_slice<'b>(&'b mut self, _lock: &'b mut crate::Lock<'_>) -> &'b mut [u8] {
        if self.is_empty() {
            return &mut [];
        }
        // SAFETY: data() is non-null for non-empty stores; byte_length() bytes are valid.
        unsafe { std::slice::from_raw_parts_mut(self.data(), self.byte_length()) }
    }
}

/// A handle to a V8 value, valid while the `HandleScope` identified by `'s` is open.
///
/// `'s` is the brand of the [`Lock`](crate::Lock) whose scope created the handle; every API
/// that creates a handle takes a `&mut Lock<'s>` and returns a `Local<'s, _>`. The handle is
/// `Copy` (it is a pointer to a slot in the scope, not an owner of anything) and covariant in
/// `'s`, so a handle from an outer scope can be used wherever a handle of an inner scope is
/// expected. It is neither `Send` nor `Sync`.
///
/// It is one non-null pointer, laid out like C++ `v8::Local<T>` and `ffi::Local`, so
/// `Option<Local<'s, T>>` is laid out like `v8::MaybeLocal<T>` and `ffi::MaybeLocal`, with
/// `None` as the null pointer. It stores no isolate: whatever needs one takes the lock.
#[repr(transparent)]
pub struct Local<'s, T> {
    // `T` is a marker type, never dereferenced; `NonNull` makes the handle neither `Send` nor
    // `Sync`.
    ptr: NonNull<T>,
    _marker: PhantomData<&'s ()>,
}

const _: () = {
    assert!(size_of::<Local<'static, Value>>() == size_of::<ffi::Local>());
    assert!(align_of::<Local<'static, Value>>() == align_of::<ffi::Local>());
    assert!(size_of::<Option<Local<'static, Value>>>() == size_of::<ffi::MaybeLocal>());
    assert!(align_of::<Option<Local<'static, Value>>>() == align_of::<ffi::MaybeLocal>());
};

// Hand-written so that `Local<'s, T>` is `Copy` for every marker type `T`; `#[derive]` would
// require `T: Copy`.
impl<T> Copy for Local<'_, T> {}

impl<T> Clone for Local<'_, T> {
    fn clone(&self) -> Self {
        *self
    }
}

impl<T> std::fmt::Debug for Local<'_, T> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Local")
            .field("ptr", &self.ptr)
            .finish_non_exhaustive()
    }
}

// Common implementations for all Local<'s, T>
impl<'s, T> Local<'s, T> {
    /// Creates a `Local` from an FFI handle.
    ///
    /// # Safety
    /// The caller must ensure that `handle` points to a V8 value that stays alive within the
    /// `HandleScope` identified by `'s`. The caller picks `'s`; prefer letting it be inferred
    /// from a `Lock<'s>` the handle is used with.
    ///
    /// # Panics
    /// Panics if `handle` is empty; a handle that can be empty is an `ffi::MaybeLocal`.
    #[expect(
        clippy::expect_used,
        reason = "an empty handle here is a C++ bug, not a JavaScript error"
    )]
    pub unsafe fn from_ffi(handle: ffi::Local) -> Self {
        Local {
            ptr: NonNull::new(std::ptr::with_exposed_provenance_mut(handle.ptr))
                .expect("C++ passed an empty v8::Local"),
            _marker: PhantomData,
        }
    }

    /// Returns the underlying FFI handle.
    ///
    /// # Safety
    /// The returned FFI handle carries no lifetime; it must not be used after the
    /// `HandleScope` that owns the V8 value is closed.
    pub unsafe fn into_ffi(self) -> ffi::Local {
        *self.raw()
    }

    /// Returns a reference to the underlying FFI handle.
    ///
    /// # Safety
    /// The caller must ensure the returned reference is not used after this `Local` is dropped.
    pub unsafe fn as_ffi(&self) -> &ffi::Local {
        self.raw()
    }

    /// Panics in debug builds if the value cannot be used in `lock`'s isolate: if it is in the
    /// heap of another isolate, or in the read-only or shared space of another isolate group.
    ///
    /// The scope rules keep handles in their isolate, except inside
    /// [`allow_scopes_of_another_isolate`](crate::allow_scopes_of_another_isolate); this
    /// catches handles that cross isolates there.
    pub(crate) fn debug_assert_isolate(&self, lock: &Lock<'_>) {
        debug_assert!(
            // SAFETY: the lock's isolate is locked, and the check reads only the value's page
            // header, which is valid whichever isolate owns the value.
            unsafe { ffi::local_is_compatible_with_isolate(lock.isolate().as_ffi(), self.raw()) },
            "attempt to use a Local in an isolate other than the one it was created in"
        );
    }

    /// The handle as the FFI type that C++ functions take, without giving up its lifetime.
    pub(crate) fn raw(&self) -> &ffi::Local {
        // SAFETY: `Local` is a `repr(transparent)` pointer and `ffi::Local` a pointer-sized
        // integer, with the same size and alignment (asserted above).
        unsafe { &*std::ptr::from_ref(self).cast::<ffi::Local>() }
    }

    /// Creates an `Option<Local>` from an FFI handle that may be empty.
    ///
    /// # Safety
    /// If `handle` is not empty, it must point to a V8 value that stays alive within the
    /// `HandleScope` identified by `'s`, as for [`from_ffi`](Self::from_ffi).
    pub unsafe fn from_ffi_maybe(handle: ffi::MaybeLocal) -> Option<Self> {
        NonNull::new(std::ptr::with_exposed_provenance_mut(handle.ptr)).map(|ptr| Local {
            ptr,
            _marker: PhantomData,
        })
    }

    /// Returns the FFI handle of `local`, empty for `None`.
    ///
    /// # Safety
    /// The returned FFI handle carries no lifetime; it must not be used after the
    /// `HandleScope` that owns the V8 value is closed.
    pub unsafe fn into_ffi_maybe(local: Option<Self>) -> ffi::MaybeLocal {
        ffi::MaybeLocal {
            ptr: local.map_or(0, |local| local.raw().ptr),
        }
    }

    /// Views `locals` as the FFI handles C++ functions take, without copying.
    fn raw_slice(locals: &[Self]) -> &[ffi::Local] {
        // SAFETY: as for `raw`, element by element.
        unsafe { std::slice::from_raw_parts(locals.as_ptr().cast::<ffi::Local>(), locals.len()) }
    }

    /// Creates a persistent handle to this value.
    pub fn to_global(self, lock: &mut Lock<'s>) -> Global<T> {
        Global::new(lock, self)
    }

    /// Creates a persistent handle to this value, for a resource field that GC traces.
    pub fn to_traced_global(self, lock: &mut Lock<'s>) -> TracedGlobal<T> {
        TracedGlobal::new(lock, self)
    }

    pub fn is_string(&self) -> bool {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_is_string(self.raw()) }
    }

    pub fn is_boolean(&self) -> bool {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_is_boolean(self.raw()) }
    }

    pub fn is_number(&self) -> bool {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_is_number(self.raw()) }
    }

    pub fn is_null(&self) -> bool {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_is_null(self.raw()) }
    }

    pub fn is_undefined(&self) -> bool {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_is_undefined(self.raw()) }
    }

    pub fn is_null_or_undefined(&self) -> bool {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_is_null_or_undefined(self.raw()) }
    }

    /// Returns true if the value is a JavaScript object.
    ///
    /// Note: Unlike JavaScript's `typeof` operator which returns "object" for `null`,
    /// this method returns `false` for `null` values. Use `is_null_or_undefined()`
    /// to check for nullish values separately.
    pub fn is_object(&self) -> bool {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_is_object(self.raw()) }
    }

    /// Returns true if the value is a native JavaScript error.
    pub fn is_native_error(&self) -> bool {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_is_native_error(self.raw()) }
    }

    /// Returns true if the value is a JavaScript array.
    pub fn is_array(&self) -> bool {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_is_array(self.raw()) }
    }

    /// Returns true if the value is a `Uint8Array`.
    pub fn is_uint8_array(&self) -> bool {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_is_uint8_array(self.raw()) }
    }

    /// Returns true if the value is a `Uint16Array`.
    pub fn is_uint16_array(&self) -> bool {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_is_uint16_array(self.raw()) }
    }

    /// Returns true if the value is a `Uint32Array`.
    pub fn is_uint32_array(&self) -> bool {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_is_uint32_array(self.raw()) }
    }

    /// Returns true if the value is an `Int8Array`.
    pub fn is_int8_array(&self) -> bool {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_is_int8_array(self.raw()) }
    }

    /// Returns true if the value is an `Int16Array`.
    pub fn is_int16_array(&self) -> bool {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_is_int16_array(self.raw()) }
    }

    /// Returns true if the value is an `Int32Array`.
    pub fn is_int32_array(&self) -> bool {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_is_int32_array(self.raw()) }
    }

    /// Returns true if the value is a `Float32Array`.
    pub fn is_float32_array(&self) -> bool {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_is_float32_array(self.raw()) }
    }

    /// Returns true if the value is a `Float64Array`.
    pub fn is_float64_array(&self) -> bool {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_is_float64_array(self.raw()) }
    }

    /// Returns true if the value is a `BigInt64Array`.
    pub fn is_bigint64_array(&self) -> bool {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_is_bigint64_array(self.raw()) }
    }

    /// Returns true if the value is a `BigUint64Array`.
    pub fn is_biguint64_array(&self) -> bool {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_is_biguint64_array(self.raw()) }
    }

    /// Returns true if the value is a `Float16Array`.
    pub fn is_float16_array(&self) -> bool {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_is_float16_array(self.raw()) }
    }

    /// Returns true if the value is a `Uint8ClampedArray`.
    pub fn is_uint8clamped_array(&self) -> bool {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_is_uint8clamped_array(self.raw()) }
    }

    /// Returns true if the value is an `ArrayBuffer`.
    pub fn is_array_buffer(&self) -> bool {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_is_array_buffer(self.raw()) }
    }

    /// Returns true if the value is an `ArrayBufferView`
    /// (i.e. any `TypedArray` or `DataView`).
    pub fn is_array_buffer_view(&self) -> bool {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_is_array_buffer_view(self.raw()) }
    }

    /// Returns true if the value is a `TypedArray`
    pub fn is_typed_array(&self) -> bool {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_is_typed_array(self.raw()) }
    }

    /// Returns true if the value is a `SharedArrayBuffer`.
    pub fn is_shared_array_buffer(&self) -> bool {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_is_shared_array_buffer(self.raw()) }
    }

    /// Returns true if the value is a JavaScript function.
    pub fn is_function(&self) -> bool {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_is_function(self.raw()) }
    }

    /// Returns true if the value is a JavaScript Promise.
    pub fn is_promise(&self) -> bool {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_is_promise(self.raw()) }
    }

    /// Returns true if the value is a JavaScript Symbol.
    ///
    /// Corresponds to `v8::Value::IsSymbol()`.
    pub fn is_symbol(&self) -> bool {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_is_symbol(self.raw()) }
    }

    /// Returns true if the value is a `v8::Name` (either a `String` or a `Symbol`).
    ///
    /// Corresponds to `v8::Value::IsName()`.
    pub fn is_name(&self) -> bool {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_is_name(self.raw()) }
    }

    /// Returns true if the value is a JavaScript `BigInt`.
    ///
    /// Corresponds to `v8::Value::IsBigInt()`.
    pub fn is_big_int(&self) -> bool {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_is_big_int(self.raw()) }
    }

    /// Returns true if the value is a JavaScript `Date`.
    ///
    /// Corresponds to `v8::Value::IsDate()`.
    pub fn is_date(&self) -> bool {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_is_date(self.raw()) }
    }

    /// Returns the JavaScript type of the underlying value as a string.
    ///
    /// Uses V8's native `TypeOf` method which returns the same result as
    /// JavaScript's `typeof` operator: "undefined", "boolean", "number",
    /// "bigint", "string", "symbol", "function", or "object".
    ///
    /// Note: For `null`, this returns "object" (JavaScript's historical behavior).
    pub fn type_of(&self, lock: &mut Lock<'s>) -> std::string::String {
        // SAFETY: `lock` guarantees the isolate is locked; handle is valid within the current
        // HandleScope.
        unsafe { ffi::local_type_of(lock.isolate().as_ffi(), self.raw()) }
    }
}

impl PartialEq for Local<'_, Value> {
    fn eq(&self, other: &Self) -> bool {
        // SAFETY: both handles are valid within the current HandleScope.
        unsafe { ffi::local_eq(self.raw(), other.raw()) }
    }
}

impl PartialEq for Local<'_, String> {
    fn eq(&self, other: &Self) -> bool {
        // SAFETY: Both handles are valid V8 String handles.
        unsafe { ffi::local_string_equals(self.raw(), other.raw()) }
    }
}

impl Eq for Local<'_, String> {}

/// The state of a function created by [`Function::new`], owned by the function's C++
/// `Wrappable`.
struct FunctionData<T>(T);

impl<T: Traced> Traced for FunctionData<T> {
    fn holds_gc_references() -> bool {
        T::holds_gc_references()
    }

    fn trace(&self, visitor: &mut GcVisitor) {
        self.0.trace(visitor);
    }
}

impl<T: Traced> GarbageCollected for FunctionData<T> {
    fn memory_name(&self) -> &'static std::ffi::CStr {
        // C++ JSG's name for the object that owns a wrapped function.
        c"WrappableFunction"
    }
}

impl Function {
    /// Creates a JavaScript function that calls `f` with `data`, like C++ JSG's
    /// `Lock::wrapReturningFunction()`.
    ///
    /// The function owns `data`: the garbage collector traces it through the function and drops
    /// it once the function is unreachable, so the `TracedGlobal`s and `jsg::Rc`s that `data`
    /// traces may lead back to the function. Two things break this:
    ///
    /// - A reference that `T::trace` does not visit, such as a `v8::Global` (`Traced::trace` does
    ///   nothing by default). If it can reach the function, the cycle is invisible to the garbage
    ///   collector, and the function and `data` are freed only when the isolate is torn down.
    /// - Another owner that the garbage collector also traces, such as a resource sharing part of
    ///   `data` through a `std::rc::Rc`. `data` must be the only owner that traces what it
    ///   traces: tracing one `jsg::Rc` from two owners fails an assertion, and a `TracedGlobal`
    ///   traced from two owners can be freed while one of them still uses it. To share state,
    ///   hold a `jsg::Rc` or `jsg::Weak` to the resource that owns it.
    ///
    /// `f` must capture nothing, which a compile-time assertion checks; whatever it needs comes
    /// from `data`.
    ///
    /// `f`'s `Ok` value is the call's result and an `Err` is thrown. A panic throws an internal
    /// error and terminates execution, as in a `#[jsg_method]`. As with the functions C++ JSG
    /// wraps, the function's `name` is empty, its `length` is 0, and it can be called with `new`.
    ///
    /// While execution is terminating, or a JavaScript exception is pending, creates nothing and
    /// returns an error for which [`Error::is_pending`] is true, so that returning it propagates
    /// the termination or the exception.
    ///
    /// ```ignore
    /// let greet = v8::Function::new(lock, String::from("hello"), |lock, greeting, args| {
    ///     let name = String::from_js(lock, args.get(0))?;
    ///     format!("{greeting}, {name}").to_js(lock)
    /// })?;
    /// ```
    pub fn new<'s, T, F>(lock: &mut Lock<'s>, data: T, f: F) -> crate::Result<Local<'s, Self>>
    where
        T: Traced + 'static,
        F: UnitType
            + 'static
            + for<'a> Fn(
                &mut Lock<'a>,
                &T,
                &mut FunctionCallbackInfo<'a>,
            ) -> Result<Local<'a, Value>, Error>,
    {
        let [function] = Self::new_sharing_data(lock, data, [Self::callback::<T, F>(f)])?;
        Ok(function)
    }

    /// Creates one function per callback, all with the same data: one Wrappable owns `data`,
    /// and its opaque wrapper is the V8 data of every function, so the garbage collector traces
    /// `data` once, whichever of the functions are reachable, and drops it once none is.
    ///
    /// Each callback comes from [`Function::callback`] with the same `T`. Fails as
    /// [`Function::new`] does.
    pub(crate) fn new_sharing_data<'s, T, const N: usize>(
        lock: &Lock<'s>,
        data: T,
        callbacks: [crate::RawFunctionCallback; N],
    ) -> crate::Result<[Local<'s, Self>; N]>
    where
        T: Traced + 'static,
    {
        // As in `jsg::Rc::new`, the Wrappable owns a leaked `Rc` of the data, released when the
        // Wrappable is destroyed.
        let raw: *const FunctionData<T> = Rc::into_raw(Rc::new(FunctionData(data)));
        let fat: *mut dyn GarbageCollected = raw as *const dyn GarbageCollected as *mut _;
        let mut wrappable: WrappableRc =
            ffi::TraitObjectPtr::from_raw(fat, TypeId::of::<FunctionData<T>>()).into();
        let functions = wrappable.new_functions(lock, callbacks);
        // A new Wrappable starts with one strong reference, which `jsg::Rc` would hold. Here only
        // the functions own it, so the reference is dropped and the garbage collector decides.
        wrappable.remove_strong_ref(true);
        functions
    }

    /// The callback with which a function that [`Function::new_sharing_data`] creates with
    /// data `T` calls `f`, as [`Function::new`]'s function calls its closure.
    pub(crate) fn callback<T, F>(f: F) -> crate::RawFunctionCallback
    where
        T: Traced + 'static,
        F: UnitType
            + 'static
            + for<'a> Fn(
                &mut Lock<'a>,
                &T,
                &mut FunctionCallbackInfo<'a>,
            ) -> Result<Local<'a, Value>, Error>,
    {
        let _ = f;
        crate::function_callback(call_function::<T, F>)
    }
}

/// The callback of a function created by [`Function::new`] with data `T` and closure `F`.
fn call_function<'s, T, F>(lock: &mut Lock<'s>, args: &mut FunctionCallbackInfo<'s>)
where
    T: Traced + 'static,
    F: UnitType
        + 'static
        + for<'a> Fn(
            &mut Lock<'a>,
            &T,
            &mut FunctionCallbackInfo<'a>,
        ) -> Result<Local<'a, Value>, Error>,
{
    let Some(data) = WrappableRc::of_function(args) else {
        unreachable!("a function created by Function::new has a Wrappable as its data");
    };
    let Some(data_ptr) = data.resolve::<FunctionData<T>>() else {
        unreachable!(
            "a function created by Function::new has data of the type it was created with"
        );
    };
    // SAFETY: `data` keeps the Wrappable, and with it the `FunctionData`, alive for this call.
    // Only shared references to the `FunctionData` are ever created.
    let data_ref = unsafe { &data_ptr.as_ref().0 };
    // SAFETY: `Function::new` was handed a value of `F`.
    let f = unsafe { crate::scope::unit_value::<F>() };
    match f(lock, data_ref, args) {
        Ok(value) => args.set_return_value(value),
        Err(err) => lock.throw_exception(&err),
    }
}

impl<'s> Local<'s, Function> {
    /// Calls this function and converts the result via [`FromJS`].
    ///
    /// `receiver` is the `this` value; pass `None` for `undefined`.
    ///
    /// `args` is a slice of `Local<Value>` — use [`ToJS::to_js`](crate::ToJS::to_js) to
    /// convert Rust values, or `.into()` for other `Local<T>` handles.
    ///
    /// # Errors
    ///
    /// If the function throws, or execution is terminating (then without calling it), the
    /// exception stays pending and this
    /// returns an error for which [`Error::is_pending`] is true. [`Lock::throw_exception`] throws
    /// nothing for it, so
    /// returning it from a callback propagates the exception to the callback's JavaScript caller,
    /// as C++ JSG does. Don't call into JavaScript after such an error; to handle the exception
    /// instead, make the call in [`Lock::try_catch`]. Also fails if the result does not convert
    /// to `R`.
    ///
    /// # Example
    ///
    /// ```ignore
    /// let x = x.to_js(lock)?;
    /// let y = y.to_js(lock)?;
    /// let result: Number = func.call(lock, None, &[x, y])?;
    /// ```
    pub fn call<R: FromJS<'s>>(
        self,
        lock: &mut Lock<'s>,
        receiver: Option<Local<'s, Value>>,
        args: &[Local<'s, Value>],
    ) -> Result<R::ResultType, Error> {
        let recv = receiver.unwrap_or_else(|| lock.undefined());
        self.debug_assert_isolate(lock);
        recv.debug_assert_isolate(lock);
        for arg in args {
            arg.debug_assert_isolate(lock);
        }

        let result = call_into_js(lock, |lock| {
            // SAFETY: lock guarantees the isolate is locked and a HandleScope is active.
            // self is a valid Local<Function>; recv and args are valid Local handles. The
            // result belongs to the lock's scope.
            Ok(unsafe {
                Local::from_ffi_maybe(ffi::local_function_call(
                    lock.isolate().as_ffi(),
                    self.raw(),
                    recv.raw(),
                    Local::raw_slice(args),
                ))
            })
        })?;
        R::from_js(lock, result)
    }
}

/// Runs `shim`, a call through the FFI that can run JavaScript, and returns what it returns.
///
/// Every such call goes through here, so that all of them handle exceptions and termination
/// alike. `shim` returns `None` if JavaScript threw, leaving the exception pending (see `ffi.h`);
/// that becomes an error for which [`Error::is_pending`] is true. While execution is terminating,
/// `shim` is not run and the error is the same: calling into V8 then would clear the termination.
/// An error from `shim` itself (a C++ exception, such as a failed `KJ_REQUIRE`) is passed on.
///
/// The one exception is `wrap_resource`, which checks for termination itself, so that its common
/// case, reading a wrapper that already exists, takes one FFI call rather than two.
pub(crate) fn call_into_js<'s, T>(
    lock: &mut Lock<'s>,
    shim: impl FnOnce(&mut Lock<'s>) -> Result<Option<T>, Error>,
) -> Result<T, Error> {
    // SAFETY: lock guarantees the isolate is locked.
    if unsafe { ffi::isolate_is_execution_terminating(lock.isolate().as_ffi()) } {
        return Err(Error::pending());
    }
    shim(lock)?.ok_or_else(Error::pending)
}

/// The error a checked downcast between `Local` handle types returns when the value is not
/// of the target type.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DataError {
    BadType {
        actual: &'static str,
        expected: &'static str,
    },
}

impl DataError {
    fn bad_type<E: 'static, A: 'static>() -> Self {
        Self::BadType {
            expected: std::any::type_name::<E>(),
            actual: std::any::type_name::<A>(),
        }
    }
}

impl std::fmt::Display for DataError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::BadType { expected, actual } => {
                write!(f, "expected type `{expected}`, got `{actual}`")
            }
        }
    }
}

impl std::error::Error for DataError {}

/// Implements the casts between `Local<$from>` and each of its supertypes `Local<$to>`.
///
/// All V8 handle subtypes share the same pointer representation, so a cast just
/// reinterprets the handle. An upcast is a `From` and costs nothing. A downcast is a
/// `TryFrom` that checks the value with `$check`.
///
/// Rust cannot derive the casts through intermediate types, so each type lists every one of
/// its supertypes. The upcast is unchecked, so each `$to` must be a superclass of `$from` in
/// V8, and `$check` must be the `Is*` test for `$from`.
macro_rules! impl_local_cast {
    ($from:ident: $($to:ident),+; $check:ident) => {
        $(
            impl<'a> From<Local<'a, $from>> for Local<'a, $to> {
                fn from(value: Local<'a, $from>) -> Self {
                    // SAFETY: V8 subtypes share handle representation, and every `$from` is a
                    // `$to`.
                    unsafe { Self::from_ffi(value.into_ffi()) }
                }
            }
            impl<'a> TryFrom<Local<'a, $to>> for Local<'a, $from> {
                type Error = DataError;

                fn try_from(value: Local<'a, $to>) -> Result<Self, Self::Error> {
                    if value.$check() {
                        // SAFETY: V8 subtypes share handle representation; `$check` verified
                        // that the value is a `$from`.
                        Ok(unsafe { Self::from_ffi(value.into_ffi()) })
                    } else {
                        Err(DataError::bad_type::<$from, $to>())
                    }
                }
            }
        )+
    };
}

impl_local_cast!(Boolean: Value; is_boolean);
impl_local_cast!(Number: Value; is_number);
impl_local_cast!(Name: Value; is_name);
impl_local_cast!(String: Name, Value; is_string);
impl_local_cast!(Symbol: Name, Value; is_symbol);
impl_local_cast!(BigInt: Value; is_big_int);
impl_local_cast!(Object: Value; is_object);
impl_local_cast!(Date: Object, Value; is_date);
impl_local_cast!(Function: Object, Value; is_function);
impl_local_cast!(Promise: Object, Value; is_promise);
impl_local_cast!(Array: Object, Value; is_array);
impl_local_cast!(ArrayBuffer: Object, Value; is_array_buffer);
impl_local_cast!(ArrayBufferView: Object, Value; is_array_buffer_view);
impl_local_cast!(TypedArray: ArrayBufferView, Object, Value; is_typed_array);
impl_local_cast!(Uint8Array: TypedArray, ArrayBufferView, Object, Value; is_uint8_array);
impl_local_cast!(Uint8ClampedArray: TypedArray, ArrayBufferView, Object, Value; is_uint8clamped_array);
impl_local_cast!(Uint16Array: TypedArray, ArrayBufferView, Object, Value; is_uint16_array);
impl_local_cast!(Uint32Array: TypedArray, ArrayBufferView, Object, Value; is_uint32_array);
impl_local_cast!(Int8Array: TypedArray, ArrayBufferView, Object, Value; is_int8_array);
impl_local_cast!(Int16Array: TypedArray, ArrayBufferView, Object, Value; is_int16_array);
impl_local_cast!(Int32Array: TypedArray, ArrayBufferView, Object, Value; is_int32_array);
impl_local_cast!(Float32Array: TypedArray, ArrayBufferView, Object, Value; is_float32_array);
impl_local_cast!(Float64Array: TypedArray, ArrayBufferView, Object, Value; is_float64_array);
impl_local_cast!(BigInt64Array: TypedArray, ArrayBufferView, Object, Value; is_bigint64_array);
impl_local_cast!(BigUint64Array: TypedArray, ArrayBufferView, Object, Value; is_biguint64_array);

impl Array {
    /// Creates a new JavaScript array with the given length.
    pub fn new<'s>(lock: &mut crate::Lock<'s>, len: usize) -> Local<'s, Self> {
        let isolate = lock.isolate();
        // SAFETY: isolate is valid and locked (guaranteed by Lock).
        unsafe { Local::from_ffi(ffi::local_new_array(isolate.as_ffi(), len)) }
    }

    /// Creates a new JavaScript array of `elements`.
    ///
    /// Runs no JavaScript: unlike [`Local::<Array>::set`], it calls no setter that a script
    /// defined on `Array.prototype`. Corresponds to `v8::Array::New(isolate, elements, length)`.
    pub fn with_elements<'s>(
        lock: &mut crate::Lock<'s>,
        elements: &[Local<'s, Value>],
    ) -> Local<'s, Self> {
        for element in elements {
            element.debug_assert_isolate(lock);
        }
        // SAFETY: lock guarantees the isolate is locked and a HandleScope is active; the
        // elements are valid handles in it.
        unsafe {
            Local::from_ffi(ffi::array_new_with_elements(
                lock.isolate().as_ffi(),
                Local::raw_slice(elements),
            ))
        }
    }
}

impl<'s> Local<'s, Array> {
    /// Returns the length of the array.
    #[inline]
    pub fn len(&self) -> usize {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_array_length(self.raw()) as usize }
    }

    /// Returns true if the array is empty.
    #[inline]
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    /// Sets element `index` to `value`, like JS `array[index] = value`.
    ///
    /// # Errors
    ///
    /// If a setter, on the array or its prototype chain, throws, or execution is terminating,
    /// the exception stays pending and the error is one for which [`Error::is_pending`] is
    /// true, as for [`Local::<Function>::call`].
    pub fn set(
        self,
        lock: &mut Lock<'s>,
        index: u32,
        value: Local<'s, Value>,
    ) -> crate::Result<()> {
        self.debug_assert_isolate(lock);
        value.debug_assert_isolate(lock);
        call_into_js(lock, |lock| {
            let mut handle = *self.raw();
            // SAFETY: lock guarantees the isolate is locked; both handles are valid in scope 's.
            let ok = unsafe {
                ffi::local_array_set(
                    lock.isolate().as_ffi(),
                    &mut handle,
                    index,
                    value.into_ffi(),
                )
            };
            Ok(ok.then_some(()))
        })
    }

    /// Iterates over array elements using V8's native `Array::Iterate()`.
    /// Returns `TracedGlobal` handles because `Local` handles get reused during iteration.
    ///
    /// # Errors
    ///
    /// Fails if the value is not an array. If a getter or Proxy trap on an element throws, or
    /// execution is terminating, the exception stays pending and the error is one for which
    /// [`Error::is_pending`] is true, as for [`Local::<Function>::call`].
    pub fn iterate(self, lock: &mut Lock<'s>) -> crate::Result<Vec<TracedGlobal<Value>>> {
        let globals = call_into_js(lock, |lock| {
            let mut globals = Vec::new();
            // SAFETY: lock guarantees the isolate is locked; handle is valid in scope 's.
            let ok = unsafe {
                ffi::local_array_iterate(lock.isolate().as_ffi(), self.into_ffi(), &mut globals)
            }?;
            Ok(ok.then_some(globals))
        })?;
        let state = lock.isolate_state();
        Ok(globals
            .into_iter()
            .map(|g| TracedGlobal::from_ffi_in(g, Arc::clone(state)))
            .collect())
    }
}

// =============================================================================
// `ArrayBuffer`-specific implementations
// =============================================================================

impl ArrayBuffer {
    /// Creates a new `ArrayBuffer` by copying `data`.
    pub fn new<'s>(lock: &mut crate::Lock<'s>, data: &[u8]) -> Local<'s, Self> {
        let isolate = lock.isolate();
        // SAFETY: Lock guarantees the isolate is locked and a HandleScope is active.
        unsafe {
            Local::from_ffi(ffi::local_new_array_buffer(
                isolate.as_ffi(),
                data.as_ptr(),
                data.len(),
            ))
        }
    }

    /// Attempts to create a new `ArrayBuffer` with `byte_length` bytes using
    /// the given initialization mode (zeroed or uninitialized).
    ///
    /// Returns `None` if allocation fails.
    pub fn new_with_mode<'s>(
        lock: &mut crate::Lock<'s>,
        byte_length: usize,
        mode: ffi::BackingStoreInitializationMode,
    ) -> Option<Local<'s, Self>> {
        let isolate = lock.isolate();
        // SAFETY: Lock guarantees the isolate is locked and a HandleScope is active; a
        // non-empty result is an `ArrayBuffer` V8 created in the lock's scope.
        unsafe {
            Local::from_ffi_maybe(ffi::array_buffer_new_with_mode(
                isolate.as_ffi(),
                byte_length,
                mode,
            ))
        }
    }

    /// Wraps an existing `BackingStore` in a new `ArrayBuffer`.
    ///
    /// The `ArrayBuffer` shares ownership of the backing store's memory via the
    /// underlying `shared_ptr` reference count.
    // The BackingStore is taken by value to express ownership transfer; the
    // C++ ArrayBuffer::New copies the shared_ptr so both sides hold a reference.
    #[expect(
        clippy::needless_pass_by_value,
        reason = "ownership transfer semantics"
    )]
    pub fn from_backing_store<'s>(
        lock: &mut crate::Lock<'s>,
        store: BackingStore,
    ) -> Local<'s, Self> {
        let isolate = lock.isolate();
        // SAFETY: Lock guarantees the isolate is locked and a HandleScope is active.
        // store.ptr is a valid heap-allocated shared_ptr; we pass it by raw pointer
        // so the C++ side can copy the shared_ptr (incrementing refcount).
        unsafe {
            Local::from_ffi(ffi::array_buffer_from_backing_store(
                isolate.as_ffi(),
                store.ptr.get(),
            ))
        }
    }

    /// Creates a new zero-initialized `ArrayBuffer` with the given byte length.
    pub fn new_zeroed<'s>(lock: &mut crate::Lock<'s>, byte_length: usize) -> Local<'s, Self> {
        let isolate = lock.isolate();
        // SAFETY: Lock guarantees the isolate is locked and a HandleScope is active.
        unsafe {
            Local::from_ffi(ffi::local_new_array_buffer_empty(
                isolate.as_ffi(),
                byte_length,
            ))
        }
    }

    /// Attempts to create a new zero-initialized `ArrayBuffer` with
    /// `byte_length` bytes, and lets `fill` write into it before anything else
    /// can observe it. Returns the buffer along with `fill`'s result, or `None`
    /// if allocation fails, in which case `fill` is not called.
    ///
    /// This is the safe way to produce an `ArrayBuffer` from bytes computed in
    /// Rust without an intermediate `Vec`: unlike the `unsafe`
    /// `Local::<ArrayBuffer>::as_mut_slice`, exclusive access to the backing
    /// store is guaranteed by construction rather than by the caller.
    ///
    /// Bytes `fill` does not write remain zero. The buffer is always zeroed
    /// first because JavaScript can read the whole backing store once it has
    /// the buffer, including any bytes a view over it excludes.
    pub fn new_zeroed_with<'s, R>(
        lock: &mut crate::Lock<'s>,
        byte_length: usize,
        fill: impl FnOnce(&mut [u8]) -> R,
    ) -> Option<(Local<'s, Self>, R)> {
        let mut buffer = Self::new_with_mode(
            lock,
            byte_length,
            ffi::BackingStoreInitializationMode::ZeroInitialized,
        )?;
        // SAFETY: `buffer` was allocated just above and is the only handle to
        // it: no other `Local` refers to it and it has never been exposed to
        // JavaScript, so nothing else can reach its backing store until it is
        // returned. `fill` is not passed the `Lock` and cannot capture it,
        // since `lock` is mutably borrowed for this call, so it cannot run
        // JavaScript. The slice does not outlive `fill`.
        let result = fill(unsafe { buffer.as_mut_slice(lock) });
        Some((buffer, result))
    }
}

impl<'s> Local<'s, ArrayBuffer> {
    /// Returns the byte length of this `ArrayBuffer`.
    #[inline]
    pub fn byte_length(&self) -> usize {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_array_buffer_byte_length(self.raw()) }
    }

    /// Returns `true` if this `ArrayBuffer` has zero bytes.
    #[inline]
    pub fn is_empty(&self) -> bool {
        self.byte_length() == 0
    }

    /// Returns a raw pointer to the backing store data.
    ///
    /// # Safety
    /// The pointer is valid only while the backing store is alive (i.e. while this
    /// `Local` handle — or another handle to the same buffer — remains in scope).
    #[inline]
    pub unsafe fn data(&self) -> *mut u8 {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_array_buffer_data(self.raw()) }
    }

    /// Returns a shared byte slice view into the `ArrayBuffer`'s backing store.
    ///
    /// Zero-copy: the slice points directly into V8-managed memory.
    ///
    /// The slice borrows `lock`. Everything that can run JavaScript (and so detach,
    /// resize or write to the buffer) needs `&mut Lock`, so none of it can happen while
    /// the slice is live.
    ///
    /// TODO(soon): When there is a safe aliasing model for Rust mutable access,
    /// integrate this method with that somehow.
    #[inline]
    pub fn as_slice<'b>(self, _lock: &'b Lock<'s>) -> &'b [u8] {
        if self.is_empty() {
            return &[];
        }
        // SAFETY: data() is non-null for non-empty buffers; byte_length() bytes are valid
        // until JS runs, which the shared borrow of the lock prevents for 'b.
        unsafe { std::slice::from_raw_parts(self.data().cast_const(), self.byte_length()) }
    }

    /// Returns a mutable byte slice view into the `ArrayBuffer`'s backing store.
    ///
    /// Zero-copy: points directly into V8-managed memory.
    ///
    /// The slice mutably borrows `lock` for its whole lifetime, so no JavaScript
    /// can execute while it is live — the borrow prevents calling `eval`, invoking
    /// functions, or any other operation that could modify or detach the buffer.
    ///
    /// # Safety
    ///
    /// The caller must ensure that no other live reference (shared or mutable)
    /// into the same `ArrayBuffer` region exists for the duration of the returned
    /// slice. `Local` is `Copy`, so `&mut self` does not prevent aliasing through
    /// another handle to the same buffer — the caller is responsible for ensuring
    /// exclusivity.
    ///
    /// TODO(soon): Define a safe aliasing model for Rust mutable access based on
    /// interrogating the backing store shared_ptr being singular for the lifetime
    /// of the current Rust execution model.
    #[doc(hidden)]
    #[inline]
    pub unsafe fn as_mut_slice<'b>(&mut self, _lock: &'b mut Lock<'s>) -> &'b mut [u8] {
        if self.is_empty() {
            return &mut [];
        }
        // SAFETY: caller guarantees exclusive access; data() is non-null for
        // non-empty buffers; byte_length() bytes are valid while the lock is borrowed.
        unsafe { std::slice::from_raw_parts_mut(self.data(), self.byte_length()) }
    }

    /// Copies the `ArrayBuffer` contents into a new `Vec<u8>`.
    pub fn to_vec(&self) -> Vec<u8> {
        if self.is_empty() {
            return Vec::new();
        }
        // SAFETY: data() is non-null for non-empty buffers; byte_length() bytes are valid,
        // and no JS can run before the copy completes.
        unsafe { std::slice::from_raw_parts(self.data().cast_const(), self.byte_length()) }.to_vec()
    }

    /// Detaches this `ArrayBuffer`, setting its byte length to zero.
    ///
    /// After detaching, the buffer's data is no longer accessible from JS.
    /// Typed array views backed by this buffer also become zero-length.
    ///
    /// Takes the lock mutably so that a detach cannot happen while a slice
    /// borrowed from it (see [`as_slice`](Self::as_slice)) is live.
    pub fn detach(self, lock: &mut Lock<'_>) {
        let mut handle = *self.raw();
        // SAFETY: lock guarantees the isolate is locked; handle is valid.
        unsafe { ffi::local_array_buffer_detach(lock.isolate().as_ffi(), &mut handle) }
    }

    /// Returns `true` if this `ArrayBuffer` has been detached.
    pub fn was_detached(&self) -> bool {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_array_buffer_was_detached(self.raw()) }
    }

    /// Returns `true` if this `ArrayBuffer` can be detached.
    pub fn is_detachable(&self) -> bool {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_array_buffer_is_detachable(self.raw()) }
    }

    /// Returns `true` if the underlying V8 value is a `SharedArrayBuffer`.
    ///
    /// This performs a value-level check, allowing code that receives a
    /// `Local<ArrayBuffer>` via cast to distinguish shared buffers.
    pub fn is_shared(&self) -> bool {
        ffi::local_array_buffer_is_shared(self.raw())
    }

    /// Returns the `BackingStore` for this `ArrayBuffer`.
    ///
    /// The returned `BackingStore` keeps the underlying memory alive independently
    /// of this `Local` handle.
    pub fn backing_store(&self) -> BackingStore {
        // SAFETY: handle is valid within the current HandleScope; the returned pointer
        // is a freshly heap-allocated shared_ptr that we uniquely own.
        let ptr = unsafe { ffi::local_array_buffer_get_backing_store(self.raw()) };
        // SAFETY: the FFI guarantees a non-null pointer on success.
        unsafe { BackingStore::from_raw(ptr) }
    }
}

// =============================================================================
// `ArrayBufferView`-specific implementations
// =============================================================================

impl<'s> Local<'s, ArrayBufferView> {
    /// Returns the byte offset of this view within its backing `ArrayBuffer`.
    #[inline]
    pub fn byte_offset(&self) -> usize {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_array_buffer_view_byte_offset(self.raw()) }
    }

    /// Returns the byte length of this view.
    #[inline]
    pub fn byte_length(&self) -> usize {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_array_buffer_view_byte_length(self.raw()) }
    }

    /// Returns `true` if this view covers zero bytes.
    #[inline]
    pub fn is_empty(&self) -> bool {
        self.byte_length() == 0
    }

    /// Returns the size in bytes of a single element for typed arrays.
    ///
    /// Returns `0` for `DataView` (which has no fixed element size).
    /// For example: `1` for `Uint8Array`, `4` for `Float32Array`, `8` for
    /// `Float64Array`.
    #[inline]
    pub fn element_size(&self) -> usize {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_array_buffer_view_element_size(self.raw()) }
    }

    /// Returns `true` if the element type is an integer type.
    ///
    /// Returns `false` for `Float32Array`, `Float64Array`, and `DataView`.
    #[inline]
    pub fn is_integer_type(&self) -> bool {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_array_buffer_view_is_integer_type(self.raw()) }
    }

    /// Returns a raw pointer to the backing `ArrayBuffer`'s data (without byte offset).
    ///
    /// Add `byte_offset()` to reach the first byte of this view's region.
    ///
    /// # Safety
    /// The pointer is valid only while the backing `ArrayBuffer` is alive.
    #[inline]
    pub unsafe fn buffer_data(&self) -> *mut u8 {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_array_buffer_view_buffer_data(self.raw()) }
    }

    /// Returns the backing `ArrayBuffer` of this view.
    pub fn buffer(self, lock: &mut Lock<'s>) -> Local<'s, ArrayBuffer> {
        // SAFETY: lock guarantees the isolate is locked; the new handle is created in the
        // lock's scope.
        unsafe {
            Local::from_ffi(ffi::local_array_buffer_view_get_buffer(
                lock.isolate().as_ffi(),
                self.raw(),
            ))
        }
    }

    /// Returns a shared byte slice of the view's visible region.
    ///
    /// Zero-copy: points directly into V8-managed memory. The slice borrows `lock`, so
    /// no JavaScript (which could detach, resize or write to the buffer) can run while
    /// it is live.
    ///
    /// TODO(soon): When there is a safe aliasing model for Rust mutable access,
    /// integrate this method with that somehow.
    #[inline]
    pub fn as_slice<'b>(self, _lock: &'b Lock<'s>) -> &'b [u8] {
        if self.is_empty() {
            return &[];
        }
        // SAFETY: buffer_data() + byte_offset() gives the start of the view's region;
        // byte_length() bytes are valid until JS runs, which the lock borrow prevents.
        unsafe {
            let ptr = self.buffer_data().byte_add(self.byte_offset());
            std::slice::from_raw_parts(ptr.cast_const(), self.byte_length())
        }
    }

    /// Returns a mutable byte slice of the view's visible region.
    ///
    /// Zero-copy: points directly into V8-managed memory.
    ///
    /// The slice mutably borrows `lock` for its whole lifetime, so no JavaScript
    /// can execute while it is live — the borrow prevents calling `eval`, invoking
    /// functions, or any other operation that could modify or detach the buffer.
    ///
    /// # Safety
    ///
    /// The caller must ensure that no other live reference (shared or mutable)
    /// into the same buffer region exists for the duration of the returned
    /// slice. `Local` is `Copy`, so `&mut self` does not prevent aliasing through
    /// another handle backed by the same buffer — the caller is responsible for
    /// ensuring exclusivity.
    ///
    /// TODO(soon): Define a safe aliasing model for Rust mutable access based on
    /// interrogating the backing store shared_ptr being singular for the lifetime
    /// of the current Rust execution model.
    #[doc(hidden)]
    #[inline]
    pub unsafe fn as_mut_slice<'b>(&mut self, _lock: &'b mut Lock<'s>) -> &'b mut [u8] {
        if self.is_empty() {
            return &mut [];
        }
        // SAFETY: caller guarantees exclusive access; same pointer derivation
        // as as_slice; the lock borrow keeps JS from running.
        unsafe {
            let ptr = self.buffer_data().byte_add(self.byte_offset());
            std::slice::from_raw_parts_mut(ptr, self.byte_length())
        }
    }

    /// Copies the view's visible byte range into a new `Vec<u8>`.
    pub fn to_vec(&self) -> Vec<u8> {
        if self.is_empty() {
            return Vec::new();
        }
        // SAFETY: same pointer derivation as as_slice; no JS can run before the copy
        // completes.
        unsafe {
            let ptr = self.buffer_data().byte_add(self.byte_offset());
            std::slice::from_raw_parts(ptr.cast_const(), self.byte_length()).to_vec()
        }
    }
}

// `TypedArray`-specific implementations
impl Local<'_, TypedArray> {
    /// Returns the number of elements in this `TypedArray`.
    pub fn len(&self) -> usize {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_typed_array_length(self.raw()) }
    }

    /// Returns true if the `TypedArray` is empty.
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }
}

// =============================================================================
// `TypedArray` Iterator Types
// =============================================================================

/// Iterator over `TypedArray` elements by reference.
///
/// Created by calling `iter()` on a `Local<'a, TypedArray>`.
/// Does not consume the array, allowing multiple iterations.
pub struct TypedArrayIter<'a, 'b, T, E> {
    array: &'b Local<'a, T>,
    index: usize,
    len: usize,
    _marker: PhantomData<E>,
}

/// Iterator over `TypedArray` elements that holds its own copy of the array handle.
///
/// Created by calling `into_iter()` on a `Local<'a, TypedArray>`.
pub struct TypedArrayIntoIter<'a, T, E> {
    array: Local<'a, T>,
    index: usize,
    len: usize,
    _marker: PhantomData<E>,
}

// =============================================================================
// `TypedArray` Implementation Macro
// =============================================================================

/// Implements methods and traits for a specific `TypedArray` type.
///
/// For each `TypedArray` marker type (e.g., `Uint8Array`), this macro generates:
/// - `len()`, `is_empty()`, `get(index)` methods
/// - `iter()` method for borrowing iteration
/// - `IntoIterator` for owned and borrowed iteration
/// - `Iterator` implementations for both iterator types
macro_rules! impl_typed_array {
    ($marker:ident, $elem:ty, $get_fn:ident) => {
        impl<'a> Local<'a, $marker> {
            /// Returns the number of elements in this `TypedArray`.
            #[inline]
            pub fn len(&self) -> usize {
                // SAFETY: handle is valid within the current HandleScope.
                unsafe { ffi::local_typed_array_length(self.raw()) }
            }

            /// Returns `true` if the `TypedArray` contains no elements.
            #[inline]
            pub fn is_empty(&self) -> bool {
                self.len() == 0
            }

            /// Returns the element at `index`.
            #[inline]
            pub fn get(&self, index: usize) -> $elem {
                // SAFETY: handle is valid within the current HandleScope.
                unsafe { ffi::$get_fn(self.raw(), index) }
            }

            /// Returns the byte offset of this view within its backing `ArrayBuffer`.
            #[inline]
            pub fn byte_offset(&self) -> usize {
                // SAFETY: handle is valid within the current HandleScope.
                unsafe { ffi::local_typed_array_byte_offset(self.raw()) }
            }

            /// Returns the byte length of this view.
            #[inline]
            pub fn byte_length(&self) -> usize {
                // SAFETY: handle is valid within the current HandleScope.
                unsafe { ffi::local_typed_array_byte_length(self.raw()) }
            }

            /// Returns the size in bytes of a single element.
            ///
            /// For example, `1` for `Uint8Array`, `4` for `Uint32Array` and
            /// `Float32Array`, `8` for `Float64Array`.
            #[inline]
            pub fn element_size(&self) -> usize {
                std::mem::size_of::<$elem>()
            }

            /// Returns `true` if the element type is an integer type.
            ///
            /// Returns `false` for `Float32Array` and `Float64Array`.
            #[inline]
            pub fn is_integer_type(&self) -> bool {
                let id = TypeId::of::<$elem>();
                id != TypeId::of::<f32>() && id != TypeId::of::<f64>()
            }

            /// Returns a raw pointer to the backing `ArrayBuffer`'s data.
            ///
            /// This does **not** account for `byte_offset()` — the caller must
            /// add it manually when accessing this view's region of the buffer.
            ///
            /// # Safety
            /// The pointer is valid only while the backing `ArrayBuffer` is alive
            /// and the `Local` handle is in scope.
            #[inline]
            pub unsafe fn data(&self) -> *mut $elem {
                // SAFETY: handle is valid within the current HandleScope.
                unsafe { ffi::local_typed_array_buffer_data(self.raw()) as *mut $elem }
            }

            /// Returns a shared slice view of the typed array's data.
            ///
            /// Zero-copy: points directly into the V8 `ArrayBuffer`'s backing store.
            /// The slice borrows `lock`, so no JavaScript (which could detach, resize
            /// or write to the buffer) can run while it is live.
            ///
            /// TODO(soon): When there is a safe aliasing model for Rust mutable access,
            /// integrate this method with that somehow.
            #[inline]
            pub fn as_slice<'b>(self, _lock: &'b crate::Lock<'a>) -> &'b [$elem] {
                if self.is_empty() {
                    return &[];
                }
                // SAFETY: handle is valid; non-empty guarantees non-null data pointer.
                // data() returns the ArrayBuffer base; byte_offset() gives this view's start.
                unsafe {
                    let ptr = self.data().byte_add(self.byte_offset());
                    std::slice::from_raw_parts(ptr.cast_const(), self.len())
                }
            }

            /// Returns a mutable slice view of the typed array's data.
            ///
            /// Zero-copy: points directly into the V8 `ArrayBuffer`'s backing store.
            ///
            /// The slice mutably borrows `lock` for its whole lifetime, so no
            /// JavaScript can execute while it is live — the borrow prevents calling
            /// `eval`, invoking functions, or any other operation that could modify
            /// or detach the underlying buffer.
            ///
            /// # Safety
            ///
            /// The caller must ensure that no other live reference (shared or mutable)
            /// into the same `ArrayBuffer` region exists for the duration of the returned
            /// slice. `Local` is `Copy`, so `&mut self` does not prevent aliasing through
            /// another handle to the same buffer — the caller is responsible for
            /// ensuring exclusivity.
            ///
            /// TODO(soon): Define a safe aliasing model for Rust mutable access based on
            /// interrogating the backing store shared_ptr being singular for the lifetime
            /// of the current Rust execution model.
            #[doc(hidden)]
            #[inline]
            pub unsafe fn as_mut_slice<'b>(
                &mut self,
                _lock: &'b mut crate::Lock<'a>,
            ) -> &'b mut [$elem] {
                if self.is_empty() {
                    return &mut [];
                }
                // SAFETY: caller guarantees exclusive access to this buffer region;
                // handle is valid and non-empty guarantees a non-null data pointer.
                unsafe {
                    let ptr = self.data().byte_add(self.byte_offset());
                    std::slice::from_raw_parts_mut(ptr, self.len())
                }
            }

            /// Returns an iterator over the elements.
            ///
            /// The iterator yields elements by value (copied from V8 memory).
            #[inline]
            pub fn iter(&self) -> TypedArrayIter<'a, '_, $marker, $elem> {
                TypedArrayIter {
                    array: self,
                    index: 0,
                    len: self.len(),
                    _marker: PhantomData,
                }
            }
        }

        // Owned iteration: `for x in array`
        impl<'a> IntoIterator for Local<'a, $marker> {
            type Item = $elem;
            type IntoIter = TypedArrayIntoIter<'a, $marker, $elem>;

            #[inline]
            fn into_iter(self) -> Self::IntoIter {
                let len = self.len();
                TypedArrayIntoIter {
                    array: self,
                    index: 0,
                    len,
                    _marker: PhantomData,
                }
            }
        }

        // Borrowed iteration: `for x in &array`
        impl<'a, 'b> IntoIterator for &'b Local<'a, $marker> {
            type Item = $elem;
            type IntoIter = TypedArrayIter<'a, 'b, $marker, $elem>;

            #[inline]
            fn into_iter(self) -> Self::IntoIter {
                self.iter()
            }
        }

        impl<'a, 'b> Iterator for TypedArrayIter<'a, 'b, $marker, $elem> {
            type Item = $elem;

            #[inline]
            fn next(&mut self) -> Option<Self::Item> {
                if self.index < self.len {
                    // SAFETY: handle is valid within the current HandleScope; index < len.
                    let value = unsafe { ffi::$get_fn(self.array.raw(), self.index) };
                    self.index += 1;
                    Some(value)
                } else {
                    None
                }
            }

            #[inline]
            fn size_hint(&self) -> (usize, Option<usize>) {
                let remaining = self.len - self.index;
                (remaining, Some(remaining))
            }
        }

        impl<'a, 'b> ExactSizeIterator for TypedArrayIter<'a, 'b, $marker, $elem> {}

        impl<'a, 'b> DoubleEndedIterator for TypedArrayIter<'a, 'b, $marker, $elem> {
            #[inline]
            fn next_back(&mut self) -> Option<Self::Item> {
                if self.index < self.len {
                    self.len -= 1;
                    // SAFETY: handle is valid within the current HandleScope; len is in bounds.
                    let value = unsafe { ffi::$get_fn(self.array.raw(), self.len) };
                    Some(value)
                } else {
                    None
                }
            }
        }

        impl<'a, 'b> std::iter::FusedIterator for TypedArrayIter<'a, 'b, $marker, $elem> {}

        impl<'a> Iterator for TypedArrayIntoIter<'a, $marker, $elem> {
            type Item = $elem;

            #[inline]
            fn next(&mut self) -> Option<Self::Item> {
                if self.index < self.len {
                    // SAFETY: handle is valid within the current HandleScope; index < len.
                    let value = unsafe { ffi::$get_fn(self.array.raw(), self.index) };
                    self.index += 1;
                    Some(value)
                } else {
                    None
                }
            }

            #[inline]
            fn size_hint(&self) -> (usize, Option<usize>) {
                let remaining = self.len - self.index;
                (remaining, Some(remaining))
            }
        }

        impl<'a> ExactSizeIterator for TypedArrayIntoIter<'a, $marker, $elem> {}

        impl<'a> DoubleEndedIterator for TypedArrayIntoIter<'a, $marker, $elem> {
            #[inline]
            fn next_back(&mut self) -> Option<Self::Item> {
                if self.index < self.len {
                    self.len -= 1;
                    // SAFETY: handle is valid within the current HandleScope; len is in bounds.
                    let value = unsafe { ffi::$get_fn(self.array.raw(), self.len) };
                    Some(value)
                } else {
                    None
                }
            }
        }

        impl<'a> std::iter::FusedIterator for TypedArrayIntoIter<'a, $marker, $elem> {}
    };
}

/// Generates `FromJS` for a handle type `Local<'_, T>`: the value must already be a `T`, as C++
/// JSG requires of a `v8::Local<T>` parameter, so no coercion runs.
macro_rules! impl_local_from_js {
    ($marker:ident, $check:ident, $name:expr) => {
        impl<'s> crate::FromJS<'s> for Local<'s, $marker> {
            type ResultType = Self;

            fn type_name() -> std::string::String {
                $name.to_owned()
            }

            fn from_js(
                lock: &mut crate::Lock<'s>,
                value: Local<'s, Value>,
            ) -> Result<Self, crate::Error> {
                if !value.$check() {
                    return Err(crate::Error::new_type_error(format!(
                        "expected {}, got {}",
                        $name,
                        value.type_of(lock)
                    )));
                }
                // SAFETY: type check passed; V8 handles share the same pointer
                // representation across subtypes within the same HandleScope.
                Ok(unsafe { Self::from_ffi(value.into_ffi()) })
            }

            fn try_from_js(
                _lock: &mut crate::Lock<'s>,
                value: Local<'s, Value>,
            ) -> Result<Option<Self>, crate::Error> {
                if !value.$check() {
                    return Ok(None);
                }
                // SAFETY: type check passed; V8 handles share the same pointer
                // representation across subtypes within the same HandleScope.
                Ok(Some(unsafe { Self::from_ffi(value.into_ffi()) }))
            }
        }
    };
}

impl_local_from_js!(Uint8Array, is_uint8_array, "Uint8Array");
impl_local_from_js!(Uint16Array, is_uint16_array, "Uint16Array");
impl_local_from_js!(Uint32Array, is_uint32_array, "Uint32Array");
impl_local_from_js!(Int8Array, is_int8_array, "Int8Array");
impl_local_from_js!(Int16Array, is_int16_array, "Int16Array");
impl_local_from_js!(Int32Array, is_int32_array, "Int32Array");
impl_local_from_js!(Float32Array, is_float32_array, "Float32Array");
impl_local_from_js!(Float64Array, is_float64_array, "Float64Array");
impl_local_from_js!(BigInt64Array, is_bigint64_array, "BigInt64Array");
impl_local_from_js!(BigUint64Array, is_biguint64_array, "BigUint64Array");
impl_local_from_js!(
    Uint8ClampedArray,
    is_uint8clamped_array,
    "Uint8ClampedArray"
);
impl_local_from_js!(ArrayBuffer, is_array_buffer, "ArrayBuffer");
impl_local_from_js!(ArrayBufferView, is_array_buffer_view, "ArrayBufferView");
impl_local_from_js!(Boolean, is_boolean, "Boolean");
impl_local_from_js!(Number, is_number, "Number");

impl_typed_array!(Uint8Array, u8, local_uint8_array_get);
impl_typed_array!(Uint16Array, u16, local_uint16_array_get);
impl_typed_array!(Uint32Array, u32, local_uint32_array_get);
impl_typed_array!(Int8Array, i8, local_int8_array_get);
impl_typed_array!(Int16Array, i16, local_int16_array_get);
impl_typed_array!(Int32Array, i32, local_int32_array_get);
impl_typed_array!(Float32Array, f32, local_float32_array_get);
impl_typed_array!(Float64Array, f64, local_float64_array_get);
impl_typed_array!(BigInt64Array, i64, local_bigint64_array_get);
impl_typed_array!(BigUint64Array, u64, local_biguint64_array_get);
// Uint8ClampedArray has the same element type as Uint8Array; clamping is a write-side JS concern.
impl_typed_array!(Uint8ClampedArray, u8, local_uint8clamped_array_get);

impl Uint8Array {
    /// Creates a `Uint8Array` viewing `length` bytes of `buffer`, starting at
    /// `byte_offset`.
    ///
    /// Zero-copy, unlike [`to_js_typed_array`](crate::ToJsTypedArray::to_js_typed_array), which
    /// allocates a fresh backing store and copies into it. Producing a `Uint8Array`
    /// from bytes computed in Rust therefore does not require an intermediate `Vec`:
    /// allocate and fill the buffer with [`ArrayBuffer::new_zeroed_with`], and wrap
    /// the written prefix here.
    ///
    /// # Errors
    ///
    /// Returns an error if `byte_offset + length` exceeds `buffer`'s byte
    /// length.
    pub fn from_buffer<'s>(
        lock: &mut crate::Lock<'s>,
        buffer: &Local<'s, ArrayBuffer>,
        byte_offset: usize,
        length: usize,
    ) -> crate::Result<Local<'s, Self>> {
        let isolate = lock.isolate();
        // SAFETY: Lock guarantees the isolate is locked and a HandleScope is active;
        // `buffer` is a live handle to an ArrayBuffer. The C++ side checks the
        // bounds and reports a violation as an `Err`.
        let handle = unsafe {
            ffi::uint8_array_from_buffer(isolate.as_ffi(), buffer.raw(), byte_offset, length)
        }?;
        // SAFETY: `handle` is a fresh Uint8Array created in `isolate`.
        Ok(unsafe { Local::from_ffi(handle) })
    }
}

// =============================================================================
// `String`-specific implementations
// =============================================================================

/// Write flags matching `v8::String::WriteFlags`.
///
/// These correspond directly to `kNone`, `kNullTerminate`, and `kReplaceInvalidUtf8`.
/// Flags can be combined with `|` via the `BitOr` impl.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
#[repr(i32)]
pub enum WriteFlags {
    /// No special write flags.
    #[default]
    None = 0,
    /// Include a null terminator in the output. The buffer must have space for it.
    NullTerminate = 1,
    /// Replace invalid UTF-8/UTF-16 sequences with the Unicode replacement character U+FFFD.
    /// Set this to guarantee valid UTF-8 output from `write_utf8`.
    ReplaceInvalidUtf8 = 2,
    /// Combines `NullTerminate` and `ReplaceInvalidUtf8`.
    ///
    /// Equivalent to `NullTerminate | ReplaceInvalidUtf8`. This variant exists so that
    /// the enum covers every value in `0..=3`, making the `BitOr` transmute sound.
    NullTerminateAndReplaceInvalidUtf8 = 3,
}

impl WriteFlags {
    /// Returns the underlying `i32` bitmask value.
    #[inline]
    pub fn bits(self) -> i32 {
        self as i32
    }
}

impl std::ops::BitOr for WriteFlags {
    type Output = Self;

    fn bitor(self, rhs: Self) -> Self {
        // SAFETY: WriteFlags variants cover all values 0..=3; OR-ing any two
        // produces a value in that range, all of which have valid discriminants.
        unsafe { std::mem::transmute(self.bits() | rhs.bits()) }
    }
}

/// Runs a synchronous callback entered from C++ with a valid, locked V8 isolate.
///
/// The callback receives the [`Lock`] of the caller's `HandleScope` and returns a local handle
/// in it. On success, the handle is transferred back across the CXX bridge as a
/// [`ffi::MaybeLocal`]. On failure, the error is scheduled as a JavaScript exception and an
/// empty `MaybeLocal` is returned. A panic is handled by [`catch_panic`](crate::catch_panic)
/// and also returns an empty `MaybeLocal`. A broken precondition is not caught: an isolate
/// that is not locked, or has no context entered, fails an assert before `catch_panic` runs,
/// which aborts the process, since reporting the panic as a JS error needs that state.
///
/// This centralizes the raw-isolate and local-handle ownership transitions for
/// C++ entry points implemented in Rust. The callback itself contains no FFI
/// safety obligations.
///
/// # Safety
///
/// `isolate` must point to a live V8 isolate locked by the current thread with a context
/// entered, and the caller's innermost `HandleScope` must stay open and innermost until the
/// caller consumes the returned handle.
pub unsafe fn run_ffi_callback<T, E, F>(isolate: *mut ffi::Isolate, callback: F) -> ffi::MaybeLocal
where
    E: Into<Error>,
    F: for<'s> FnOnce(&mut Lock<'s>) -> Result<Local<'s, T>, E>,
{
    // SAFETY: forwarded from this function's safety contract.
    let scope = std::pin::pin!(unsafe { crate::CallbackScope::new(isolate) });
    crate::catch_panic(scope, |lock| {
        let local = callback(lock)
            .map_err(|error| lock.throw_exception(&error.into()))
            .ok();
        // SAFETY: the handle lives in the caller's `HandleScope`, which stays open until the
        // caller consumes it, per this function's safety contract.
        unsafe { Local::into_ffi_maybe(local) }
    })
    .unwrap_or_default()
}

impl<'s> Local<'s, String> {
    // Instance methods — correspond to `v8::String` member functions

    /// Returns the number of characters (UTF-16 code units) in the string.
    ///
    /// Returns `i32` to match the V8 API (`v8::String::Length()` returns `int`).
    /// V8 enforces a maximum string length well below `i32::MAX`, so the result
    /// is always non-negative.
    ///
    /// Corresponds to `v8::String::Length()`.
    #[inline]
    pub fn length(&self) -> i32 {
        // SAFETY: the handle is a valid V8 String handle.
        unsafe { ffi::local_string_length(self.raw()) }
    }

    /// Returns `true` if the string is represented internally as one-byte (Latin-1).
    ///
    /// Corresponds to `v8::String::IsOneByte()`.
    #[inline]
    pub fn is_one_byte(&self) -> bool {
        // SAFETY: the handle is a valid V8 String handle.
        unsafe { ffi::local_string_is_one_byte(self.raw()) }
    }

    /// Returns `true` if all characters in the string fit in one byte (Latin-1).
    ///
    /// Unlike `is_one_byte()`, this scans the entire string and may be slow for
    /// two-byte strings that happen to contain only Latin-1 characters.
    ///
    /// Corresponds to `v8::String::ContainsOnlyOneByte()`.
    #[inline]
    pub fn contains_only_one_byte(&self) -> bool {
        // SAFETY: the handle is a valid V8 String handle.
        unsafe { ffi::local_string_contains_only_one_byte(self.raw()) }
    }

    /// Returns the number of bytes required to encode the string as UTF-8.
    ///
    /// Does not include a null terminator.
    ///
    /// Corresponds to `v8::String::Utf8LengthV2()`.
    #[inline]
    pub fn utf8_length(&self, lock: &mut crate::Lock) -> usize {
        // SAFETY: Lock guarantees the isolate is locked; the handle is a valid String handle.
        unsafe { ffi::local_string_utf8_length(lock.isolate().as_ffi(), self.raw()) }
    }

    /// Writes the string as UTF-16 code units into `buffer`.
    ///
    /// `offset` is the index of the first character to write; `length` is the
    /// maximum number of characters to write. `flags` is a combination of
    /// [`WriteFlags`] variants.
    ///
    /// # Panics
    ///
    /// Panics if `buffer.len() < length`.
    ///
    /// Corresponds to `v8::String::WriteV2()`.
    pub fn write(
        &self,
        lock: &mut crate::Lock,
        offset: u32,
        length: u32,
        buffer: &mut [u16],
        flags: WriteFlags,
    ) {
        assert!(
            buffer.len() >= length as usize,
            "buffer too small for requested length"
        );
        // SAFETY: Lock guarantees the isolate is locked; buffer is valid and large enough.
        unsafe {
            ffi::local_string_write_v2(
                lock.isolate().as_ffi(),
                self.raw(),
                offset,
                length,
                buffer.as_mut_ptr(),
                flags.bits(),
            );
        }
    }

    /// Writes the string as Latin-1 bytes into `buffer`.
    ///
    /// Only meaningful when `is_one_byte()` returns `true`; characters outside
    /// the Latin-1 range are truncated.
    ///
    /// # Panics
    ///
    /// Panics if `buffer.len() < length`.
    ///
    /// Corresponds to `v8::String::WriteOneByteV2()`.
    pub fn write_one_byte(
        &self,
        lock: &mut crate::Lock,
        offset: u32,
        length: u32,
        buffer: &mut [u8],
        flags: WriteFlags,
    ) {
        assert!(
            buffer.len() >= length as usize,
            "buffer too small for requested length"
        );
        // SAFETY: Lock guarantees the isolate is locked; buffer is valid and large enough.
        unsafe {
            ffi::local_string_write_one_byte_v2(
                lock.isolate().as_ffi(),
                self.raw(),
                offset,
                length,
                buffer.as_mut_ptr(),
                flags.bits(),
            );
        }
    }

    /// Writes the string as UTF-8 into `buffer`.
    ///
    /// Returns the number of bytes written. `flags` is a combination of
    /// [`WriteFlags`] constants.
    ///
    /// Corresponds to `v8::String::WriteUtf8V2()`.
    pub fn write_utf8(
        &self,
        lock: &mut crate::Lock,
        buffer: &mut [u8],
        flags: WriteFlags,
    ) -> usize {
        // SAFETY: Lock guarantees the isolate is locked; buffer is valid for `capacity` bytes.
        unsafe {
            ffi::local_string_write_utf8_v2(
                lock.isolate().as_ffi(),
                self.raw(),
                buffer.as_mut_ptr(),
                buffer.len(),
                flags.bits(),
            )
        }
    }

    // -------------------------------------------------------------------------
    // Convenience helpers
    // -------------------------------------------------------------------------

    /// Decodes the string to an owned Rust `String` via UTF-8.
    ///
    /// Allocates a buffer using `utf8_length`, writes into it, and converts.
    pub fn to_string(&self, lock: &mut crate::Lock) -> std::string::String {
        let byte_len = self.utf8_length(lock);
        let mut buf = vec![0u8; byte_len];
        let written = self.write_utf8(lock, &mut buf, WriteFlags::None);
        buf.truncate(written);
        // V8 guarantees valid UTF-8 output when REPLACE_INVALID_UTF8 is not set and
        // the string was originally created from valid data. Use from_utf8 to avoid a
        // redundant allocation in the common (valid UTF-8) case; fall back to
        // from_utf8_lossy only when the bytes are not valid UTF-8 (e.g. two-byte strings
        // with unpaired surrogates).
        std::string::String::from_utf8(buf)
            .unwrap_or_else(|e| std::string::String::from_utf8_lossy(e.as_bytes()).into_owned())
    }

    // -------------------------------------------------------------------------
    // Additional string operations
    // -------------------------------------------------------------------------

    /// Returns an internalized version of this string.
    ///
    /// If an equal internalized string already exists in V8's string table it is
    /// returned; otherwise a new internalized copy is created. The result is
    /// pointer-equal to any other internalized string with the same content.
    ///
    /// Corresponds to `v8::String::InternalizeString()`.
    #[must_use]
    pub fn internalize(self, lock: &mut crate::Lock<'s>) -> Self {
        let isolate = lock.isolate();
        // SAFETY: Lock guarantees the isolate is locked; the handle is a valid String handle.
        unsafe { Local::from_ffi(ffi::local_string_internalize(isolate.as_ffi(), self.raw())) }
    }

    /// Returns `true` if the string has a flat (contiguous) internal representation.
    ///
    /// A flat string stores its characters in a single contiguous buffer, which
    /// is required by some V8 APIs. Newly created strings are usually flat; cons
    /// strings produced by concatenation may not be.
    ///
    /// Note: This method is available via a Cloudflare-specific V8 patch
    /// (`0029-Add-v8-String-IsFlat-API.patch`).
    ///
    /// Corresponds to `v8::String::IsFlat()`.
    #[inline]
    pub fn is_flat(&self) -> bool {
        // SAFETY: the handle is a valid V8 String handle.
        unsafe { ffi::local_string_is_flat(self.raw()) }
    }
}

// =============================================================================
// `Boolean`
// =============================================================================

impl Boolean {
    /// Returns the `true` or `false` singleton.
    ///
    /// Corresponds to `v8::Boolean::New()`.
    pub fn new<'s>(lock: &mut crate::Lock<'s>, value: bool) -> Local<'s, Self> {
        let isolate = lock.isolate();
        // SAFETY: Lock guarantees the isolate is locked and a HandleScope is active.
        unsafe { Local::from_ffi(ffi::local_new_boolean(isolate.as_ffi(), value)) }
    }
}

impl Local<'_, Boolean> {
    /// The boolean this handle refers to.
    ///
    /// Corresponds to `v8::Boolean::Value()`.
    #[inline]
    pub fn value(&self) -> bool {
        // SAFETY: the handle is a valid V8 Boolean handle.
        unsafe { ffi::local_boolean_value(self.raw()) }
    }
}

// =============================================================================
// `Number`
// =============================================================================

impl Number {
    /// Creates a number.
    ///
    /// Corresponds to `v8::Number::New()`.
    pub fn new<'s>(lock: &mut crate::Lock<'s>, value: f64) -> Local<'s, Self> {
        let isolate = lock.isolate();
        // SAFETY: Lock guarantees the isolate is locked and a HandleScope is active.
        unsafe { Local::from_ffi(ffi::local_new_number(isolate.as_ffi(), value)) }
    }
}

impl Local<'_, Number> {
    /// The number this handle refers to.
    ///
    /// Corresponds to `v8::Number::Value()`.
    #[inline]
    pub fn value(&self) -> f64 {
        // SAFETY: the handle is a valid V8 Number handle.
        unsafe { ffi::local_number_value(self.raw()) }
    }
}

// =============================================================================
// `Name` — supertype of `String` and `Symbol`
// =============================================================================

impl Local<'_, Name> {
    /// Returns the identity hash for this name.
    ///
    /// The hash is stable for the lifetime of the name and is never `0`,
    /// but is not guaranteed to be unique across different names.
    ///
    /// Corresponds to `v8::Name::GetIdentityHash()`.
    #[inline]
    pub fn get_identity_hash(&self) -> i32 {
        // SAFETY: the handle is a valid V8 Name handle.
        unsafe { ffi::local_name_get_identity_hash(self.raw()) }
    }
}

// =============================================================================
// `Symbol`
// =============================================================================

impl Symbol {
    /// Creates a new unique Symbol with an optional description.
    ///
    /// The returned symbol is unique — two calls with the same description return
    /// distinct symbols that are not `===` equal. Pass `None` to create a symbol
    /// without a description.
    ///
    /// Corresponds to `v8::Symbol::New()`.
    pub fn new<'s>(
        lock: &mut crate::Lock<'s>,
        description: Option<Local<'s, String>>,
    ) -> Local<'s, Self> {
        let isolate = lock.isolate();
        // SAFETY: Lock guarantees the isolate is locked and a HandleScope is active.
        unsafe {
            let handle = match description {
                Some(desc) => {
                    ffi::local_symbol_new_with_description(isolate.as_ffi(), desc.into_ffi())
                }
                None => ffi::local_symbol_new(isolate.as_ffi()),
            };
            Local::from_ffi(handle)
        }
    }
}

impl<'s> Local<'s, Symbol> {
    /// Returns the description of this symbol, if any.
    ///
    /// Returns `None` if the symbol was created without a description.
    ///
    /// Corresponds to `v8::Symbol::Description()`.
    pub fn description(self, lock: &mut crate::Lock<'s>) -> Option<Local<'s, String>> {
        let isolate = lock.isolate();
        // SAFETY: Lock guarantees the isolate is locked; the handle is a valid Symbol handle,
        // and a description is a string in the lock's scope.
        unsafe {
            Local::from_ffi_maybe(ffi::local_symbol_description(isolate.as_ffi(), self.raw()))
        }
    }
}

impl BigInt {
    /// Creates a `BigInt` with the value of `value`.
    ///
    /// Corresponds to `v8::BigInt::New()`.
    pub fn new<'s>(lock: &mut crate::Lock<'s>, value: i64) -> Local<'s, Self> {
        let isolate = lock.isolate();
        // SAFETY: `lock` proves the isolate is locked and a HandleScope for `'s` is open.
        unsafe { Local::from_ffi(ffi::big_int_new(isolate.as_ffi(), value)) }
    }

    /// Creates a `BigInt` with the value of `value`.
    ///
    /// Corresponds to `v8::BigInt::NewFromUnsigned()`.
    pub fn from_unsigned<'s>(lock: &mut crate::Lock<'s>, value: u64) -> Local<'s, Self> {
        let isolate = lock.isolate();
        // SAFETY: `lock` proves the isolate is locked and a HandleScope for `'s` is open.
        unsafe { Local::from_ffi(ffi::big_int_new_from_unsigned(isolate.as_ffi(), value)) }
    }
}

impl Local<'_, BigInt> {
    /// Returns the value as an `i64`, or `None` if it is out of the range of `i64`.
    ///
    /// Corresponds to `v8::BigInt::Int64Value()`.
    pub fn int64_value(&self) -> Option<i64> {
        let mut lossless = false;
        // SAFETY: the handle is a valid BigInt handle within the current HandleScope.
        let value = unsafe { ffi::local_big_int_int64_value(self.raw(), &mut lossless) };
        lossless.then_some(value)
    }

    /// Returns the value as a `u64`, or `None` if it is out of the range of `u64`.
    ///
    /// Corresponds to `v8::BigInt::Uint64Value()`.
    pub fn uint64_value(&self) -> Option<u64> {
        let mut lossless = false;
        // SAFETY: the handle is a valid BigInt handle within the current HandleScope.
        let value = unsafe { ffi::local_big_int_uint64_value(self.raw(), &mut lossless) };
        lossless.then_some(value)
    }
}

impl Date {
    /// Creates a `Date` in the current context with the time value `time`, in milliseconds
    /// since the epoch. A time outside the range of JavaScript dates gives an invalid date.
    ///
    /// Corresponds to `v8::Date::New()`.
    ///
    /// # Errors
    ///
    /// If V8 throws while creating it, or execution is terminating, the exception stays pending
    /// and the error is one for which [`Error::is_pending`] is true, as for
    /// [`Local::<Function>::call`]. `v8::Date::New()` prepares for executing JavaScript, which
    /// would clear a pending termination.
    #[inline]
    pub fn new<'s>(lock: &mut Lock<'s>, time: f64) -> crate::Result<Local<'s, Self>> {
        call_into_js(lock, |lock| {
            // SAFETY: `lock` proves the isolate is locked, with a context entered and a
            // HandleScope for `'s` open; a non-empty result is a `Date` V8 created in that scope.
            Ok(unsafe { Local::from_ffi_maybe(ffi::date_new(lock.isolate().as_ffi(), time)) })
        })
    }
}

impl Local<'_, Date> {
    /// Returns the time value: milliseconds since the epoch, or NaN for an invalid date.
    ///
    /// Corresponds to `v8::Date::ValueOf()`.
    pub fn value_of(&self) -> f64 {
        // SAFETY: the handle is a valid Date handle within the current HandleScope.
        unsafe { ffi::local_date_value_of(self.raw()) }
    }
}

// =============================================================================
// `Utf8Value`
// =============================================================================

/// Rust equivalent of `v8::String::Utf8Value`.
///
/// Converts any V8 value to its UTF-8 string representation (analogous to calling
/// `.toString()` in JavaScript) and holds the result for the duration of its lifetime.
///
/// The UTF-8 bytes are a **heap-allocated copy** independent of the V8 heap — the data
/// remains valid and stable for the full lifetime of this `Utf8Value` regardless of GC
/// activity.
///
/// If the value cannot be converted to a string (e.g. a `Symbol`), V8 stores a null
/// pointer internally. In that case [`as_ptr`](Self::as_ptr) returns null,
/// [`length`](Self::length) returns `0`, and [`as_bytes`](Self::as_bytes) /
/// [`as_str`](Self::as_str) return empty slices.
///
/// # Example
///
/// ```ignore
/// let utf8 = Utf8Value::new(lock, &value);
/// println!("{}", utf8.as_str().unwrap_or(""));
/// ```
pub struct Utf8Value {
    inner: ffi::Utf8Value,
}

impl Utf8Value {
    /// Constructs a `Utf8Value` by converting `value` to its UTF-8 string representation.
    ///
    /// Produces a heap-allocated copy of the UTF-8 bytes that is independent of the V8
    /// heap. If `value` cannot be converted to a string (e.g. a `Symbol`), the internal
    /// data pointer will be null and [`length`](Self::length) will return `0`.
    ///
    /// Corresponds to `v8::String::Utf8Value(isolate, obj)`.
    pub fn new(lock: &mut Lock, value: &Local<'_, Value>) -> Self {
        // SAFETY: Lock guarantees the isolate is locked and a HandleScope is active.
        let inner = unsafe { ffi::utf8_value_new(lock.isolate().as_ffi(), *value.raw()) };
        Self { inner }
    }

    /// Returns the number of UTF-8 bytes in the string, excluding the null terminator.
    ///
    /// Returns `0` if V8 could not convert the value to a string.
    ///
    /// Corresponds to `v8::String::Utf8Value::length()`.
    #[inline]
    pub fn length(&self) -> usize {
        // SAFETY: self.inner is a valid Utf8Value.
        unsafe { ffi::utf8_value_length(&self.inner) }
    }

    /// Returns a raw pointer to the null-terminated UTF-8 bytes stored in this copy.
    ///
    /// The pointer points into a heap-allocated buffer owned by this `Utf8Value`, not into
    /// V8 memory. It is valid for the lifetime of this `Utf8Value`.
    ///
    /// Returns null if V8 could not convert the value to a string (e.g. a `Symbol`).
    ///
    /// Corresponds to `v8::String::Utf8Value::operator*()`.
    #[inline]
    pub fn as_ptr(&self) -> *const u8 {
        // SAFETY: self.inner is a valid Utf8Value.
        unsafe { ffi::utf8_value_data(&self.inner) }
    }

    /// Returns the UTF-8 content as a byte slice.
    ///
    /// Returns an empty slice if V8 could not convert the value to a string (e.g. a `Symbol`),
    /// in which case `operator*()` returns a null pointer.
    #[inline]
    pub fn as_bytes(&self) -> &[u8] {
        let ptr = self.as_ptr();
        if ptr.is_null() {
            return &[];
        }
        // SAFETY: ptr is non-null and points to length() valid bytes for the lifetime of self.
        unsafe { std::slice::from_raw_parts(ptr, self.length()) }
    }

    /// Returns the UTF-8 content as a `&str`, or `None` if the bytes are not valid UTF-8.
    #[inline]
    pub fn as_str(&self) -> Option<&str> {
        std::str::from_utf8(self.as_bytes()).ok()
    }
}

impl Drop for Utf8Value {
    fn drop(&mut self) {
        let inner = ffi::Utf8Value {
            ptr: self.inner.ptr,
        };
        // SAFETY: self.inner is a valid Utf8Value being released.
        unsafe { ffi::utf8_value_drop(inner) };
    }
}

// Object-specific implementations
impl Object {
    /// Creates a new object with `properties` as its own data properties, in that order.
    ///
    /// Runs no JavaScript: unlike [`Local::<Object>::set`], it calls no setter that a script
    /// defined on `Object.prototype`. Each property is defined as by
    /// `v8::Object::CreateDataProperty` (writable, enumerable and configurable); a later duplicate
    /// key replaces the earlier value.
    ///
    /// # Errors
    ///
    /// If a key is longer than [`String::MAX_LENGTH`], or V8 fails to create it.
    pub fn with_data_properties<'s>(
        lock: &mut Lock<'s>,
        properties: &[(&str, Local<'s, Value>)],
    ) -> crate::Result<Local<'s, Self>> {
        let (keys, values): (Vec<&str>, Vec<Local<'s, Value>>) = properties
            .iter()
            .map(|(key, value)| {
                value.debug_assert_isolate(lock);
                (*key, *value)
            })
            .unzip();
        // SAFETY: lock guarantees the isolate is locked, a context is entered and a HandleScope
        // is active; the values are valid handles in it. The result belongs to that scope.
        let object = unsafe {
            ffi::object_new_with_data_properties(
                lock.isolate().as_ffi(),
                &keys,
                Local::raw_slice(&values),
            )
        }?;
        // SAFETY: as above.
        Ok(unsafe { Local::from_ffi(object) })
    }
}

impl<'s> Local<'s, Object> {
    /// Sets `key` to `value`, like JS `obj[key] = value`.
    ///
    /// # Errors
    ///
    /// If a setter on the prototype chain or a Proxy trap throws, or execution is terminating,
    /// the exception stays pending and the error is one for which [`Error::is_pending`] is
    /// true, as for [`Local::<Function>::call`].
    pub fn set(self, lock: &mut Lock<'s>, key: &str, value: Local<'s, Value>) -> crate::Result<()> {
        self.debug_assert_isolate(lock);
        value.debug_assert_isolate(lock);
        call_into_js(lock, |lock| {
            let mut handle = *self.raw();
            // SAFETY: isolate is valid and locked (guaranteed by Lock); handles are valid in 's.
            let ok = unsafe {
                ffi::local_object_set_property(
                    lock.isolate().as_ffi(),
                    &mut handle,
                    key,
                    value.into_ffi(),
                )
            }?;
            Ok(ok.then_some(()))
        })
    }

    /// Whether `key` is in the object or its prototype chain.
    ///
    /// # Errors
    ///
    /// If a Proxy `has` trap throws, or execution is terminating, the exception stays pending
    /// and the error is one for which [`Error::is_pending`] is true, as for
    /// [`Local::<Function>::call`].
    pub fn has(self, lock: &mut Lock<'s>, key: &str) -> crate::Result<bool> {
        call_into_js(lock, |lock| {
            let mut has = false;
            // SAFETY: isolate is valid and locked (guaranteed by Lock); handle is valid.
            let ok = unsafe {
                ffi::local_object_has_property(lock.isolate().as_ffi(), self.raw(), key, &mut has)
            }?;
            Ok(ok.then_some(has))
        })
    }

    /// Reads `key`, like JS `obj[key]`: an absent property is `undefined`.
    ///
    /// # Errors
    ///
    /// If a getter or Proxy trap throws, or execution is terminating, the exception stays
    /// pending and the error is one for which [`Error::is_pending`] is true, as for
    /// [`Local::<Function>::call`].
    pub fn get(self, lock: &mut Lock<'s>, key: &str) -> crate::Result<Local<'s, Value>> {
        call_into_js(lock, |lock| {
            // SAFETY: isolate is valid and locked (guaranteed by Lock); handle is valid. The
            // result was created in the current HandleScope.
            Ok(unsafe {
                Local::from_ffi_maybe(ffi::local_object_get_property(
                    lock.isolate().as_ffi(),
                    self.raw(),
                    key,
                )?)
            })
        })
    }
}

/// The V8 handles of a [`TracedGlobal`], in a box of their own.
///
/// GC may make the strong handle weak, and V8 then keeps the address of the handle's slot to
/// write a null into it when the value dies, so that address must not change for as long as
/// the handle exists, however the `TracedGlobal` moves, for example when the `Vec` holding it
/// grows. This costs one small allocation per `TracedGlobal`. A [`Global`], which is never weak,
/// holds its handle inline, and gets a slot only when a drop without the isolate lock queues it.
///
/// Both fields are written through a shared reference to the `TracedGlobal`: by V8, during any GC,
/// and by [`GcVisitor::visit_traced_global`]. They are therefore read only under the isolate lock,
/// and every shared access other than the visit copies the strong handle out before it calls
/// into V8, so no reference into the slot is live when a GC could write into it.
pub(crate) struct GlobalSlot {
    /// Strong `v8::Global<v8::Value>` handle; weak while `traced` is set, and nulled by V8
    /// once its value dies.
    pub(crate) strong: UnsafeCell<ffi::Global>,
    /// Weak `v8::TracedReference<v8::Data>` handle used during GC tracing.
    ///
    /// Empty (`ptr == 0`) while the strong handle is strong. Set by a GC visit that finds the
    /// parent `Wrappable` reachable only through tracing, which also makes the strong handle
    /// weak. Rust never resets it; see [`GcVisitor::visit_traced_global`].
    traced: UnsafeCell<ffi::TracedReference>,
}

impl GlobalSlot {
    /// A slot for the strong handle `strong`, which has no traced reference.
    pub(crate) fn new_boxed(strong: ffi::Global) -> Box<Self> {
        Box::new(Self {
            strong: UnsafeCell::new(strong),
            traced: UnsafeCell::new(ffi::TracedReference { ptr: 0 }),
        })
    }
}

/// A persistent handle for a field of a resource, which GC traces so that a cycle through it
/// can be collected.
///
/// While the resource has a strong reference from Rust the handle is strong. Once only its
/// JavaScript wrapper keeps the resource alive, GC makes the handle weak and traces its value
/// instead; see [`GcVisitor::visit_traced_global`]. Its handles therefore live in a box, since
/// V8 keeps the address of a weak handle's slot. A handle that no resource owns is a
/// [`Global`], which needs no box; [`TracedGlobal::from`] turns one into a `TracedGlobal`.
///
/// Opening or cloning it in an isolate other than the one it was created in panics, and it is
/// dropped like a `Global`.
pub struct TracedGlobal<T> {
    /// `ManuallyDrop`, so that the drop can hand the box to the isolate state.
    slot: ManuallyDrop<Box<GlobalSlot>>,
    /// The state of the isolate the handle was created in. Every use checks that the lock
    /// belongs to that isolate, and a drop checks that the isolate is still alive.
    state: Arc<IsolateState>,
    /// `fn() -> T` leaves `Send` and `Sync` to the other fields: a `TracedGlobal` is `Send`, since
    /// it may be dropped on any thread (see [`IsolateState`]), a visit outside GC on a thread
    /// without the isolate lock leaves it alone (see [`GcVisitor::visit_traced_global`]), and every
    /// other use takes a `Lock` of its isolate. The `UnsafeCell`s of its slot keep it from
    /// being `Sync`.
    _marker: PhantomData<fn() -> T>,
}

// Common implementations for all TracedGlobal<T>
impl<T> TracedGlobal<T> {
    /// Creates a persistent handle to `local`.
    pub fn new(lock: &mut Lock<'_>, local: Local<'_, T>) -> Self {
        Global::new(lock, local).into()
    }

    /// Creates a `TracedGlobal` from an FFI handle.
    ///
    /// # Safety
    /// The caller must ensure that `handle` points to a valid V8 persistent handle of the
    /// lock's isolate.
    pub unsafe fn from_ffi(lock: &mut Lock<'_>, handle: ffi::Global) -> Self {
        Self::from_ffi_in(handle, Arc::clone(lock.isolate_state()))
    }

    pub(crate) fn from_ffi_in(handle: ffi::Global, state: Arc<IsolateState>) -> Self {
        Self {
            slot: ManuallyDrop::new(GlobalSlot::new_boxed(handle)),
            state,
            _marker: PhantomData,
        }
    }

    /// A copy of the strong handle, for calls into V8 that read it. The copy owns nothing: it
    /// must not be reset, and is only valid while `self` is. The isolate must be locked by
    /// this thread; see [`GlobalSlot`].
    fn strong_handle(&self) -> ffi::Global {
        // SAFETY: GC writes into the slot only while it runs, never while this read is in
        // progress on the thread holding the lock.
        let ptr = unsafe { (*self.slot.strong.get()).ptr };
        ffi::Global { ptr }
    }

    /// Returns a non-owning copy of the strong FFI handle.
    ///
    /// # Safety
    /// The isolate of this `TracedGlobal` must be locked by the current thread, since GC may write
    /// into the handle's slot. The caller must not reset the copy, nor use it after this
    /// `TracedGlobal` is dropped.
    pub unsafe fn as_ffi(&self) -> ffi::Global {
        self.strong_handle()
    }

    /// Materializes this handle as a `Local` in the scope of `lock`.
    pub fn as_local<'s>(&self, lock: &mut Lock<'s>) -> Local<'s, T> {
        assert_in_isolate_of(lock, &self.state);
        // SAFETY: the handle belongs to the lock's isolate, as checked above.
        unsafe { global_to_local(lock, &self.strong_handle()) }
    }
}

/// Dropping a `TracedGlobal` hands its slot to its isolate's state, which resets the strong handle
/// at once under the isolate lock, at the next lock otherwise, and not at all once the isolate is
/// being disposed. See [`IsolateState`].
impl<T> Drop for TracedGlobal<T> {
    #[expect(unsafe_code, reason = "takes the slot out of the ManuallyDrop")]
    fn drop(&mut self) {
        // SAFETY: `self.slot` is not used again.
        let slot = unsafe { ManuallyDrop::take(&mut self.slot) };
        self.state.drop_slot(slot);
    }
}

/// `TracedGlobal<T>` is a strong GC handle — visited via `GcVisitor::visit_traced_global`.
impl<T> crate::Traced for TracedGlobal<T> {
    fn trace(&self, visitor: &mut GcVisitor) {
        visitor.visit_traced_global(self);
    }
}

// Note: TracedGlobal<T> intentionally does NOT implement the std Clone trait.
// Cloning a V8 persistent handle requires an isolate pointer to create an
// independent handle via v8::Global::New(isolate, original). The Clone trait
// cannot provide this. Use the `clone()` method directly instead.
impl<T> TracedGlobal<T> {
    /// Creates an independent copy of this persistent handle.
    ///
    /// This properly creates a new V8 persistent handle that references the same
    /// JS object. Both the original and clone can be independently dropped.
    ///
    /// The returned clone always starts in **strong mode** (`traced_ptr = 0`),
    /// regardless of whether `self` is currently in traced mode. If the clone
    /// needs to be traced, `GcVisitor::visit_traced_global` will transition it on the
    /// next GC cycle.
    #[must_use]
    pub fn clone(&self, lock: &mut Lock) -> Self {
        assert_in_isolate_of(lock, &self.state);
        // SAFETY: the handle belongs to the lock's isolate, as checked above.
        let handle = unsafe { global_clone(lock, &self.strong_handle()) };
        Self::from_ffi_in(handle, Arc::clone(&self.state))
    }
}

/// A persistent handle that outlives `HandleScope`s, and is always strong: GC never visits it.
///
/// It keeps its value alive for as long as it exists, so a cycle through it is never
/// collected. Use it for handles that Rust code owns outside resources, such as the `Realm`'s
/// resource templates. It has no [`Traced`](crate::Traced) impl, so it cannot be a field of a
/// resource that GC traces; such a field is a [`TracedGlobal`], which [`TracedGlobal::from`]
/// makes of a `Global`. In return a `Global` holds its strong handle inline, with no
/// allocation: V8 keeps the address of a handle's slot only while the handle is weak, which a
/// `Global` never is, so it may move like any other value.
///
/// Opening or cloning it in an isolate other than the one it was created in panics. A drop
/// resets the handle at once under the isolate lock, queues it for the next lock otherwise, and
/// leaves it to `v8::Isolate::Dispose` once the isolate is being disposed.
pub struct Global<T> {
    handle: ffi::Global,
    /// The state of the isolate the handle was created in. Every use checks that the lock
    /// belongs to that isolate, and a drop checks that the isolate is still alive.
    state: Arc<IsolateState>,
    /// `fn() -> T` leaves `Send` and `Sync` to the other fields, which are both. A `Global` is
    /// `Send`, since it may be dropped on any thread (see [`IsolateState`]) and every other use
    /// takes a `Lock` of its isolate, and `Sync` because nothing writes into it through a shared
    /// reference: GC never visits it, and V8 writes into a handle's slot only while the handle
    /// is weak. A shared reference only reads the handle; every use of the node it points to
    /// takes a `Lock` of its isolate, which one thread holds at a time.
    _marker: PhantomData<fn() -> T>,
}

impl<T> Global<T> {
    /// Creates a persistent handle to `local`.
    pub fn new(lock: &mut Lock<'_>, local: Local<'_, T>) -> Self {
        local.debug_assert_isolate(lock);
        // SAFETY: isolate is valid and locked (guaranteed by Lock); the handle is valid, and
        // belongs to the lock's isolate because all open scopes do.
        let handle = unsafe { ffi::local_to_global(lock.isolate().as_ffi(), local.into_ffi()) };
        Self::from_ffi_in(handle, Arc::clone(lock.isolate_state()))
    }

    /// Creates a `Global` from an FFI handle, taking ownership of it.
    ///
    /// # Safety
    /// The caller must ensure that `handle` points to a valid, strong V8 persistent handle of
    /// the lock's isolate, which nothing else resets.
    pub unsafe fn from_ffi(lock: &mut Lock<'_>, handle: ffi::Global) -> Self {
        Self::from_ffi_in(handle, Arc::clone(lock.isolate_state()))
    }

    pub(crate) const fn from_ffi_in(handle: ffi::Global, state: Arc<IsolateState>) -> Self {
        Self {
            handle,
            state,
            _marker: PhantomData,
        }
    }

    /// Returns a non-owning copy of the FFI handle. The caller must not reset the copy, nor use
    /// it after this `Global` is dropped.
    #[must_use]
    pub const fn as_ffi(&self) -> ffi::Global {
        ffi::Global {
            ptr: self.handle.ptr,
        }
    }

    /// Materializes this handle as a `Local` in the scope of `lock`.
    pub fn as_local<'s>(&self, lock: &mut Lock<'s>) -> Local<'s, T> {
        assert_in_isolate_of(lock, &self.state);
        // SAFETY: the handle belongs to the lock's isolate, as checked above.
        unsafe { global_to_local(lock, &self.handle) }
    }

    /// Creates an independent copy of this persistent handle.
    #[must_use]
    pub fn clone(&self, lock: &mut Lock) -> Self {
        assert_in_isolate_of(lock, &self.state);
        // SAFETY: the handle belongs to the lock's isolate, as checked above.
        let handle = unsafe { global_clone(lock, &self.handle) };
        Self::from_ffi_in(handle, Arc::clone(&self.state))
    }

    /// Takes the handle and the isolate state, which the caller becomes responsible for.
    fn into_parts(self) -> (ffi::Global, Arc<IsolateState>) {
        let this = ManuallyDrop::new(self);
        // SAFETY: `this` is never dropped, so the state is moved out of it exactly once.
        let state = unsafe { std::ptr::read(&raw const this.state) };
        (this.as_ffi(), state)
    }

    /// Consumes the `Global`, returning its FFI handle and transferring ownership to the
    /// caller, which becomes responsible for disposing the handle. Snapshot-pipeline use only.
    #[must_use]
    pub fn into_ffi_for_snapshot(self) -> ffi::Global {
        self.into_parts().0
    }
}

impl Global<FunctionTemplate> {
    /// Returns the constructor function for this function template.
    ///
    /// This is the V8 `Function` object that can be called as a constructor or
    /// used to access static methods, analogous to a JavaScript class reference
    /// (e.g., `URL`, `TextEncoder`).
    pub fn as_local_function<'s>(&self, lock: &mut Lock<'s>) -> Local<'s, Function> {
        assert_in_isolate_of(lock, &self.state);
        // SAFETY: the handle belongs to the lock's isolate, as checked above.
        unsafe { function_template_get_function(lock, &self.handle) }
    }
}

/// Dropping a `Global` hands its handle to its isolate's state, as dropping a [`TracedGlobal`]
/// does.
impl<T> Drop for Global<T> {
    fn drop(&mut self) {
        let handle = std::mem::replace(&mut self.handle, ffi::Global { ptr: 0 });
        self.state.drop_global(handle);
    }
}

/// Moves the handle of a `Global` into the slot of a new `TracedGlobal`, which GC may visit.
impl<T> From<Global<T>> for TracedGlobal<T> {
    fn from(global: Global<T>) -> Self {
        let (handle, state) = global.into_parts();
        Self::from_ffi_in(handle, state)
    }
}

/// Checks that `lock` belongs to the isolate of `state`, in which a persistent handle was
/// created.
///
/// # Panics
/// Panics if it does not.
fn assert_in_isolate_of(lock: &mut Lock<'_>, state: &Arc<IsolateState>) {
    assert!(
        Arc::ptr_eq(state, lock.isolate_state()),
        "attempt to use a Global or TracedGlobal in an isolate other than the one it was created in"
    );
}

/// Opens the persistent `handle` in the scope of `lock`.
///
/// # Safety
/// `handle` must be a valid persistent handle of the isolate of `lock`.
unsafe fn global_to_local<'s, T>(lock: &mut Lock<'s>, handle: &ffi::Global) -> Local<'s, T> {
    // SAFETY: isolate is valid and locked (guaranteed by Lock); the handle is a valid handle of
    // its isolate (guaranteed by caller).
    unsafe { Local::from_ffi(ffi::global_to_local(lock.isolate().as_ffi(), handle)) }
}

/// Returns the constructor function of the persistent function template `handle`, in the
/// scope of `lock`.
///
/// # Safety
/// `handle` must be a valid `v8::Global<v8::FunctionTemplate>` of the isolate of `lock`.
pub(crate) unsafe fn function_template_get_function<'s>(
    lock: &mut Lock<'s>,
    handle: &ffi::Global,
) -> Local<'s, Function> {
    // SAFETY: isolate is valid and locked, and a HandleScope is open (guaranteed by Lock); the
    // handle is a valid function template of its isolate (guaranteed by caller).
    unsafe {
        Local::from_ffi(ffi::function_template_get_function(
            lock.isolate().as_ffi(),
            handle,
        ))
    }
}

/// Creates a new persistent handle to the value of `handle`.
///
/// # Safety
/// `handle` must be a valid persistent handle of the isolate of `lock`.
unsafe fn global_clone(lock: &mut Lock<'_>, handle: &ffi::Global) -> ffi::Global {
    // SAFETY: isolate is valid and locked (guaranteed by Lock); the handle is a valid handle of
    // its isolate (guaranteed by caller).
    unsafe { ffi::global_clone(lock.isolate().as_ffi(), handle) }
}

/// The arguments of a V8 function callback.
///
/// `'cb` is the brand of the callback's [`Lock`](crate::Lock): V8 opens a `HandleScope`
/// for every callback, and the arguments, `this` and the return value all live in it.
/// Invariant in `'cb`.
pub struct FunctionCallbackInfo<'cb>(*mut ffi::FunctionCallbackInfo, PhantomData<*mut &'cb ()>);

impl<'cb> FunctionCallbackInfo<'cb> {
    /// Wraps the `FunctionCallbackInfo` of the running callback, whose lock is `lock`.
    ///
    /// # Safety
    /// `info` must be the `FunctionCallbackInfo` V8 passed to the running callback, and
    /// `lock` the lock of the [`CallbackScope`](crate::CallbackScope) opened from it.
    pub(crate) unsafe fn new(
        _lock: &crate::Lock<'cb>,
        info: *mut ffi::FunctionCallbackInfo,
    ) -> Self {
        Self(info, PhantomData)
    }

    /// Returns the V8 isolate associated with this function callback.
    pub fn isolate(&self) -> IsolatePtr {
        // SAFETY: self.0 is a valid FunctionCallbackInfo pointer (guaranteed by constructor).
        unsafe { IsolatePtr::from_ffi(ffi::fci_get_isolate(self.0)) }
    }

    pub fn this(&self) -> Local<'cb, Value> {
        // SAFETY: self.0 is a valid FunctionCallbackInfo pointer (guaranteed by constructor).
        unsafe { Local::from_ffi(ffi::fci_get_this(self.0)) }
    }

    pub fn len(&self) -> usize {
        // SAFETY: self.0 is a valid FunctionCallbackInfo pointer (guaranteed by constructor).
        unsafe { ffi::fci_get_length(self.0) }
    }

    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    /// Returns `true` if the function was called with `new` (or `Reflect.construct`).
    pub fn is_construct_call(&self) -> bool {
        // SAFETY: self.0 is a valid FunctionCallbackInfo pointer (guaranteed by constructor).
        unsafe { ffi::fci_is_construct_call(self.0) }
    }

    pub fn get(&self, index: usize) -> Local<'cb, Value> {
        // SAFETY: self.0 is a valid FunctionCallbackInfo pointer (guaranteed by constructor).
        unsafe { Local::from_ffi(ffi::fci_get_arg(self.0, index)) }
    }

    pub fn set_return_value(&mut self, value: Local<'cb, Value>) {
        // SAFETY: self.0 is a valid FunctionCallbackInfo pointer (guaranteed by constructor).
        unsafe {
            ffi::fci_set_return_value(self.0, value.into_ffi());
        }
    }
}

/// The arguments to a native data property getter, or to the getter of a wildcard property.
///
/// `'cb` is the brand of the callback's [`Lock`](crate::Lock), as for
/// [`FunctionCallbackInfo`]. Invariant in `'cb`.
pub struct PropertyCallbackInfo<'cb>(*mut ffi::PropertyCallbackInfo, PhantomData<*mut &'cb ()>);

impl<'cb> PropertyCallbackInfo<'cb> {
    /// Wraps the `PropertyCallbackInfo` of the running getter, whose lock is `lock`.
    ///
    /// # Safety
    /// `info` must be the `PropertyCallbackInfo` V8 passed to the running getter, and `lock`
    /// the lock of the [`CallbackScope`](crate::CallbackScope) opened from it.
    pub(crate) unsafe fn new(
        _lock: &crate::Lock<'cb>,
        info: *mut ffi::PropertyCallbackInfo,
    ) -> Self {
        Self(info, PhantomData)
    }

    pub fn isolate(&self) -> IsolatePtr {
        // SAFETY: self.0 is valid (guaranteed by constructor).
        unsafe { IsolatePtr::from_ffi(ffi::pci_get_isolate(self.0)) }
    }

    pub fn this(&self) -> Local<'cb, Value> {
        // SAFETY: self.0 is valid (guaranteed by constructor).
        unsafe { Local::from_ffi(ffi::pci_get_this(self.0)) }
    }

    pub fn set_return_value(&mut self, value: Local<'cb, Value>) {
        // SAFETY: self.0 is valid (guaranteed by constructor), and `value` lives in the
        // callback's scope.
        unsafe { ffi::pci_set_return_value(self.0, value.into_ffi()) }
    }
}

/// The `PropertyCallbackInfo` of a wildcard property query, which tells V8 whether the property
/// exists.
///
/// `'cb` is the brand of the callback's [`Lock`](crate::Lock), as for
/// [`FunctionCallbackInfo`]. Invariant in `'cb`.
pub struct PropertyQueryCallbackInfo<'cb>(
    *mut ffi::PropertyQueryCallbackInfo,
    PhantomData<*mut &'cb ()>,
);

impl<'cb> PropertyQueryCallbackInfo<'cb> {
    /// Wraps the `PropertyCallbackInfo` of the running query, whose lock is `lock`.
    ///
    /// # Safety
    /// `info` must be the `PropertyCallbackInfo` V8 passed to the running query, and `lock`
    /// the lock of the [`CallbackScope`](crate::CallbackScope) opened from it.
    pub(crate) unsafe fn new(
        _lock: &crate::Lock<'cb>,
        info: *mut ffi::PropertyQueryCallbackInfo,
    ) -> Self {
        Self(info, PhantomData)
    }

    pub fn isolate(&self) -> IsolatePtr {
        // SAFETY: self.0 is valid (guaranteed by constructor).
        unsafe { IsolatePtr::from_ffi(ffi::pqci_get_isolate(self.0)) }
    }

    /// The object that has the interceptor (V8's `Holder()`), which may be on the prototype
    /// chain of the object whose property is looked up.
    pub fn this(&self) -> Local<'cb, Value> {
        // SAFETY: self.0 is valid (guaranteed by constructor).
        unsafe { Local::from_ffi(ffi::pqci_get_this(self.0)) }
    }
}

/// The arguments to a native data property setter. `get(0)` is the value
/// assigned, so setters share codegen with methods.
///
/// `'cb` is the brand of the callback's [`Lock`](crate::Lock), as for
/// [`FunctionCallbackInfo`]. Invariant in `'cb`.
pub struct PropertySetterCallbackInfo<'cb>(
    *mut ffi::PropertySetterCallbackInfo,
    usize,
    PhantomData<*mut &'cb ()>,
);

impl<'cb> PropertySetterCallbackInfo<'cb> {
    /// Wraps the `PropertyCallbackInfo` of the running setter, whose lock is `lock`.
    ///
    /// # Safety
    /// `info` must be the `PropertyCallbackInfo` V8 passed to the running setter, `value` the
    /// `v8::Local<v8::Value>` V8 passed alongside it, and `lock` the lock of the
    /// [`CallbackScope`](crate::CallbackScope) opened from it.
    pub(crate) unsafe fn new(
        _lock: &crate::Lock<'cb>,
        info: *mut ffi::PropertySetterCallbackInfo,
        value: usize,
    ) -> Self {
        Self(info, value, PhantomData)
    }

    pub fn isolate(&self) -> IsolatePtr {
        // SAFETY: self.0 is valid (guaranteed by constructor).
        unsafe { IsolatePtr::from_ffi(ffi::psci_get_isolate(self.0)) }
    }

    pub fn this(&self) -> Local<'cb, Value> {
        // SAFETY: self.0 is valid (guaranteed by constructor).
        unsafe { Local::from_ffi(ffi::psci_get_this(self.0)) }
    }

    /// # Panics
    /// If `index` is not 0.
    pub fn get(&self, index: usize) -> Local<'cb, Value> {
        assert_eq!(index, 0);
        // SAFETY: self.1 is the live handle V8 passed (guaranteed by constructor).
        unsafe { Local::from_ffi(ffi::Local { ptr: self.1 }) }
    }

    /// Setters return nothing.
    pub fn set_return_value(&mut self, _value: Local<'cb, Value>) {}
}

/// A fat pointer to a `dyn GarbageCollected` trait object, decomposed into its
/// data pointer and vtable pointer halves.
///
/// Rust trait object pointers (`*mut dyn Trait`) are fat pointers with the
/// layout `[data_ptr, vtable_ptr]`. We decompose them so the two halves can be
/// stored in the C++ `Wrappable::data[0..1]` slots (`uintptr_t`).
///
/// # Safety of the transmute
///
/// Rust guarantees that `*mut dyn Trait` is two pointers wide (the "fat
/// pointer" representation). We transmute to `[*mut (); 2]` to split and
/// rejoin. While the *exact* field order (`[data, vtable]`) is not
/// stabilised in a language RFC, it has been the layout since Rust 1.0 and
/// is relied upon by miri, the compiler test suite, and the unstable
/// `ptr_metadata` API. A layout change would break the ecosystem; we add a
/// compile-time size assert as a safety net.
///
/// TODO(rust-lang/rust#81513): Replace the transmute with `std::ptr::metadata`
/// / `std::ptr::from_raw_parts_mut` once the `ptr_metadata` feature is
/// stabilised. That will make the `[data, vtable]` ordering an explicit API
/// contract rather than an assumed layout.
// Fat pointer must be exactly two pointers wide.
const _: () = assert!(
    size_of::<*mut dyn GarbageCollected>() == 2 * size_of::<usize>(),
    "trait object pointer must be two pointers wide",
);

impl Clone for ffi::TraitObjectPtr {
    fn clone(&self) -> Self {
        Self {
            data_ptr: self.data_ptr,
            vtable_ptr: self.vtable_ptr,
            type_id_lo: self.type_id_lo,
            type_id_hi: self.type_id_hi,
        }
    }
}

impl ffi::TraitObjectPtr {
    /// Creates a `TraitObjectPtr` from a raw `*mut dyn GarbageCollected` fat pointer
    /// and the concrete type's `TypeId`.
    pub(crate) fn from_raw(ptr: *mut dyn GarbageCollected, type_id: std::any::TypeId) -> Self {
        // SAFETY: a fat pointer is layout-equivalent to [*mut (); 2].
        let [data, vtable]: [*mut (); 2] = unsafe { std::mem::transmute(ptr) };

        // Casting a fat pointer to *mut () is guaranteed to yield the data pointer.
        // If the transmuted layout ever diverges from [data, vtable], this catches it.
        debug_assert_eq!(
            data.cast::<()>(),
            ptr.cast::<()>(),
            "fat pointer layout assumption violated: expected [data, vtable]"
        );

        assert!(!data.is_null(), "Rc::into_raw returned null");

        // SAFETY: TypeId is 128 bits (two usize on 64-bit), transmute to split halves for FFI storage.
        let [type_id_lo, type_id_hi]: [usize; 2] = unsafe { std::mem::transmute(type_id) };

        Self {
            data_ptr: data as usize,
            vtable_ptr: vtable as usize,
            type_id_lo,
            type_id_hi,
        }
    }

    /// Returns `true` if this trait object has been cleared (data pointer is null).
    pub(crate) fn is_cleared(&self) -> bool {
        self.data_ptr == 0
    }

    /// Reads the trait object pointer from a Wrappable.
    ///
    /// Returns `None` if the trait object has been cleared (resource already dropped).
    fn from_wrappable(wrappable: &ffi::Wrappable) -> Option<&Self> {
        // SAFETY: wrappable is valid (guaranteed by KjRc lifetime).
        let ptr = unsafe { ffi::wrappable_get_trait_object(wrappable) };
        if ptr.is_cleared() { None } else { Some(ptr) }
    }

    /// Returns the stored `TypeId`.
    pub(crate) fn type_id(&self) -> std::any::TypeId {
        // SAFETY: Reconstructing TypeId from the same [lo, hi] halves stored in from_raw.
        unsafe { std::mem::transmute([self.type_id_lo, self.type_id_hi]) }
    }

    /// Returns the object as a typed `NonNull<R>`, or `None` if the trait object has been
    /// cleared or holds a type other than `R`.
    pub(crate) fn resolve<R: 'static>(&self) -> Option<NonNull<R>> {
        if self.is_cleared() || self.type_id() != std::any::TypeId::of::<R>() {
            return None;
        }
        // SAFETY: TypeId matched, so data_ptr is a valid Rc::into_raw pointer to R.
        Some(unsafe { self.data_as::<R>() })
    }

    /// Returns the data pointer as a `NonNull<R>`.
    ///
    /// # Safety
    /// The caller must have verified the `TypeId` matches `R`.
    unsafe fn data_as<R>(&self) -> NonNull<R> {
        // SAFETY: data_ptr is non-null (checked in from_raw) and TypeId was verified by caller.
        unsafe { NonNull::new_unchecked(self.data_ptr as *mut R) }
    }

    /// Reconstructs a shared reference to the `dyn GarbageCollected` object.
    ///
    /// # Safety
    /// The original object must still be alive for lifetime `'a`.
    #[expect(
        clippy::needless_lifetimes,
        reason = "the safety contract refers to the lifetime by name"
    )]
    unsafe fn as_gc_ref<'a>(&'a self) -> &'a dyn GarbageCollected {
        // SAFETY: transmuting [data, vtable] back into a fat pointer.
        unsafe {
            let fat_ptr: *const dyn GarbageCollected =
                std::mem::transmute([self.data_ptr as *const (), self.vtable_ptr as *const ()]);
            &*fat_ptr
        }
    }

    /// Reconstructs the `Rc<dyn GarbageCollected>` and drops it.
    ///
    /// # Safety
    /// The data pointer must have originated from `Rc::into_raw`.
    /// Must only be called once per allocation.
    unsafe fn drop_rc(&self) {
        // SAFETY: transmuting [data, vtable] back into a fat pointer; Rc::from_raw
        // reclaims the allocation originally created by Rc::into_raw in from_raw.
        unsafe {
            let fat_ptr: *const dyn GarbageCollected =
                std::mem::transmute([self.data_ptr as *const (), self.vtable_ptr as *const ()]);
            drop(Rc::from_raw(fat_ptr));
        }
    }

    /// Zeroes the trait object in a Wrappable.
    ///
    /// Called before `drop_rc` so that any re-entrant `wrappable_invoke_trace`
    /// calls during destruction see null and no-op.
    ///
    /// # Safety
    /// Must only be called while holding exclusive access (e.g. via `Pin<&mut Wrappable>`).
    unsafe fn clear_wrappable(wrappable: Pin<&mut ffi::Wrappable>) {
        // SAFETY: wrappable is valid and exclusively accessed (caller holds Pin<&mut>).
        unsafe { ffi::wrappable_clear_trait_object(wrappable) };
    }
}

/// `TraitObjectPtr` → `WrappableRc`: allocates a new Wrappable on the KJ heap,
/// transferring ownership of the `Rc`-backed `dyn GarbageCollected` fat pointer.
///
/// # Safety
/// The data pointer must have come from `Rc::into_raw` and must not be used to
/// reconstruct the Rc after this call (the Wrappable now owns it).
impl From<ffi::TraitObjectPtr> for WrappableRc {
    fn from(ptr: ffi::TraitObjectPtr) -> Self {
        Self {
            // SAFETY: ptr contains a valid Rc::into_raw data pointer (guaranteed by caller).
            handle: unsafe { ffi::wrappable_new(ptr) },
        }
    }
}

unsafe fn wrappable_invoke_drop(wrappable: Pin<&mut ffi::Wrappable>) {
    // SAFETY: wrappable is valid and exclusively accessed (Pin<&mut>).
    // Clear data slots before drop to prevent re-entrant trace during destruction,
    // then drop the Rc<dyn GarbageCollected> to release the resource.
    unsafe {
        let Some(trait_ptr) = ffi::TraitObjectPtr::from_wrappable(wrappable.as_ref().get_ref())
        else {
            return;
        };
        // Clone before clearing — clear_wrappable_data zeroes data_ptr.
        let saved = trait_ptr.clone();
        ffi::TraitObjectPtr::clear_wrappable(wrappable);
        saved.drop_rc();
    }
}

unsafe fn wrappable_invoke_trace(wrappable: &ffi::Wrappable, visitor: *mut ffi::GcVisitor) {
    // SAFETY: wrappable is valid. visitor is a valid C++ GcVisitor pointer.
    unsafe {
        let Some(trait_ptr) = ffi::TraitObjectPtr::from_wrappable(wrappable) else {
            return;
        };
        let mut gc_visitor = GcVisitor::from_ffi(visitor);
        trait_ptr.as_gc_ref().trace(&mut gc_visitor);
    }
}

unsafe fn wrappable_invoke_get_name(wrappable: &ffi::Wrappable) -> &'static str {
    // SAFETY: wrappable is valid.
    unsafe {
        let Some(trait_ptr) = ffi::TraitObjectPtr::from_wrappable(wrappable) else {
            return "";
        };
        // memory_name() returns a &'static CStr (a compile-time NUL-terminated
        // literal). Convert to &str so CXX can pass it as rust::Str. The C++
        // side uses .data()/.size() directly as a kj::StringPtr — no copy.
        trait_ptr.as_gc_ref().memory_name().to_str().unwrap_or("")
    }
}

/// Visitor for garbage collection tracing.
///
/// `GcVisitor` wraps a C++ `jsg::GcVisitor` pointer. All GC visitation logic
/// (strong/traced switching, parent tracking) is handled by the C++ side via
/// `Wrappable::visitRef()`.
#[derive(Debug)]
pub struct GcVisitor {
    pub(crate) handle: ffi::GcVisitor,
}

impl GcVisitor {
    /// Creates a `GcVisitor` from a raw FFI pointer.
    ///
    /// # Safety
    ///
    /// `visitor` must be a valid, non-null pointer to a live `ffi::GcVisitor`.
    pub(crate) unsafe fn from_ffi(visitor: *mut ffi::GcVisitor) -> Self {
        Self {
            handle: ffi::GcVisitor {
                // SAFETY: visitor is a valid, non-null pointer (guaranteed by caller).
                ptr: unsafe { (*visitor).ptr },
            },
        }
    }

    /// Visits a `jsg::Rc<R>` field during GC tracing.
    ///
    /// Delegates to the C++ `Wrappable::visitRef()` which handles all the
    /// strong/traced switching logic and transitive tracing.
    pub fn visit_rc<R: crate::Resource>(&mut self, r: &crate::Rc<R>) {
        r.visit(self);
    }

    /// Visits a `v8::TracedGlobal<T>` field during GC tracing.
    ///
    /// Implements the same strong↔traced dual-mode switching that `jsg::Data`
    /// / `jsg::V8Ref<T>` use in C++. When the parent `Wrappable` has strong
    /// Rust refs the handle stays strong; once all Rust refs are dropped and
    /// only the JS wrapper keeps it alive, the handle is downgraded to a
    /// `v8::TracedReference` that cppgc can follow — allowing GC to detect
    /// and break reference cycles.
    ///
    /// # Traced nodes
    ///
    /// Rust never resets a traced reference. A `TracedGlobal` taken out of its resource
    /// (`Cell::take`, `mem::replace`) is traced by nothing until it is put back, and nothing
    /// tells it that it moved, as a C++ move constructor would, so the next full GC may free
    /// its node while it still points to it. Every visit therefore first lets go of the traced
    /// reference: it clears the pointer without a reset, and makes the strong handle strong
    /// again, since C++ expects it to be weak exactly while a traced reference is set. C++ then
    /// treats the `TracedGlobal` as never traced: it leaves the strong handle strong, or installs a
    /// new node, makes the strong handle weak and traces the node. A drop lets go of the traced
    /// reference in the same way.
    ///
    /// The cost: every visit of a `TracedGlobal` in traced mode allocates a new node, and the old
    /// one, like that of a dropped `TracedGlobal`, lingers until the next full GC; while its value
    /// is young, scavenges keep the value alive, as young traced nodes are scavenger roots. To
    /// avoid that, the isolate could count the GCs that can free traced nodes (full
    /// mark-compacts, and minor mark-sweeps with cppgc's young generation) in GC prologue and
    /// epilogue callbacks, each visit could record the count through which its node is known to
    /// be live, and visits and drops could reuse or reset only the nodes known to be live. A
    /// node seen live before the collection in progress is still there until its marking
    /// completes.
    ///
    /// A visit after the `TracedGlobal`'s isolate is closed does nothing, and neither does a visit
    /// on behalf of another isolate, which debug builds assert against: a `TracedGlobal` belongs in
    /// resources of its own isolate. A visit has no isolate when its resource has neither been
    /// wrapped nor reached by a trace. Such a visit is not part of a GC: it comes from the
    /// resource's first strong reference, which keeps the handle strong, and `jsg::Rc::new`
    /// makes that reference without a `Lock`, possibly on a thread without the isolate lock.
    /// The visit then touches the handles only if this thread holds the lock, and otherwise
    /// leaves them as they are: a weak handle stays weak until a later visit under the lock,
    /// so its value can die meanwhile, after which `TracedGlobal::as_local` panics.
    ///
    /// Accepts `&TracedGlobal<T>` even though it mutates the handles. This is sound because GC
    /// tracing is always single-threaded within a V8 isolate, and `trace` is never
    /// re-entrant on the same object during a GC cycle; see also [`GlobalSlot`].
    ///
    /// # One owner
    ///
    /// The visit makes the handle strong if the parent has strong Rust references, and weak if
    /// only the parent's wrapper keeps it alive. If two owners visit the same `TracedGlobal`, the
    /// last visit wins, and the value can be collected while the other owner still uses it. So
    /// a `TracedGlobal` must have exactly one GC owner, and must not be kept behind an
    /// `std::rc::Rc`; see `README.md` §Safety rules, rule 2.
    pub fn visit_traced_global<T>(&mut self, global: &TracedGlobal<T>) {
        if global.state.is_closed() {
            // The isolate is being torn down, and its handles are left to `Dispose`, as those
            // of a dropped `TracedGlobal` are.
            return;
        }
        // SAFETY: the visitor is valid for the duration of the visit.
        let visiting = unsafe { ffi::gc_visitor_v8_isolate(&raw mut self.handle) };
        if visiting.is_null() {
            // Not a GC visit, but the one for a resource's first strong reference, which
            // `jsg::Rc::new` makes without a `Lock`.
            if !global.state.this_thread_holds_lock() {
                return;
            }
        } else {
            let foreign = !global.state.belongs_to(visiting);
            debug_assert!(
                !foreign,
                "a TracedGlobal was visited on behalf of an isolate other than its own"
            );
            if foreign {
                return;
            }
        }
        // SAFETY: see above, and `GlobalSlot`.
        let (strong, traced) = unsafe {
            (
                &raw mut (*global.slot.strong.get()).ptr,
                &mut *global.slot.traced.get(),
            )
        };
        if traced.ptr != 0 {
            traced.ptr = 0;
            // SAFETY: the strong handle is valid, and the visit runs under the isolate lock.
            unsafe { ffi::global_clear_weak(strong) };
        }
        // SAFETY: the handles are valid handles of the visitor's isolate, which is locked, and
        // stay where they are for as long as they exist.
        unsafe { ffi::wrappable_visit_global(&raw mut self.handle, strong, traced) };
    }
}

/// A safe wrapper around a V8 isolate pointer.
///
/// `IsolatePtr` provides a type-safe abstraction over raw `v8::Isolate*` pointers,
/// ensuring that the pointer is always non-null. This type is `Copy` and can be
/// freely passed around without worrying about ownership.
///
/// # Thread Safety
///
/// V8 isolates are single-threaded. While `IsolatePtr` itself is `Send` and `Sync`
/// (as it's just a pointer wrapper), V8 operations must only be performed on the
/// thread that owns the isolate lock. Use `is_locked()` to verify the current
/// thread holds the lock before performing V8 operations.
///
/// # Example
///
/// ```ignore
/// // Create from raw pointer (unsafe)
/// let isolate = unsafe { v8::IsolatePtr::from_ffi(raw_ptr) };
///
/// // Check if locked before V8 operations
/// assert!(unsafe { isolate.is_locked() });
///
/// // Get raw pointer for FFI calls
/// let ptr = isolate.as_ffi();
/// ```
#[derive(Clone, Copy, Debug)]
pub struct IsolatePtr {
    handle: NonNull<ffi::Isolate>,
}

impl IsolatePtr {
    /// Creates an `IsolatePtr` from a raw pointer.
    ///
    /// # Safety
    /// The pointer must be non-null and point to a valid V8 isolate.
    pub unsafe fn from_ffi(handle: *mut ffi::Isolate) -> Self {
        // SAFETY: isolate pointer is valid (guaranteed by caller).
        debug_assert!(unsafe { ffi::isolate_is_locked(handle) });
        Self {
            // SAFETY: handle is non-null (guaranteed by caller).
            handle: unsafe { NonNull::new_unchecked(handle) },
        }
    }

    /// Creates an `IsolatePtr` from a `NonNull` pointer.
    pub fn from_non_null(handle: NonNull<ffi::Isolate>) -> Self {
        // SAFETY: handle is non-null (guaranteed by NonNull) and points to a valid isolate.
        debug_assert!(unsafe { ffi::isolate_is_locked(handle.as_ptr()) });
        Self { handle }
    }

    /// Returns whether this isolate is currently locked by the current thread.
    ///
    /// # Safety
    ///
    /// The caller must ensure the isolate is still valid and not deallocated.
    pub unsafe fn is_locked(&self) -> bool {
        // SAFETY: isolate pointer is valid (guaranteed by caller).
        unsafe { ffi::isolate_is_locked(self.handle.as_ptr()) }
    }

    /// Returns the raw pointer to the V8 isolate.
    pub fn as_ffi(&self) -> *mut ffi::Isolate {
        self.handle.as_ptr()
    }

    /// Returns the `NonNull` pointer to the V8 isolate.
    pub fn as_non_null(&self) -> NonNull<ffi::Isolate> {
        self.handle
    }
}

// =============================================================================
// Wrappable — owned, reference-counted handle
// =============================================================================

/// Owned, reference-counted handle to a C++ `Wrappable` on the KJ heap.
///
/// Encapsulates `KjRc<ffi::Wrappable>` so that modules outside `v8` never
/// reference the CXX-generated `ffi::Wrappable` type directly.
///
/// `Clone` / `Drop` only affect the `KjRc` refcount (`kj::Rc` reference counting).
/// GC strong-ref tracking (`addStrongRef` / `removeStrongRef`) is handled by
/// `Rc<R>`, not here.
#[derive(Clone)]
pub struct WrappableRc {
    handle: kj_rs::KjRc<ffi::Wrappable>,
}

impl WrappableRc {
    /// Unwraps a JavaScript value to get an owned Wrappable handle.
    ///
    /// Returns `None` if the value is not a Rust-tagged Wrappable
    /// (e.g. a C++ JSG object, a plain JS object, or a primitive).
    ///
    /// The C++ `unwrap_resource` returns `None` when the value doesn't contain
    /// a Rust Wrappable.
    #[doc(hidden)]
    pub fn from_js(lock: &mut Lock<'_>, value: Local<'_, Value>) -> Option<Self> {
        // SAFETY: isolate is valid and locked (guaranteed by Lock); value handle is valid.
        let handle: Option<kj_rs::KjRc<ffi::Wrappable>> =
            unsafe { ffi::unwrap_resource(lock.isolate().as_ffi(), value.into_ffi()) }.into();
        handle.map(|handle| Self { handle })
    }

    /// Wraps this Wrappable as a JavaScript object of resource type `R`, using (and on first
    /// use creating) `R`'s cached constructor template.
    ///
    /// Returns `None` if the Wrappable already has a JavaScript wrapper in another isolate. That
    /// is a bug in the caller, which [`resource::wrap`](crate::resource::wrap) turns into a
    /// panic.
    ///
    /// # Errors
    ///
    /// If V8 fails to create the wrapper, or execution is terminating, the exception stays
    /// pending and the error is one for which [`Error::is_pending`] is true, as for
    /// [`Local::<Function>::call`].
    pub(crate) fn to_js<'s, R: Resource>(
        &self,
        lock: &mut Lock<'s>,
    ) -> crate::Result<Option<Local<'s, Value>>> {
        let constructor = lock.realm().get_constructor::<R>().as_ffi();
        let mut wrapped_elsewhere = false;
        // SAFETY: isolate is valid and locked; the constructor is a function template of the
        // isolate, which the Realm's template cache keeps alive. The shim checks for termination
        // itself, as `call_into_js()` would.
        let handle = unsafe {
            ffi::wrap_resource(
                lock.isolate().as_ffi(),
                self.handle.clone(),
                &constructor,
                &mut wrapped_elsewhere,
            )
        };
        if wrapped_elsewhere {
            return Ok(None);
        }
        // SAFETY: the wrapper is created in (or read into) the scope of `lock`.
        let wrapper = unsafe { Local::from_ffi_maybe(handle) }.ok_or_else(Error::pending)?;
        Ok(Some(wrapper))
    }

    /// Attaches this Wrappable to the `this` object in a V8 constructor callback.
    ///
    /// V8 has already created the `this` object from the `FunctionTemplate`'s
    /// `InstanceTemplate`; this method attaches the Wrappable to it via
    /// `CppgcShim` so that instance methods can resolve the resource.
    pub fn attach_to_this(&self, info: &mut FunctionCallbackInfo<'_>) {
        // SAFETY: info is valid for the duration of the callback.
        let pin = unsafe { std::pin::Pin::new_unchecked(&mut *info.0) };
        // SAFETY: The Pin guarantees info is valid. wrap_constructor attaches
        // the Wrappable to args.This() and sets the Rust tag.
        unsafe { ffi::wrappable_attach_wrapper(self.handle.clone(), pin) };
    }

    /// Creates an owning `WrappableRc` from a raw `*const ffi::Wrappable` pointer.
    ///
    /// Increments the `kj::Rc` refcount via `addRefToThis()`.
    ///
    /// # Safety
    /// The pointed-to Wrappable must still be alive.
    pub(crate) unsafe fn from_raw_wrappable(ptr: *const ffi::Wrappable) -> Self {
        // SAFETY: ptr is valid and alive (caller verified via Weak::upgrade).
        // The const-to-mut cast is sound for the same reason as as_pin_mut().
        // wrappable_to_rc calls addRefToThis() which uses interior mutability.
        unsafe {
            let wrappable = Pin::new_unchecked(&mut *ptr.cast_mut());
            Self {
                handle: ffi::wrappable_to_rc(wrappable),
            }
        }
    }

    /// Returns a `Pin<&mut ffi::Wrappable>` for C++ FFI calls.
    ///
    /// Takes `&self` because the mutation target is the C++ `Wrappable` on the
    /// KJ heap (behind the raw pointer in `KjRc`), not the `WrappableRc` wrapper
    /// itself. `KjRc::get()` returns `*const T`; the const-to-mut cast is
    /// required because CXX maps non-const C++ references (`T&`) to
    /// `Pin<&mut T>`. `KjRc::get_mut()` cannot be used because it returns
    /// `None` when the refcount > 1 (the common case).
    ///
    /// # Safety
    ///
    /// The returned `Pin<&mut Wrappable>` must be used transiently — passed
    /// directly into a C++ FFI call and never stored. This prevents aliased
    /// `&mut` references from coexisting. The invariant is enforced by:
    ///
    /// 1. **`pub(crate)` visibility** — only code in this crate can call this.
    /// 2. **Single-threaded V8 isolate** — all callers run on the isolate's
    ///    thread, so no concurrent access is possible.
    /// 3. **No same-object re-entrancy** — the C++ methods called through this
    ///    pin (`addStrongRef`, `removeStrongRef`, `visitRef`) may re-enter Rust
    ///    for *different* Wrappables during GC tracing (e.g. `visitRef` traces
    ///    children, which calls `Traced::trace()` on them), but never
    ///    create a second `Pin<&mut Wrappable>` for the *same* object.
    #[expect(
        clippy::mut_from_ref,
        reason = "Pin<&mut> comes from a raw pointer, not from &self"
    )]
    unsafe fn as_pin_mut(&self) -> Pin<&mut ffi::Wrappable> {
        // SAFETY: KjRc pointer is valid; const-to-mut cast is sound (see doc comment above).
        unsafe { Pin::new_unchecked(&mut *self.handle.get().cast_mut()) }
    }

    /// Visits this Wrappable during GC tracing.
    ///
    /// Takes `&self` because this is called from `Ref::visit(&self)` which is
    /// called from `Traced::trace(&self)`. The mutation target is
    /// the C++ `Wrappable` on the KJ heap, not the `WrappableRc` wrapper.
    pub(crate) fn visit_rc(&self, parent: *mut usize, strong: *mut bool, visitor: &mut GcVisitor) {
        // SAFETY: wrappable, parent, strong, and visitor pointers are all valid (guaranteed by callers).
        unsafe {
            ffi::wrappable_visit_ref(
                self.as_pin_mut(),
                parent,
                strong,
                std::ptr::from_mut(&mut visitor.handle),
            );
        }
    }

    /// Returns a `NonNull` pointer to the underlying `ffi::Wrappable`.
    ///
    /// The `KjRc` always holds a valid, non-null pointer to the Wrappable on
    /// the KJ heap.
    pub(crate) fn as_ptr(&self) -> NonNull<ffi::Wrappable> {
        // SAFETY: KjRc always holds a valid, non-null pointer.
        unsafe { NonNull::new_unchecked(self.handle.get().cast_mut()) }
    }

    /// Increments the strong reference count on the underlying Wrappable.
    ///
    /// Called when a new `Rc<R>` is created (clone, unwrap) to inform the GC
    /// that this Wrappable has an additional strong reference from Rust.
    pub(crate) fn add_strong_ref(&mut self) {
        // SAFETY: wrappable is valid (guaranteed by KjRc lifetime).
        unsafe { ffi::wrappable_add_strong_ref(self.as_pin_mut()) };
    }

    /// Decrements the strong reference count and potentially defers destruction.
    ///
    /// Called when a `Rc<R>` is dropped. Calls `maybeDeferDestruction` on
    /// the C++ side with the ref's current `strong` flag. If `is_strong` is
    /// true, `~RefToDelete` will call `removeStrongRef()`; if false (the ref
    /// was already weakened by GC tracing), it skips the decrement.
    // The bool maps directly to the C++ FFI parameter; an enum would just
    // convert back to bool immediately before crossing the boundary.
    pub(crate) fn remove_strong_ref(&mut self, is_strong: bool) {
        // SAFETY: wrappable is valid (guaranteed by KjRc lifetime).
        unsafe { ffi::wrappable_remove_strong_ref(self.as_pin_mut(), is_strong) };
    }

    /// Returns the current strong reference count from the C++ Wrappable.
    #[cfg(debug_assertions)]
    pub(crate) fn strong_refcount(&self) -> u32 {
        // SAFETY: wrappable pointer is valid (guaranteed by KjRc lifetime).
        unsafe { ffi::wrappable_strong_refcount(&*self.handle.get()) }
    }

    /// Creates one JavaScript function per callback, each with an opaque wrapper of this
    /// Wrappable as its data, which keeps the Wrappable alive while one of them is reachable.
    /// The Wrappable must not have a wrapper yet, and gets one wrapper for all of them.
    ///
    /// While execution is terminating, or an exception is pending, creates nothing and returns
    /// [`Error::pending`]'s error, as [`Local::<Function>::call`] does. V8 fails to create a
    /// function only while execution is terminating.
    pub(crate) fn new_functions<'s, const N: usize>(
        &self,
        lock: &Lock<'s>,
        callbacks: [crate::RawFunctionCallback; N],
    ) -> crate::Result<[Local<'s, Function>; N]> {
        let isolate = lock.isolate().as_ffi();
        // Creating the wrapper opens V8's `PrepareForExecutionScope`, which clears a pending
        // exception or termination, so nothing is created while one is pending.
        // SAFETY: isolate is valid and locked (guaranteed by Lock).
        if unsafe {
            ffi::isolate_is_execution_terminating(isolate)
                || ffi::isolate_has_pending_exception(isolate)
        } {
            return Err(Error::pending());
        }
        // SAFETY: isolate is valid and locked, with a context entered and a HandleScope open
        // (guaranteed by Lock).
        let data = unsafe { ffi::wrappable_attach_opaque_wrapper(isolate, self.handle.clone()) }
            .map_err(|_| Error::pending())?;
        let mut functions = Vec::with_capacity(N);
        for callback in callbacks {
            // SAFETY: as above; `callback` is a V8 function callback and `data` a handle in the
            // lock's scope, in which the function is created.
            let function = unsafe {
                Local::from_ffi_maybe(ffi::function_new(isolate, callback as usize, &data))
            };
            functions.push(function.ok_or_else(Error::pending)?);
        }
        let Ok(functions) = functions.try_into() else {
            unreachable!("one function per callback")
        };
        Ok(functions)
    }

    /// Returns the Rust Wrappable whose opaque wrapper is the running function's data, as
    /// [`new_functions`](Self::new_functions) attaches it, or `None` if the data is anything else.
    pub(crate) fn of_function(args: &FunctionCallbackInfo<'_>) -> Option<Self> {
        // SAFETY: args.0 is the running callback's FunctionCallbackInfo.
        let handle: Option<kj_rs::KjRc<ffi::Wrappable>> =
            unsafe { ffi::fci_get_data_wrappable(args.0) }.into();
        handle.map(|handle| Self { handle })
    }

    /// Resolves the `Rc::into_raw` pointer stored in the Wrappable as a typed `NonNull<R>`.
    ///
    /// Returns `None` if the trait object has been cleared or holds a type other than `R`.
    pub(crate) fn resolve<R: 'static>(&self) -> Option<NonNull<R>> {
        // SAFETY: wrappable pointer is valid (guaranteed by KjRc lifetime).
        let trait_ptr = unsafe {
            let wrappable = &*self.handle.get();
            ffi::TraitObjectPtr::from_wrappable(wrappable)?
        };
        trait_ptr.resolve::<R>()
    }

    /// Resolves the `Rc::into_raw` pointer stored in the Wrappable as a typed `NonNull<R>`.
    ///
    /// Returns `None` if the trait object has been cleared or the stored `TypeId`
    /// does not match `R`, preventing type confusion in all builds.
    #[doc(hidden)]
    pub fn resolve_resource<R: Resource>(&self) -> Option<NonNull<R>> {
        self.resolve::<R>()
    }
}
