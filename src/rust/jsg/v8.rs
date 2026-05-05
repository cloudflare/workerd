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
//! - [`Global<T>`] - Persistent handle that outlives `HandleScope`s
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
//! drops it, which may drop the resource. The Rust `Ref<R>` smart pointer holds its own
//! `Rc<R>` plus a `WrappableRc` (`KjRc<Wrappable>`) for reference-counted ownership. On
//! drop, `wrappable_remove_strong_ref()` handles GC cleanup via `maybeDeferDestruction()`,
//! then the `WrappableRc` drop decrements the `kj::Rc` refcount.

use std::any::TypeId;
use std::cell::UnsafeCell;
use std::fmt::Display;
use std::marker::PhantomData;
use std::num::NonZeroUsize;
use std::pin::Pin;
use std::ptr::NonNull;
use std::rc::Rc;

use crate::Error;
use crate::FromJS;
use crate::GarbageCollected;
use crate::Lock;
use crate::Number;
use crate::Resource;
#[expect(clippy::missing_safety_doc)]
#[cxx::bridge(namespace = "workerd::rust::jsg")]
#[expect(
    clippy::fn_params_excessive_bools,
    reason = "bool parameters in FFI are dictated by the C++ interface"
)]
pub mod ffi {
    #[derive(Debug)]
    struct Local {
        ptr: usize,
    }

    /// Mirrors `v8::MaybeLocal<T>`: a single pointer-sized word where `ptr == 0` means empty.
    /// This matches V8's internal layout exactly — `v8::MaybeLocal<T>` holds one `Local<T>`
    /// field which is itself one `internal::Address*` (one pointer word).
    #[derive(Debug)]
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
        pub unsafe fn local_drop(value: Local);
        pub unsafe fn local_clone(value: &Local) -> Local;
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
        ) -> KjMaybe<Local>;
        pub unsafe fn array_buffer_from_backing_store(isolate: *mut Isolate, store: usize)
        -> Local;
        pub unsafe fn local_array_buffer_byte_length(
            isolate: *mut Isolate,
            buffer: &Local,
        ) -> usize;
        pub unsafe fn local_array_buffer_data(isolate: *mut Isolate, buffer: &Local) -> *mut u8;
        pub unsafe fn local_array_buffer_get_backing_store(
            isolate: *mut Isolate,
            buffer: &Local,
        ) -> usize;

        // Local<ArrayBufferView>
        pub unsafe fn local_array_buffer_view_byte_offset(
            isolate: *mut Isolate,
            view: &Local,
        ) -> usize;
        pub unsafe fn local_array_buffer_view_byte_length(
            isolate: *mut Isolate,
            view: &Local,
        ) -> usize;
        pub unsafe fn local_array_buffer_view_buffer_data(
            isolate: *mut Isolate,
            view: &Local,
        ) -> *mut u8;
        pub unsafe fn local_array_buffer_view_get_buffer(
            isolate: *mut Isolate,
            view: &Local,
        ) -> Local;
        pub unsafe fn local_array_buffer_view_element_size(
            isolate: *mut Isolate,
            view: &Local,
        ) -> usize;
        pub unsafe fn local_array_buffer_view_is_integer_type(
            isolate: *mut Isolate,
            view: &Local,
        ) -> bool;

        pub unsafe fn local_eq(lhs: &Local, rhs: &Local) -> bool;
        pub unsafe fn local_has_value(value: &Local) -> bool;
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
        pub unsafe fn local_is_array_buffer(value: &Local) -> bool;
        pub unsafe fn local_is_array_buffer_view(value: &Local) -> bool;
        pub unsafe fn local_is_shared_array_buffer(value: &Local) -> bool;
        pub unsafe fn local_is_function(value: &Local) -> bool;
        pub unsafe fn local_is_symbol(value: &Local) -> bool;
        pub unsafe fn local_is_name(value: &Local) -> bool;
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
        ) -> Local;
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
        pub unsafe fn maybe_local_is_empty(value: &MaybeLocal) -> bool;

        // Local<Name>
        pub unsafe fn local_name_get_identity_hash(value: &Local) -> i32;

        // Local<Symbol>
        pub unsafe fn local_symbol_new(isolate: *mut Isolate) -> Local;
        pub unsafe fn local_symbol_new_with_description(
            isolate: *mut Isolate,
            description: Local,
        ) -> Local;
        pub unsafe fn local_symbol_description(isolate: *mut Isolate, value: &Local) -> MaybeLocal;

        // Local<Function>
        pub unsafe fn local_function_call(
            isolate: *mut Isolate,
            function: &Local,
            recv: &Local,
            args: &[Local],
        ) -> Local;

        // Local<Object>
        pub unsafe fn local_object_set_property(
            isolate: *mut Isolate,
            object: &mut Local,
            key: &str,
            value: Local,
        );
        pub unsafe fn local_object_has_property(
            isolate: *mut Isolate,
            object: &Local,
            key: &str,
        ) -> bool;
        pub unsafe fn local_object_get_property(
            isolate: *mut Isolate,
            object: &Local,
            key: &str,
        ) -> KjMaybe<Local>;

        // Local<Array>
        pub unsafe fn local_array_length(isolate: *mut Isolate, array: &Local) -> u32;
        pub unsafe fn local_array_get(isolate: *mut Isolate, array: &Local, index: u32) -> Local;
        pub unsafe fn local_array_set(
            isolate: *mut Isolate,
            array: &mut Local,
            index: u32,
            value: Local,
        );
        pub unsafe fn local_array_iterate(isolate: *mut Isolate, value: Local) -> Vec<Global>;

        // Local<TypedArray>
        pub unsafe fn local_typed_array_length(isolate: *mut Isolate, array: &Local) -> usize;
        pub unsafe fn local_typed_array_buffer_data(isolate: *mut Isolate, array: &Local) -> usize;
        pub unsafe fn local_typed_array_byte_offset(isolate: *mut Isolate, array: &Local) -> usize;
        pub unsafe fn local_typed_array_byte_length(isolate: *mut Isolate, array: &Local) -> usize;
        pub unsafe fn local_uint8_array_get(
            isolate: *mut Isolate,
            array: &Local,
            index: usize,
        ) -> u8;
        pub unsafe fn local_uint16_array_get(
            isolate: *mut Isolate,
            array: &Local,
            index: usize,
        ) -> u16;
        pub unsafe fn local_uint32_array_get(
            isolate: *mut Isolate,
            array: &Local,
            index: usize,
        ) -> u32;
        pub unsafe fn local_int8_array_get(
            isolate: *mut Isolate,
            array: &Local,
            index: usize,
        ) -> i8;
        pub unsafe fn local_int16_array_get(
            isolate: *mut Isolate,
            array: &Local,
            index: usize,
        ) -> i16;
        pub unsafe fn local_int32_array_get(
            isolate: *mut Isolate,
            array: &Local,
            index: usize,
        ) -> i32;
        pub unsafe fn local_float32_array_get(
            isolate: *mut Isolate,
            array: &Local,
            index: usize,
        ) -> f32;
        pub unsafe fn local_float64_array_get(
            isolate: *mut Isolate,
            array: &Local,
            index: usize,
        ) -> f64;
        pub unsafe fn local_bigint64_array_get(
            isolate: *mut Isolate,
            array: &Local,
            index: usize,
        ) -> i64;
        pub unsafe fn local_biguint64_array_get(
            isolate: *mut Isolate,
            array: &Local,
            index: usize,
        ) -> u64;
        pub unsafe fn local_uint8clamped_array_get(
            isolate: *mut Isolate,
            array: &Local,
            index: usize,
        ) -> u8;

        // Global<T>
        pub unsafe fn global_reset(value: Pin<&mut Global>);
        pub unsafe fn global_clone(isolate: *mut Isolate, value: &Global) -> Global;
        pub unsafe fn global_to_local(isolate: *mut Isolate, value: &Global) -> Local;

        // Wrappable - data access
        #[expect(
            clippy::needless_lifetimes,
            reason = "CXX bridge requires explicit lifetimes on return references"
        )]
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
        /// Resets a `v8::TracedReference`, releasing the weak GC handle.
        /// Must be called when a `Global<T>` is dropped in traced mode to avoid
        /// leaking a live `v8::TracedReference`.
        pub unsafe fn traced_reference_reset(traced: &mut TracedReference);

        // Unwrappers
        pub unsafe fn unwrap_string(isolate: *mut Isolate, value: Local) -> String;
        pub unsafe fn unwrap_boolean(isolate: *mut Isolate, value: Local) -> bool;
        pub unsafe fn unwrap_number(isolate: *mut Isolate, value: Local) -> f64;
        pub unsafe fn unwrap_uint8_array(isolate: *mut Isolate, value: Local) -> Vec<u8>;
        pub unsafe fn unwrap_uint16_array(isolate: *mut Isolate, value: Local) -> Vec<u16>;
        pub unsafe fn unwrap_uint32_array(isolate: *mut Isolate, value: Local) -> Vec<u32>;
        pub unsafe fn unwrap_int8_array(isolate: *mut Isolate, value: Local) -> Vec<i8>;
        pub unsafe fn unwrap_int16_array(isolate: *mut Isolate, value: Local) -> Vec<i16>;
        pub unsafe fn unwrap_int32_array(isolate: *mut Isolate, value: Local) -> Vec<i32>;
        pub unsafe fn unwrap_float32_array(isolate: *mut Isolate, value: Local) -> Vec<f32>;
        pub unsafe fn unwrap_float64_array(isolate: *mut Isolate, value: Local) -> Vec<f64>;
        pub unsafe fn unwrap_bigint64_array(isolate: *mut Isolate, value: Local) -> Vec<i64>;
        pub unsafe fn unwrap_biguint64_array(isolate: *mut Isolate, value: Local) -> Vec<u64>;

        // ArrayBuffer detach/detachable/was-detached
        pub unsafe fn local_array_buffer_detach(isolate: *mut Isolate, buffer: &mut Local);
        pub unsafe fn local_array_buffer_was_detached(
            isolate: *mut Isolate,
            buffer: &Local,
        ) -> bool;
        pub unsafe fn local_array_buffer_is_detachable(
            isolate: *mut Isolate,
            buffer: &Local,
        ) -> bool;

        // Value-level shared check
        pub fn local_array_buffer_is_shared(value: &Local) -> bool;

        // FunctionCallbackInfo
        pub unsafe fn fci_get_isolate(args: *mut FunctionCallbackInfo) -> *mut Isolate;
        pub unsafe fn fci_get_this(args: *mut FunctionCallbackInfo) -> Local;
        pub unsafe fn fci_get_length(args: *mut FunctionCallbackInfo) -> usize;
        pub unsafe fn fci_get_arg(args: *mut FunctionCallbackInfo, index: usize) -> Local;
        pub unsafe fn fci_set_return_value(args: *mut FunctionCallbackInfo, value: Local);

        // Errors
        pub unsafe fn exception_create(
            isolate: *mut Isolate,
            exception_type: ExceptionType,
            message: &str,
        ) -> Local;

        // Isolate
        pub unsafe fn isolate_throw_exception(isolate: *mut Isolate, exception: Local);
        pub unsafe fn isolate_throw_error(isolate: *mut Isolate, message: &str);
        pub unsafe fn isolate_throw_internal_error(isolate: *mut Isolate, internal_message: &str);
        pub unsafe fn isolate_terminate_execution(isolate: *mut Isolate);
        pub unsafe fn isolate_is_locked(isolate: *mut Isolate) -> bool;
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

    pub struct MethodDescriptor {
        name: String,
        callback: usize,
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
    }

    /// Descriptor for a single accessor property. `setter_callback` is `None` for
    /// read-only properties; ignored entirely for `Inspect`.
    pub struct PropertyDescriptor {
        pub name: String,
        pub kind: PropertyKind,
        pub getter_callback: usize,
        /// `None` means read-only (no setter).
        pub setter_callback: KjMaybe<usize>,
    }

    pub struct ResourceDescriptor {
        pub name: String,
        pub constructor: KjMaybe<ConstructorDescriptor>,
        pub methods: Vec<MethodDescriptor>,
        pub properties: Vec<PropertyDescriptor>,
        pub static_methods: Vec<MethodDescriptor>,
        pub static_constants: Vec<StaticConstantDescriptor>,
    }

    // Resources
    unsafe extern "C++" {
        unsafe fn create_resource_template(
            isolate: *mut Isolate,
            descriptor: &ResourceDescriptor,
        ) -> Global /* v8::Global<FunctionTemplate> */;

        pub unsafe fn wrap_resource(
            isolate: *mut Isolate,
            wrappable: KjRc<Wrappable>,
            constructor: &Global, /* v8::Global<FunctionTemplate> */
        ) -> Local /* v8::Local<Value> */;

        pub unsafe fn wrappable_attach_wrapper(
            wrappable: KjRc<Wrappable>,
            args: Pin<&mut FunctionCallbackInfo>,
        );

        pub unsafe fn unwrap_resource(
            isolate: *mut Isolate,
            value: Local, /* v8::LocalValue */
        ) -> KjRc<Wrappable>;

        pub unsafe fn function_template_get_function(
            isolate: *mut Isolate,
            constructor: &Global, /* v8::Global<FunctionTemplate> */
        ) -> Local /* v8::Local<Function> */;

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
            callback: unsafe fn(*mut Isolate) -> Local,
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

// Marker types for Local<T>
#[derive(Debug)]
pub struct Value;
/// Marker for `v8::Name` handles (supertype of `String` and `Symbol`).
#[derive(Debug)]
pub struct Name;
/// Marker for `v8::String` handles.
#[derive(Debug)]
pub struct String;
/// Marker for `v8::Symbol` handles.
#[derive(Debug)]
pub struct Symbol;

impl String {
    /// Maximum length of a V8 string in UTF-16 code units.
    ///
    /// Matches `v8::String::kMaxLength`. Attempting to create a string longer than
    /// this will cause V8 to return an empty `MaybeLocal`.
    pub const MAX_LENGTH: i32 = if cfg!(target_pointer_width = "32") {
        (1 << 28) - 16
    } else {
        (1 << 29) - 24
    };

    /// Returns the empty string singleton.
    ///
    /// Corresponds to `v8::String::Empty()`.
    pub fn empty<'a>(lock: &mut crate::Lock) -> Local<'a, Self> {
        let isolate = lock.isolate();
        // SAFETY: Lock guarantees the isolate is locked and a HandleScope is active.
        unsafe { Local::from_ffi(isolate, ffi::local_string_empty(isolate.as_ffi())) }
    }

    /// Creates a new string from a `&str`.
    ///
    /// Corresponds to `v8::String::NewFromUtf8`.
    pub fn new_from_str<'a>(lock: &mut crate::Lock, data: &str) -> MaybeLocal<'a, Self> {
        Self::new_from_utf8(lock, data.as_bytes())
    }

    /// Creates an internalized string from a `&str`.
    ///
    /// Equal strings will be pointer-equal after internalization, which speeds up
    /// property-key lookups at the cost of a hash-table probe on creation.
    ///
    /// Corresponds to `v8::String::NewFromUtf8` with `kInternalized`.
    pub fn new_internalized_from_str<'a>(
        lock: &mut crate::Lock,
        data: &str,
    ) -> MaybeLocal<'a, Self> {
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
    pub fn new_from_utf8_literal<'a>(
        lock: &mut crate::Lock,
        literal: &'static str,
    ) -> MaybeLocal<'a, Self> {
        assert!(
            literal.len() <= Self::MAX_LENGTH as usize,
            "string literal exceeds v8::String::kMaxLength"
        );
        Self::new_from_utf8(lock, literal.as_bytes())
    }

    /// Creates a new string from UTF-8 data.
    ///
    /// Returns an empty `MaybeLocal` if V8 cannot allocate the string.
    ///
    /// Strings longer than `i32::MAX` bytes are silently truncated to `i32::MAX` bytes
    /// to satisfy V8's `int`-typed length parameter; in practice V8 will reject strings
    /// that large anyway due to heap limits.
    ///
    /// Corresponds to `v8::String::NewFromUtf8`.
    pub fn new_from_utf8<'a>(lock: &mut crate::Lock, data: &[u8]) -> MaybeLocal<'a, Self> {
        let isolate = lock.isolate();
        let len = i32::try_from(data.len()).unwrap_or(i32::MAX);
        // SAFETY: Lock guarantees the isolate is locked and a HandleScope is active;
        // data.as_ptr() and len describe a valid byte slice.
        let handle =
            unsafe { ffi::local_string_new_from_utf8(isolate.as_ffi(), data.as_ptr(), len, false) };
        // SAFETY: handle is a valid MaybeLocal from V8; ptr==0 means empty.
        unsafe { MaybeLocal::from_ffi(handle) }
    }

    /// Creates an internalized string from UTF-8 data.
    ///
    /// Equal strings will be pointer-equal after internalization, which speeds up
    /// property-key lookups at the cost of a hash-table probe on creation.
    ///
    /// Strings longer than `i32::MAX` bytes are silently truncated to `i32::MAX` bytes.
    ///
    /// Corresponds to `v8::String::NewFromUtf8` with `kInternalized`.
    pub fn new_internalized_from_utf8<'a>(
        lock: &mut crate::Lock,
        data: &[u8],
    ) -> MaybeLocal<'a, Self> {
        let isolate = lock.isolate();
        let len = i32::try_from(data.len()).unwrap_or(i32::MAX);
        // SAFETY: Lock guarantees the isolate is locked and a HandleScope is active;
        // data.as_ptr() and len describe a valid byte slice.
        let handle =
            unsafe { ffi::local_string_new_from_utf8(isolate.as_ffi(), data.as_ptr(), len, true) };
        // SAFETY: handle is a valid MaybeLocal from V8; ptr==0 means empty.
        unsafe { MaybeLocal::from_ffi(handle) }
    }

    /// Creates a new string from Latin-1 (one-byte) data.
    ///
    /// Each byte is mapped to the Unicode code point with the same value.
    /// Returns an empty `MaybeLocal` if V8 cannot allocate the string.
    ///
    /// Strings longer than `i32::MAX` bytes are silently truncated to `i32::MAX` bytes.
    ///
    /// Corresponds to `v8::String::NewFromOneByte`.
    pub fn new_from_one_byte<'a>(lock: &mut crate::Lock, data: &[u8]) -> MaybeLocal<'a, Self> {
        let isolate = lock.isolate();
        let len = i32::try_from(data.len()).unwrap_or(i32::MAX);
        // SAFETY: Lock guarantees the isolate is locked; data.as_ptr() and len are valid.
        let handle = unsafe {
            ffi::local_string_new_from_one_byte(isolate.as_ffi(), data.as_ptr(), len, false)
        };
        // SAFETY: handle is a valid MaybeLocal from V8; ptr==0 means empty.
        unsafe { MaybeLocal::from_ffi(handle) }
    }

    /// Creates an internalized string from Latin-1 (one-byte) data.
    ///
    /// Equal strings will be pointer-equal after internalization.
    /// Strings longer than `i32::MAX` bytes are silently truncated to `i32::MAX` bytes.
    ///
    /// Corresponds to `v8::String::NewFromOneByte` with `kInternalized`.
    pub fn new_internalized_from_one_byte<'a>(
        lock: &mut crate::Lock,
        data: &[u8],
    ) -> MaybeLocal<'a, Self> {
        let isolate = lock.isolate();
        let len = i32::try_from(data.len()).unwrap_or(i32::MAX);
        // SAFETY: Lock guarantees the isolate is locked; data.as_ptr() and len are valid.
        let handle = unsafe {
            ffi::local_string_new_from_one_byte(isolate.as_ffi(), data.as_ptr(), len, true)
        };
        // SAFETY: handle is a valid MaybeLocal from V8; ptr==0 means empty.
        unsafe { MaybeLocal::from_ffi(handle) }
    }

    /// Creates a new string from UTF-16 data.
    ///
    /// Returns an empty `MaybeLocal` if V8 cannot allocate the string.
    ///
    /// Strings longer than `i32::MAX` code units are silently truncated to `i32::MAX` code units.
    ///
    /// Corresponds to `v8::String::NewFromTwoByte`.
    pub fn new_from_two_byte<'a>(lock: &mut crate::Lock, data: &[u16]) -> MaybeLocal<'a, Self> {
        let isolate = lock.isolate();
        let len = i32::try_from(data.len()).unwrap_or(i32::MAX);
        // SAFETY: Lock guarantees the isolate is locked; data.as_ptr() and len are valid.
        let handle = unsafe {
            ffi::local_string_new_from_two_byte(isolate.as_ffi(), data.as_ptr(), len, false)
        };
        // SAFETY: handle is a valid MaybeLocal from V8; ptr==0 means empty.
        unsafe { MaybeLocal::from_ffi(handle) }
    }

    /// Creates an internalized string from UTF-16 data.
    ///
    /// Equal strings will be pointer-equal after internalization.
    /// Strings longer than `i32::MAX` code units are silently truncated to `i32::MAX` code units.
    ///
    /// Corresponds to `v8::String::NewFromTwoByte` with `kInternalized`.
    pub fn new_internalized_from_two_byte<'a>(
        lock: &mut crate::Lock,
        data: &[u16],
    ) -> MaybeLocal<'a, Self> {
        let isolate = lock.isolate();
        let len = i32::try_from(data.len()).unwrap_or(i32::MAX);
        // SAFETY: Lock guarantees the isolate is locked; data.as_ptr() and len are valid.
        let handle = unsafe {
            ffi::local_string_new_from_two_byte(isolate.as_ffi(), data.as_ptr(), len, true)
        };
        // SAFETY: handle is a valid MaybeLocal from V8; ptr==0 means empty.
        unsafe { MaybeLocal::from_ffi(handle) }
    }

    /// Concatenates two strings.
    ///
    /// Corresponds to `v8::String::Concat()`.
    pub fn concat<'a>(
        lock: &mut crate::Lock,
        left: Local<'a, Self>,
        right: Local<'a, Self>,
    ) -> Local<'a, Self> {
        let isolate = lock.isolate();
        // SAFETY: Lock guarantees the isolate is locked and a HandleScope is active.
        unsafe {
            Local::from_ffi(
                isolate,
                ffi::local_string_concat(isolate.as_ffi(), left.into_ffi(), right.into_ffi()),
            )
        }
    }
}

impl Display for Local<'_, Value> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        // SAFETY: isolate is valid and locked (guaranteed by the Local's invariant).
        let mut lock = unsafe { Lock::from_isolate_ptr(self.isolate.as_ffi()) };
        match <std::string::String as FromJS>::from_js(&mut lock, self.clone()) {
            Ok(value) => write!(f, "{value}"),
            Err(e) => write!(f, "{e:?}"),
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
    /// Requires `&mut Lock` so that no JavaScript can execute while the slice
    /// is live — JS could otherwise modify the buffer through a `TypedArray`
    /// view, violating the immutability of `&[u8]`.
    ///
    /// # Safety
    /// For shared backing stores (`is_shared() == true`), other agents may
    /// concurrently write to this memory. The caller must ensure no concurrent
    /// writes occur for the duration of the borrow.
    ///
    /// TODO(soon): When there is a safe aliasing model for Rust mutable access,
    /// integrate this method with that somehow.
    #[inline]
    pub unsafe fn as_slice(&self, _lock: &mut crate::Lock) -> &[u8] {
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
    pub unsafe fn as_mut_slice(&mut self, _lock: &mut crate::Lock) -> &mut [u8] {
        if self.is_empty() {
            return &mut [];
        }
        // SAFETY: data() is non-null for non-empty stores; byte_length() bytes are valid.
        unsafe { std::slice::from_raw_parts_mut(self.data(), self.byte_length()) }
    }
}

// Generic Local<'a, T> handle with lifetime
#[derive(Debug)]
pub struct Local<'a, T> {
    handle: ffi::Local,
    isolate: IsolatePtr,
    _marker: PhantomData<(&'a (), T)>,
}

impl<T> Drop for Local<'_, T> {
    fn drop(&mut self) {
        if self.handle.ptr == 0 {
            return;
        }
        let handle = std::mem::replace(&mut self.handle, ffi::Local { ptr: 0 });
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_drop(handle) };
    }
}

// Common implementations for all Local<'a, T>
impl<'a, T> Local<'a, T> {
    /// Creates a `Local` from an FFI handle.
    ///
    /// # Safety
    /// The caller must ensure that `isolate` is valid and that `handle` points to a V8 value
    /// that is still alive within the current `HandleScope`.
    pub unsafe fn from_ffi(isolate: IsolatePtr, handle: ffi::Local) -> Self {
        Local {
            handle,
            isolate,
            _marker: PhantomData,
        }
    }

    /// Consumes this `Local` and returns the underlying FFI handle.
    ///
    /// # Safety
    /// The returned FFI handle must not outlive the `HandleScope` that owns the V8 value.
    pub unsafe fn into_ffi(mut self) -> ffi::Local {
        let handle = ffi::Local {
            ptr: self.handle.ptr,
        };
        self.handle.ptr = 0;
        handle
    }

    /// Returns a reference to the underlying FFI handle.
    ///
    /// # Safety
    /// The caller must ensure the returned reference is not used after this `Local` is dropped.
    pub unsafe fn as_ffi(&self) -> &ffi::Local {
        &self.handle
    }

    /// Returns a mutable reference to the underlying FFI handle.
    ///
    /// # Safety
    /// The caller must ensure the returned reference is not used after this `Local` is dropped.
    pub unsafe fn as_ffi_mut(&mut self) -> &mut ffi::Local {
        &mut self.handle
    }

    pub fn null(lock: &mut crate::Lock) -> Local<'a, Value> {
        // SAFETY: isolate is valid and locked (guaranteed by Lock).
        unsafe { Local::from_ffi(lock.isolate(), ffi::local_new_null(lock.isolate().as_ffi())) }
    }

    pub fn undefined(lock: &mut crate::Lock) -> Local<'a, Value> {
        // SAFETY: isolate is valid and locked (guaranteed by Lock).
        unsafe {
            Local::from_ffi(
                lock.isolate(),
                ffi::local_new_undefined(lock.isolate().as_ffi()),
            )
        }
    }

    pub fn has_value(&self) -> bool {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_has_value(&self.handle) }
    }

    pub fn is_string(&self) -> bool {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_is_string(&self.handle) }
    }

    pub fn is_boolean(&self) -> bool {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_is_boolean(&self.handle) }
    }

    pub fn is_number(&self) -> bool {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_is_number(&self.handle) }
    }

    pub fn is_null(&self) -> bool {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_is_null(&self.handle) }
    }

    pub fn is_undefined(&self) -> bool {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_is_undefined(&self.handle) }
    }

    pub fn is_null_or_undefined(&self) -> bool {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_is_null_or_undefined(&self.handle) }
    }

    /// Returns true if the value is a JavaScript object.
    ///
    /// Note: Unlike JavaScript's `typeof` operator which returns "object" for `null`,
    /// this method returns `false` for `null` values. Use `is_null_or_undefined()`
    /// to check for nullish values separately.
    pub fn is_object(&self) -> bool {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_is_object(&self.handle) }
    }

    /// Returns true if the value is a native JavaScript error.
    pub fn is_native_error(&self) -> bool {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_is_native_error(&self.handle) }
    }

    /// Returns true if the value is a JavaScript array.
    pub fn is_array(&self) -> bool {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_is_array(&self.handle) }
    }

    /// Returns true if the value is a `Uint8Array`.
    pub fn is_uint8_array(&self) -> bool {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_is_uint8_array(&self.handle) }
    }

    /// Returns true if the value is a `Uint16Array`.
    pub fn is_uint16_array(&self) -> bool {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_is_uint16_array(&self.handle) }
    }

    /// Returns true if the value is a `Uint32Array`.
    pub fn is_uint32_array(&self) -> bool {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_is_uint32_array(&self.handle) }
    }

    /// Returns true if the value is an `Int8Array`.
    pub fn is_int8_array(&self) -> bool {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_is_int8_array(&self.handle) }
    }

    /// Returns true if the value is an `Int16Array`.
    pub fn is_int16_array(&self) -> bool {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_is_int16_array(&self.handle) }
    }

    /// Returns true if the value is an `Int32Array`.
    pub fn is_int32_array(&self) -> bool {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_is_int32_array(&self.handle) }
    }

    /// Returns true if the value is a `Float32Array`.
    pub fn is_float32_array(&self) -> bool {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_is_float32_array(&self.handle) }
    }

    /// Returns true if the value is a `Float64Array`.
    pub fn is_float64_array(&self) -> bool {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_is_float64_array(&self.handle) }
    }

    /// Returns true if the value is a `BigInt64Array`.
    pub fn is_bigint64_array(&self) -> bool {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_is_bigint64_array(&self.handle) }
    }

    /// Returns true if the value is a `BigUint64Array`.
    pub fn is_biguint64_array(&self) -> bool {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_is_biguint64_array(&self.handle) }
    }

    /// Returns true if the value is a `Float16Array`.
    pub fn is_float16_array(&self) -> bool {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_is_float16_array(&self.handle) }
    }

    /// Returns true if the value is a `Uint8ClampedArray`.
    pub fn is_uint8clamped_array(&self) -> bool {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_is_uint8clamped_array(&self.handle) }
    }

    /// Returns true if the value is an `ArrayBuffer`.
    pub fn is_array_buffer(&self) -> bool {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_is_array_buffer(&self.handle) }
    }

    /// Returns true if the value is an `ArrayBufferView`
    /// (i.e. any `TypedArray` or `DataView`).
    pub fn is_array_buffer_view(&self) -> bool {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_is_array_buffer_view(&self.handle) }
    }

    /// Returns true if the value is a `SharedArrayBuffer`.
    pub fn is_shared_array_buffer(&self) -> bool {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_is_shared_array_buffer(&self.handle) }
    }

    /// Returns true if the value is a JavaScript function.
    pub fn is_function(&self) -> bool {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_is_function(&self.handle) }
    }

    /// Returns true if the value is a JavaScript Symbol.
    ///
    /// Corresponds to `v8::Value::IsSymbol()`.
    pub fn is_symbol(&self) -> bool {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_is_symbol(&self.handle) }
    }

    /// Returns true if the value is a `v8::Name` (either a `String` or a `Symbol`).
    ///
    /// Corresponds to `v8::Value::IsName()`.
    pub fn is_name(&self) -> bool {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_is_name(&self.handle) }
    }

    /// Returns the JavaScript type of the underlying value as a string.
    ///
    /// Uses V8's native `TypeOf` method which returns the same result as
    /// JavaScript's `typeof` operator: "undefined", "boolean", "number",
    /// "bigint", "string", "symbol", "function", or "object".
    ///
    /// Note: For `null`, this returns "object" (JavaScript's historical behavior).
    pub fn type_of(&self) -> std::string::String {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_type_of(self.isolate.as_ffi(), &self.handle) }
    }
}

impl<T> Clone for Local<'_, T> {
    fn clone(&self) -> Self {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { Self::from_ffi(self.isolate, ffi::local_clone(&self.handle)) }
    }
}

/// Trait for safe checked casts between V8 `Local` handle types.
///
/// Use via the turbofish method on `Local<Value>`:
/// ```ignore
/// let func: Local<Function> = value.try_as::<Function>().unwrap();
/// ```
pub trait As<T>: Sized {
    type Output;

    /// Attempts to cast this handle to the target type.
    /// Returns `None` if the underlying value is not of the target type.
    fn try_as(self) -> Option<Self::Output>;
}

/// Implements `As<$target>` for `Local<Value>` using the given type-check method.
macro_rules! impl_as {
    ($target:ident, $check:ident) => {
        impl<'a> As<$target> for Local<'a, Value> {
            type Output = Local<'a, $target>;

            fn try_as(self) -> Option<Local<'a, $target>> {
                if self.$check() {
                    // SAFETY: We verified the type above.
                    Some(unsafe { Local::from_ffi(self.isolate, self.into_ffi()) })
                } else {
                    None
                }
            }
        }
    };
}

impl_as!(Function, is_function);
impl_as!(Object, is_object);
impl_as!(Array, is_array);
impl_as!(String, is_string);
impl_as!(Symbol, is_symbol);
impl_as!(Name, is_name);
impl_as!(ArrayBuffer, is_array_buffer);
impl_as!(ArrayBufferView, is_array_buffer_view);

// Value-specific implementations
impl<'a> Local<'a, Value> {
    pub fn to_global(self, lock: &'a mut Lock) -> Global<Value> {
        // SAFETY: isolate is valid and locked (guaranteed by Lock); handle is valid.
        unsafe { ffi::local_to_global(lock.isolate().as_ffi(), self.into_ffi()).into() }
    }

    /// Attempts a checked cast to a more specific `Local<T>` type.
    ///
    /// Returns `None` if the value is not of the target type.
    pub fn try_as<T>(self) -> Option<<Self as As<T>>::Output>
    where
        Self: As<T>,
    {
        As::<T>::try_as(self)
    }
}

impl PartialEq for Local<'_, Value> {
    fn eq(&self, other: &Self) -> bool {
        // SAFETY: both handles are valid within the current HandleScope.
        unsafe { ffi::local_eq(&self.handle, &other.handle) }
    }
}

impl PartialEq for Local<'_, String> {
    fn eq(&self, other: &Self) -> bool {
        // SAFETY: Both handles are valid V8 String handles.
        unsafe { ffi::local_string_equals(&self.handle, &other.handle) }
    }
}

impl Eq for Local<'_, String> {}

impl Local<'_, Function> {
    /// Calls this function and converts the result via [`FromJS`].
    ///
    /// `receiver` is the `this` value; pass `None` for `undefined`. Any `Local<T>`
    /// that converts to `Local<Value>` (via `Into`) can be used directly.
    ///
    /// `args` is a slice of `Local<Value>` — use [`ToJS::to_js`] to convert Rust
    /// values, or `.into()` for other `Local<T>` handles.
    ///
    /// # Example
    ///
    /// ```ignore
    /// let result: Number = func.call(lock, None::<Local<Value>>, &[x.to_js(lock), y.to_js(lock)])?;
    /// ```
    pub fn call<'b, R: FromJS, Recv: Into<Local<'b, Value>>>(
        &self,
        lock: &mut Lock,
        receiver: Option<Recv>,
        args: &[Local<'_, Value>],
    ) -> Result<R::ResultType, Error> {
        let recv = receiver.map_or_else(|| Local::<Value>::undefined(lock), Into::into);

        // Build a contiguous array of ffi::Local handles for the FFI call.
        let ffi_args: Vec<ffi::Local> = args
            .iter()
            // SAFETY: each arg handle is valid within the current HandleScope.
            .map(|a| unsafe { ffi::local_clone(a.as_ffi()) })
            .collect();

        // SAFETY: lock guarantees the isolate is locked and a HandleScope is active.
        // self.handle is a valid Local<Function>. recv and ffi_args are valid Local handles.
        let result = unsafe {
            Local::from_ffi(
                lock.isolate(),
                ffi::local_function_call(
                    lock.isolate().as_ffi(),
                    &self.handle,
                    recv.as_ffi(),
                    &ffi_args,
                ),
            )
        };
        R::from_js(lock, result)
    }
}

/// Implements bidirectional `From` conversions between `Local<$from>` and `Local<$to>`.
///
/// All V8 handle subtypes share the same pointer representation, so these casts
/// are just reinterpretations of the handle. The `assert!` verifies the type
/// invariant at runtime — redundant for upcasts (always true) but guards
/// downcasts against misuse.
macro_rules! impl_local_cast {
    ($from:ident -> $to:ident, $check:ident) => {
        impl<'a> From<Local<'a, $from>> for Local<'a, $to> {
            fn from(value: Local<'a, $from>) -> Self {
                assert!(value.$check());
                // SAFETY: V8 subtypes share handle representation; assert verifies the invariant.
                unsafe { Self::from_ffi(value.isolate, value.into_ffi()) }
            }
        }
        impl<'a> From<Local<'a, $to>> for Local<'a, $from> {
            fn from(value: Local<'a, $to>) -> Self {
                assert!(value.$check());
                // SAFETY: V8 subtypes share handle representation; assert verifies the invariant.
                unsafe { Self::from_ffi(value.isolate, value.into_ffi()) }
            }
        }
    };
}

// Upcasts to Value
impl_local_cast!(String -> Value, is_string);
impl_local_cast!(Name -> Value, is_name);
impl_local_cast!(Symbol -> Value, is_symbol);
impl_local_cast!(Object -> Value, is_object);
impl_local_cast!(Function -> Value, is_function);
impl_local_cast!(Array -> Value, is_array);

// String and Symbol are both subtypes of Name
impl_local_cast!(String -> Name, is_string);
impl_local_cast!(Symbol -> Name, is_symbol);
impl_local_cast!(Uint8Array -> Value, is_uint8_array);
impl_local_cast!(Uint16Array -> Value, is_uint16_array);
impl_local_cast!(Uint32Array -> Value, is_uint32_array);
impl_local_cast!(Int8Array -> Value, is_int8_array);
impl_local_cast!(Int16Array -> Value, is_int16_array);
impl_local_cast!(Int32Array -> Value, is_int32_array);
impl_local_cast!(Float32Array -> Value, is_float32_array);
impl_local_cast!(Float64Array -> Value, is_float64_array);
impl_local_cast!(BigInt64Array -> Value, is_bigint64_array);
impl_local_cast!(BigUint64Array -> Value, is_biguint64_array);
impl_local_cast!(Uint8ClampedArray -> Value, is_uint8clamped_array);

// TypedArray base type to Value. Uses `is_array_buffer_view` which also matches
// `DataView`, but this is acceptable because `TypedArray` is only constructed from
// concrete typed array types (Uint8Array, etc.) that are always ArrayBufferViews.
impl_local_cast!(TypedArray -> Value, is_array_buffer_view);

// Concrete typed arrays to TypedArray base
impl_local_cast!(Uint8Array -> TypedArray, is_uint8_array);
impl_local_cast!(Uint16Array -> TypedArray, is_uint16_array);
impl_local_cast!(Uint32Array -> TypedArray, is_uint32_array);
impl_local_cast!(Int8Array -> TypedArray, is_int8_array);
impl_local_cast!(Int16Array -> TypedArray, is_int16_array);
impl_local_cast!(Int32Array -> TypedArray, is_int32_array);
impl_local_cast!(Float32Array -> TypedArray, is_float32_array);
impl_local_cast!(Float64Array -> TypedArray, is_float64_array);
impl_local_cast!(BigInt64Array -> TypedArray, is_bigint64_array);
impl_local_cast!(BigUint64Array -> TypedArray, is_biguint64_array);
impl_local_cast!(Uint8ClampedArray -> TypedArray, is_uint8clamped_array);

// Upcasts to Object (Function, Array, TypedArray are all Object subtypes in V8)
impl_local_cast!(Function -> Object, is_function);
impl_local_cast!(Array -> Object, is_array);
impl_local_cast!(TypedArray -> Object, is_array_buffer_view);

// ArrayBuffer <-> Value, ArrayBuffer <-> Object
impl_local_cast!(ArrayBuffer -> Value, is_array_buffer);
impl_local_cast!(ArrayBuffer -> Object, is_array_buffer);

// ArrayBufferView <-> Value, ArrayBufferView <-> Object
// All concrete TypedArray types and DataView are ArrayBufferViews.
impl_local_cast!(ArrayBufferView -> Value, is_array_buffer_view);
impl_local_cast!(ArrayBufferView -> Object, is_array_buffer_view);

// Concrete typed arrays <-> ArrayBufferView base
impl_local_cast!(Uint8Array -> ArrayBufferView, is_uint8_array);
impl_local_cast!(Uint16Array -> ArrayBufferView, is_uint16_array);
impl_local_cast!(Uint32Array -> ArrayBufferView, is_uint32_array);
impl_local_cast!(Int8Array -> ArrayBufferView, is_int8_array);
impl_local_cast!(Int16Array -> ArrayBufferView, is_int16_array);
impl_local_cast!(Int32Array -> ArrayBufferView, is_int32_array);
impl_local_cast!(Float32Array -> ArrayBufferView, is_float32_array);
impl_local_cast!(Float64Array -> ArrayBufferView, is_float64_array);
impl_local_cast!(BigInt64Array -> ArrayBufferView, is_bigint64_array);
impl_local_cast!(BigUint64Array -> ArrayBufferView, is_biguint64_array);

impl Array {
    /// Creates a new JavaScript array with the given length.
    pub fn new<'a>(lock: &mut crate::Lock, len: usize) -> Local<'a, Self> {
        let isolate = lock.isolate();
        // SAFETY: isolate is valid and locked (guaranteed by Lock).
        unsafe { Local::from_ffi(isolate, ffi::local_new_array(isolate.as_ffi(), len)) }
    }
}

impl Local<'_, Array> {
    /// Returns the length of the array.
    #[inline]
    pub fn len(&self) -> usize {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_array_length(self.isolate.as_ffi(), &self.handle) as usize }
    }

    /// Returns true if the array is empty.
    #[inline]
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    /// Sets an element at the given index.
    pub fn set(&mut self, index: usize, value: Local<'_, Value>) {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe {
            ffi::local_array_set(
                self.isolate.as_ffi(),
                &mut self.handle,
                index as u32,
                value.into_ffi(),
            );
        }
    }

    /// Iterates over array elements using V8's native `Array::Iterate()`.
    /// Returns Global handles because Local handles get reused during iteration.
    pub fn iterate(self) -> Vec<Global<Value>> {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_array_iterate(self.isolate.as_ffi(), self.into_ffi()) }
            .into_iter()
            // SAFETY: each Global handle was created by the C++ side and is valid.
            .map(|g| unsafe { Global::from_ffi(g) })
            .collect()
    }
}

// =============================================================================
// `ArrayBuffer`-specific implementations
// =============================================================================

impl ArrayBuffer {
    /// Creates a new `ArrayBuffer` by copying `data`.
    pub fn new<'a>(lock: &mut crate::Lock, data: &[u8]) -> Local<'a, Self> {
        let isolate = lock.isolate();
        // SAFETY: Lock guarantees the isolate is locked and a HandleScope is active.
        unsafe {
            Local::from_ffi(
                isolate,
                ffi::local_new_array_buffer(isolate.as_ffi(), data.as_ptr(), data.len()),
            )
        }
    }

    /// Attempts to create a new `ArrayBuffer` with `byte_length` bytes using
    /// the given initialization mode (zeroed or uninitialized).
    ///
    /// Returns `None` if allocation fails.
    pub fn new_with_mode<'a>(
        lock: &mut crate::Lock,
        byte_length: usize,
        mode: ffi::BackingStoreInitializationMode,
    ) -> Option<Local<'a, Self>> {
        let isolate = lock.isolate();
        // SAFETY: Lock guarantees the isolate is locked and a HandleScope is active.
        let opt: Option<ffi::Local> =
            unsafe { ffi::array_buffer_new_with_mode(isolate.as_ffi(), byte_length, mode) }.into();
        // SAFETY: isolate is valid and locked; local is a valid handle from V8.
        opt.map(|local| unsafe { Local::from_ffi(isolate, local) })
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
    pub fn from_backing_store<'a>(lock: &mut crate::Lock, store: BackingStore) -> Local<'a, Self> {
        let isolate = lock.isolate();
        // SAFETY: Lock guarantees the isolate is locked and a HandleScope is active.
        // store.ptr is a valid heap-allocated shared_ptr; we pass it by raw pointer
        // so the C++ side can copy the shared_ptr (incrementing refcount).
        unsafe {
            Local::from_ffi(
                isolate,
                ffi::array_buffer_from_backing_store(isolate.as_ffi(), store.ptr.get()),
            )
        }
    }

    /// Creates a new zero-initialized `ArrayBuffer` with the given byte length.
    pub fn new_zeroed<'a>(lock: &mut crate::Lock, byte_length: usize) -> Local<'a, Self> {
        let isolate = lock.isolate();
        // SAFETY: Lock guarantees the isolate is locked and a HandleScope is active.
        unsafe {
            Local::from_ffi(
                isolate,
                ffi::local_new_array_buffer_empty(isolate.as_ffi(), byte_length),
            )
        }
    }
}

impl Local<'_, ArrayBuffer> {
    /// Returns the byte length of this `ArrayBuffer`.
    #[inline]
    pub fn byte_length(&self) -> usize {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_array_buffer_byte_length(self.isolate.as_ffi(), &self.handle) }
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
        unsafe { ffi::local_array_buffer_data(self.isolate.as_ffi(), &self.handle) }
    }

    /// Returns a shared byte slice view into the `ArrayBuffer`'s backing store.
    ///
    /// Zero-copy: the slice points directly into V8-managed memory.
    ///
    /// TODO(soon): When there is a safe aliasing model for Rust mutable access,
    /// integrate this method with that somehow.
    #[inline]
    pub fn as_slice(&self) -> &[u8] {
        if self.is_empty() {
            return &[];
        }
        // SAFETY: data() is non-null for non-empty buffers; byte_length() bytes are valid.
        unsafe { std::slice::from_raw_parts(self.data().cast_const(), self.byte_length()) }
    }

    /// Returns a mutable byte slice view into the `ArrayBuffer`'s backing store.
    ///
    /// Zero-copy: points directly into V8-managed memory.
    ///
    /// Requires `&mut Lock` so that no JavaScript can execute while the mutable
    /// slice is live — the borrow on the lock prevents calling `eval`, invoking
    /// functions, or any other operation that could modify or detach the buffer.
    ///
    /// # Safety
    ///
    /// The caller must ensure that no other live reference (shared or mutable)
    /// into the same `ArrayBuffer` region exists for the duration of the returned
    /// slice. `&mut self` prevents aliasing through *this* `Local` handle, but
    /// two distinct `Local` handles may back the same buffer — the caller is
    /// responsible for ensuring exclusivity.
    ///
    /// TODO(soon): Define a safe aliasing model for Rust mutable access based on
    /// interrogating the backing store shared_ptr being singular for the lifetime
    /// of the current Rust execution model.
    #[doc(hidden)]
    #[inline]
    pub unsafe fn as_mut_slice(&mut self, _lock: &mut crate::Lock) -> &mut [u8] {
        if self.is_empty() {
            return &mut [];
        }
        // SAFETY: caller guarantees exclusive access; data() is non-null for
        // non-empty buffers; byte_length() bytes are valid.
        unsafe { std::slice::from_raw_parts_mut(self.data(), self.byte_length()) }
    }

    /// Copies the `ArrayBuffer` contents into a new `Vec<u8>`.
    pub fn to_vec(&self) -> Vec<u8> {
        self.as_slice().to_vec()
    }

    /// Detaches this `ArrayBuffer`, setting its byte length to zero.
    ///
    /// After detaching, the buffer's data is no longer accessible from JS.
    /// Typed array views backed by this buffer also become zero-length.
    pub fn detach(&mut self) {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_array_buffer_detach(self.isolate.as_ffi(), &mut self.handle) }
    }

    /// Returns `true` if this `ArrayBuffer` has been detached.
    pub fn was_detached(&self) -> bool {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_array_buffer_was_detached(self.isolate.as_ffi(), &self.handle) }
    }

    /// Returns `true` if this `ArrayBuffer` can be detached.
    pub fn is_detachable(&self) -> bool {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_array_buffer_is_detachable(self.isolate.as_ffi(), &self.handle) }
    }

    /// Returns `true` if the underlying V8 value is a `SharedArrayBuffer`.
    ///
    /// This performs a value-level check, allowing code that receives a
    /// `Local<ArrayBuffer>` via cast to distinguish shared buffers.
    pub fn is_shared(&self) -> bool {
        ffi::local_array_buffer_is_shared(&self.handle)
    }

    /// Returns the `BackingStore` for this `ArrayBuffer`.
    ///
    /// The returned `BackingStore` keeps the underlying memory alive independently
    /// of this `Local` handle.
    pub fn backing_store(&self) -> BackingStore {
        // SAFETY: handle is valid within the current HandleScope; the returned pointer
        // is a freshly heap-allocated shared_ptr that we uniquely own.
        let ptr = unsafe {
            ffi::local_array_buffer_get_backing_store(self.isolate.as_ffi(), &self.handle)
        };
        // SAFETY: the FFI guarantees a non-null pointer on success.
        unsafe { BackingStore::from_raw(ptr) }
    }
}

// =============================================================================
// `ArrayBufferView`-specific implementations
// =============================================================================

impl<'a> Local<'a, ArrayBufferView> {
    /// Returns the byte offset of this view within its backing `ArrayBuffer`.
    #[inline]
    pub fn byte_offset(&self) -> usize {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_array_buffer_view_byte_offset(self.isolate.as_ffi(), &self.handle) }
    }

    /// Returns the byte length of this view.
    #[inline]
    pub fn byte_length(&self) -> usize {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_array_buffer_view_byte_length(self.isolate.as_ffi(), &self.handle) }
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
        unsafe { ffi::local_array_buffer_view_element_size(self.isolate.as_ffi(), &self.handle) }
    }

    /// Returns `true` if the element type is an integer type.
    ///
    /// Returns `false` for `Float32Array`, `Float64Array`, and `DataView`.
    #[inline]
    pub fn is_integer_type(&self) -> bool {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_array_buffer_view_is_integer_type(self.isolate.as_ffi(), &self.handle) }
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
        unsafe { ffi::local_array_buffer_view_buffer_data(self.isolate.as_ffi(), &self.handle) }
    }

    /// Returns the backing `ArrayBuffer` of this view.
    pub fn buffer(&self) -> Local<'a, ArrayBuffer> {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe {
            Local::from_ffi(
                self.isolate,
                ffi::local_array_buffer_view_get_buffer(self.isolate.as_ffi(), &self.handle),
            )
        }
    }

    /// Returns a shared byte slice of the view's visible region.
    ///
    /// Zero-copy: points directly into V8-managed memory.
    ///
    /// TODO(soon): When there is a safe aliasing model for Rust mutable access,
    /// integrate this method with that somehow.
    #[inline]
    pub fn as_slice(&self) -> &[u8] {
        if self.is_empty() {
            return &[];
        }
        // SAFETY: buffer_data() + byte_offset() gives the start of the view's region;
        // byte_length() bytes are valid for the lifetime of this Local.
        unsafe {
            let ptr = self.buffer_data().byte_add(self.byte_offset());
            std::slice::from_raw_parts(ptr.cast_const(), self.byte_length())
        }
    }

    /// Returns a mutable byte slice of the view's visible region.
    ///
    /// Zero-copy: points directly into V8-managed memory.
    ///
    /// Requires `&mut Lock` so that no JavaScript can execute while the mutable
    /// slice is live — the borrow on the lock prevents calling `eval`, invoking
    /// functions, or any other operation that could modify or detach the buffer.
    ///
    /// # Safety
    ///
    /// The caller must ensure that no other live reference (shared or mutable)
    /// into the same buffer region exists for the duration of the returned
    /// slice. `&mut self` prevents aliasing through *this* handle, but two
    /// distinct handles backed by the same buffer could alias — the caller is
    /// responsible for ensuring exclusivity.
    ///
    /// TODO(soon): Define a safe aliasing model for Rust mutable access based on
    /// interrogating the backing store shared_ptr being singular for the lifetime
    /// of the current Rust execution model.
    #[doc(hidden)]
    #[inline]
    pub unsafe fn as_mut_slice(&mut self, _lock: &mut crate::Lock) -> &mut [u8] {
        if self.is_empty() {
            return &mut [];
        }
        // SAFETY: caller guarantees exclusive access; same pointer derivation
        // as as_slice; &mut self prevents aliasing through this handle.
        unsafe {
            let ptr = self.buffer_data().byte_add(self.byte_offset());
            std::slice::from_raw_parts_mut(ptr, self.byte_length())
        }
    }

    /// Copies the view's visible byte range into a new `Vec<u8>`.
    pub fn to_vec(&self) -> Vec<u8> {
        self.as_slice().to_vec()
    }
}

// `TypedArray`-specific implementations
impl Local<'_, TypedArray> {
    /// Returns the number of elements in this `TypedArray`.
    pub fn len(&self) -> usize {
        // SAFETY: handle is valid within the current HandleScope.
        unsafe { ffi::local_typed_array_length(self.isolate.as_ffi(), &self.handle) }
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

/// Owning iterator over `TypedArray` elements.
///
/// Created by calling `into_iter()` on a `Local<'a, TypedArray>`.
/// Consumes the array handle.
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
                unsafe { ffi::local_typed_array_length(self.isolate.as_ffi(), &self.handle) }
            }

            /// Returns `true` if the `TypedArray` contains no elements.
            #[inline]
            pub fn is_empty(&self) -> bool {
                self.len() == 0
            }

            /// Returns the element at `index`.
            ///
            /// # Panics
            ///
            /// Panics if `index >= self.len()`.
            #[inline]
            pub fn get(&self, index: usize) -> $elem {
                debug_assert!(index < self.len(), "index out of bounds");
                // SAFETY: handle is valid within the current HandleScope.
                unsafe { ffi::$get_fn(self.isolate.as_ffi(), &self.handle, index) }
            }

            /// Returns the byte offset of this view within its backing `ArrayBuffer`.
            #[inline]
            pub fn byte_offset(&self) -> usize {
                // SAFETY: handle is valid within the current HandleScope.
                unsafe { ffi::local_typed_array_byte_offset(self.isolate.as_ffi(), &self.handle) }
            }

            /// Returns the byte length of this view.
            #[inline]
            pub fn byte_length(&self) -> usize {
                // SAFETY: handle is valid within the current HandleScope.
                unsafe { ffi::local_typed_array_byte_length(self.isolate.as_ffi(), &self.handle) }
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
                unsafe {
                    ffi::local_typed_array_buffer_data(self.isolate.as_ffi(), &self.handle)
                        as *mut $elem
                }
            }

            /// Returns a shared slice view of the typed array's data.
            ///
            /// Zero-copy: points directly into the V8 `ArrayBuffer`'s backing store.
            /// The slice is valid for the lifetime of this `Local` handle.
            ///
            /// TODO(soon): When there is a safe aliasing model for Rust mutable access,
            /// integrate this method with that somehow.
            #[inline]
            pub fn as_slice(&self) -> &[$elem] {
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
            /// The slice is valid for the lifetime of this `Local` handle.
            ///
            /// Requires `&mut Lock` so that no JavaScript can execute while the
            /// mutable slice is live — the borrow on the lock prevents calling
            /// `eval`, invoking functions, or any other operation that could modify
            /// or detach the underlying buffer.
            ///
            /// # Safety
            ///
            /// The caller must ensure that no other live reference (shared or mutable)
            /// into the same `ArrayBuffer` region exists for the duration of the returned
            /// slice. `&mut self` prevents aliasing through *this* `Local` handle, but
            /// two distinct `Local` handles may back the same buffer — the caller is
            /// responsible for ensuring exclusivity.
            ///
            /// TODO(soon): Define a safe aliasing model for Rust mutable access based on
            /// interrogating the backing store shared_ptr being singular for the lifetime
            /// of the current Rust execution model.
            #[doc(hidden)]
            #[inline]
            pub unsafe fn as_mut_slice(&mut self, _lock: &mut crate::Lock) -> &mut [$elem] {
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
                    let value = unsafe {
                        ffi::$get_fn(self.array.isolate.as_ffi(), &self.array.handle, self.index)
                    };
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
                    let value = unsafe {
                        ffi::$get_fn(self.array.isolate.as_ffi(), &self.array.handle, self.len)
                    };
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
                    let value = unsafe {
                        ffi::$get_fn(self.array.isolate.as_ffi(), &self.array.handle, self.index)
                    };
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
                    let value = unsafe {
                        ffi::$get_fn(self.array.isolate.as_ffi(), &self.array.handle, self.len)
                    };
                    Some(value)
                } else {
                    None
                }
            }
        }

        impl<'a> std::iter::FusedIterator for TypedArrayIntoIter<'a, $marker, $elem> {}
    };
}

/// Generates `FromJS` for `Local<'_, T>` typed array types.
macro_rules! impl_typed_array_from_js {
    ($marker:ident, $check:ident, $name:expr) => {
        impl crate::FromJS for Local<'_, $marker> {
            type ResultType = Self;

            fn from_js(_lock: &mut crate::Lock, value: Local<Value>) -> Result<Self, crate::Error> {
                if !value.$check() {
                    return Err(crate::Error::new_type_error(format!(
                        "expected {}, got {}",
                        $name,
                        value.type_of()
                    )));
                }
                // SAFETY: type check passed; V8 handles share the same pointer
                // representation across subtypes within the same HandleScope.
                Ok(unsafe { Self::from_ffi(value.isolate, value.into_ffi()) })
            }
        }
    };
}

impl_typed_array_from_js!(Uint8Array, is_uint8_array, "Uint8Array");
impl_typed_array_from_js!(Uint16Array, is_uint16_array, "Uint16Array");
impl_typed_array_from_js!(Uint32Array, is_uint32_array, "Uint32Array");
impl_typed_array_from_js!(Int8Array, is_int8_array, "Int8Array");
impl_typed_array_from_js!(Int16Array, is_int16_array, "Int16Array");
impl_typed_array_from_js!(Int32Array, is_int32_array, "Int32Array");
impl_typed_array_from_js!(Float32Array, is_float32_array, "Float32Array");
impl_typed_array_from_js!(Float64Array, is_float64_array, "Float64Array");
impl_typed_array_from_js!(BigInt64Array, is_bigint64_array, "BigInt64Array");
impl_typed_array_from_js!(BigUint64Array, is_biguint64_array, "BigUint64Array");
impl_typed_array_from_js!(
    Uint8ClampedArray,
    is_uint8clamped_array,
    "Uint8ClampedArray"
);
impl_typed_array_from_js!(ArrayBuffer, is_array_buffer, "ArrayBuffer");
impl_typed_array_from_js!(ArrayBufferView, is_array_buffer_view, "ArrayBufferView");

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

/// Rust equivalent of `v8::MaybeLocal<T>` — either a `Local<T>` or empty.
///
/// Mirrors V8's layout exactly: one pointer-sized word where `ptr == 0` is empty.
/// No boxing, no `Option` overhead, no stored isolate — this is a direct ABI-compatible
/// view of the `ffi::MaybeLocal` returned across the FFI boundary.
///
/// Methods that produce a `Local<T>` require a `&mut Lock` so they can recover the
/// isolate pointer on demand, matching the pattern used by `Local<T>` constructors.
pub struct MaybeLocal<'a, T> {
    /// The raw FFI handle. `ptr == 0` means empty (matches `v8::MaybeLocal` default).
    handle: ffi::MaybeLocal,
    _marker: PhantomData<(&'a (), T)>,
}

impl<'a, T> MaybeLocal<'a, T> {
    /// Wraps a raw `ffi::MaybeLocal` returned from the FFI layer.
    ///
    /// # Safety
    /// If `handle.ptr != 0`, the handle must point to a live V8 value within the
    /// current `HandleScope` of the active isolate.
    pub unsafe fn from_ffi(handle: ffi::MaybeLocal) -> Self {
        Self {
            handle,
            _marker: PhantomData,
        }
    }

    /// Returns `true` if this `MaybeLocal` is empty.
    pub fn is_empty(&self) -> bool {
        // SAFETY: handle is a valid ffi::MaybeLocal; no isolate or HandleScope needed.
        unsafe { ffi::maybe_local_is_empty(&self.handle) }
    }

    /// Returns the contained value as a `Local<T>` without consuming `self`, or `None` if empty.
    ///
    /// Copies the underlying V8 handle pointer so that both `self` and the returned `Local`
    /// refer to the same V8 value. V8 `Local` handles are non-owning references into the
    /// `HandleScope` stack; `local_clone` is a cheap pointer copy (not a deep clone), and
    /// `local_drop` is a no-op for locals. Use [`into_option`](Self::into_option) to transfer
    /// ownership without the copy when `self` is no longer needed.
    pub fn to_local(&self, lock: &mut crate::Lock) -> Option<Local<'a, T>> {
        // SAFETY: handle is a valid ffi::MaybeLocal; no isolate or HandleScope needed.
        if unsafe { ffi::maybe_local_is_empty(&self.handle) } {
            return None;
        }
        // local_clone is a bitwise pointer copy — both the MaybeLocal and the returned Local
        // refer to the same HandleScope entry. This is safe because Local handles are
        // non-owning and local_drop is a no-op; the HandleScope itself manages the lifetime.
        // SAFETY: handle is non-empty and points to a live V8 value in the current HandleScope.
        let cloned = unsafe {
            ffi::local_clone(&ffi::Local {
                ptr: self.handle.ptr,
            })
        };
        // SAFETY: handle is non-empty, isolate is valid, and cloned is a valid Local.
        Some(unsafe { Local::from_ffi(lock.isolate(), cloned) })
    }

    /// Converts into `Option<Local<'a, T>>`, consuming `self`.
    ///
    /// Transfers ownership of the underlying handle without cloning, which is more efficient
    /// than [`to_local`](Self::to_local) when `self` is no longer needed after the call.
    pub fn into_option(self, lock: &mut crate::Lock) -> Option<Local<'a, T>> {
        if self.handle.ptr == 0 {
            return None;
        }
        // Transfer ownership: zero out the ptr so Drop becomes a no-op, then wrap in Local.
        let ptr = self.handle.ptr;
        // SAFETY: We are taking ownership of the handle; zeroing the ptr prevents double-free.
        let mut this = std::mem::ManuallyDrop::new(self);
        this.handle.ptr = 0;
        // SAFETY: ptr is non-zero (checked above), isolate is valid, handle ownership transferred.
        Some(unsafe { Local::from_ffi(lock.isolate(), ffi::Local { ptr }) })
    }

    /// Unwraps the value, panicking if empty.
    ///
    /// # Panics
    ///
    /// Panics if the `MaybeLocal` is empty.
    pub fn unwrap(self, lock: &mut crate::Lock) -> Local<'a, T> {
        self.into_option(lock).expect("MaybeLocal is empty")
    }

    /// Returns the contained `Local<T>`, or `default` if empty.
    pub fn unwrap_or(self, lock: &mut crate::Lock, default: Local<'a, T>) -> Local<'a, T> {
        self.into_option(lock).unwrap_or(default)
    }
}

impl<T> Drop for MaybeLocal<'_, T> {
    fn drop(&mut self) {
        if self.handle.ptr != 0 {
            let handle = std::mem::replace(&mut self.handle, ffi::MaybeLocal { ptr: 0 });
            // SAFETY: handle is a valid non-empty V8 handle being released.
            unsafe { ffi::local_drop(ffi::Local { ptr: handle.ptr }) };
        }
    }
}

impl<'a, T> From<Option<Local<'a, T>>> for MaybeLocal<'a, T> {
    /// Constructs a `MaybeLocal` from an `Option<Local<T>>`.
    /// `None` produces an empty handle (`ptr == 0`); `Some(local)` reuses its pointer.
    fn from(opt: Option<Local<'a, T>>) -> Self {
        let ptr = match opt {
            None => 0,
            Some(local) => {
                // SAFETY: we take the handle's ptr out without dropping the Local so the
                // handle slot stays alive in the HandleScope.
                let ptr = local.handle.ptr;
                std::mem::forget(local);
                ptr
            }
        };
        Self {
            handle: ffi::MaybeLocal { ptr },
            _marker: PhantomData,
        }
    }
}

impl Local<'_, String> {
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
        // SAFETY: self.handle is a valid V8 String handle.
        unsafe { ffi::local_string_length(&self.handle) }
    }

    /// Returns `true` if the string is represented internally as one-byte (Latin-1).
    ///
    /// Corresponds to `v8::String::IsOneByte()`.
    #[inline]
    pub fn is_one_byte(&self) -> bool {
        // SAFETY: self.handle is a valid V8 String handle.
        unsafe { ffi::local_string_is_one_byte(&self.handle) }
    }

    /// Returns `true` if all characters in the string fit in one byte (Latin-1).
    ///
    /// Unlike `is_one_byte()`, this scans the entire string and may be slow for
    /// two-byte strings that happen to contain only Latin-1 characters.
    ///
    /// Corresponds to `v8::String::ContainsOnlyOneByte()`.
    #[inline]
    pub fn contains_only_one_byte(&self) -> bool {
        // SAFETY: self.handle is a valid V8 String handle.
        unsafe { ffi::local_string_contains_only_one_byte(&self.handle) }
    }

    /// Returns the number of bytes required to encode the string as UTF-8.
    ///
    /// Does not include a null terminator.
    ///
    /// Corresponds to `v8::String::Utf8LengthV2()`.
    #[inline]
    pub fn utf8_length(&self, lock: &mut crate::Lock) -> usize {
        // SAFETY: Lock guarantees the isolate is locked; self.handle is a valid String handle.
        unsafe { ffi::local_string_utf8_length(lock.isolate().as_ffi(), &self.handle) }
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
                &self.handle,
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
                &self.handle,
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
                &self.handle,
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
    pub fn internalize(&self, lock: &mut crate::Lock) -> Self {
        let isolate = lock.isolate();
        // SAFETY: Lock guarantees the isolate is locked; self.handle is a valid String handle.
        unsafe {
            Local::from_ffi(
                isolate,
                ffi::local_string_internalize(isolate.as_ffi(), &self.handle),
            )
        }
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
        // SAFETY: self.handle is a valid V8 String handle.
        unsafe { ffi::local_string_is_flat(&self.handle) }
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
        // SAFETY: self.handle is a valid V8 Name handle.
        unsafe { ffi::local_name_get_identity_hash(&self.handle) }
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
    pub fn new<'a>(
        lock: &mut crate::Lock,
        description: Option<Local<'a, String>>,
    ) -> Local<'a, Self> {
        let isolate = lock.isolate();
        // SAFETY: Lock guarantees the isolate is locked and a HandleScope is active.
        unsafe {
            let handle = match description {
                Some(desc) => {
                    ffi::local_symbol_new_with_description(isolate.as_ffi(), desc.into_ffi())
                }
                None => ffi::local_symbol_new(isolate.as_ffi()),
            };
            Local::from_ffi(isolate, handle)
        }
    }
}

impl<'a> Local<'a, Symbol> {
    /// Returns the description of this symbol, if any.
    ///
    /// Returns `None` if the symbol was created without a description.
    ///
    /// Corresponds to `v8::Symbol::Description()`.
    pub fn description(&self, lock: &mut crate::Lock) -> Option<Local<'a, String>> {
        let isolate = lock.isolate();
        // SAFETY: Lock guarantees the isolate is locked; self.handle is a valid Symbol handle.
        let maybe = unsafe {
            MaybeLocal::<String>::from_ffi(ffi::local_symbol_description(
                isolate.as_ffi(),
                &self.handle,
            ))
        };
        maybe.to_local(lock)
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
        // local_clone produces a cheap handle copy matching V8's by-value constructor semantics.
        let inner = unsafe {
            ffi::utf8_value_new(lock.isolate().as_ffi(), ffi::local_clone(value.as_ffi()))
        };
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
impl<'a> Local<'a, Object> {
    pub fn set(&mut self, lock: &mut Lock, key: &str, value: Local<'a, Value>) {
        // SAFETY: isolate is valid and locked (guaranteed by Lock); handle is valid.
        unsafe {
            ffi::local_object_set_property(
                lock.isolate().as_ffi(),
                &mut self.handle,
                key,
                value.into_ffi(),
            );
        }
    }

    pub fn has(&self, lock: &mut Lock, key: &str) -> bool {
        // SAFETY: isolate is valid and locked (guaranteed by Lock); handle is valid.
        unsafe { ffi::local_object_has_property(lock.isolate().as_ffi(), &self.handle, key) }
    }

    pub fn get(&self, lock: &mut Lock, key: &str) -> Option<Local<'a, Value>> {
        if !self.has(lock, key) {
            return None;
        }

        // SAFETY: isolate is valid and locked (guaranteed by Lock); handle is valid.
        unsafe {
            let maybe_local =
                ffi::local_object_get_property(lock.isolate().as_ffi(), &self.handle, key);
            let opt_local: Option<ffi::Local> = maybe_local.into();
            opt_local.map(|local| Local::from_ffi(lock.isolate(), local))
        }
    }
}

// Generic Global<T> handle without lifetime
pub struct Global<T> {
    handle: ffi::Global,
    /// Weak `v8::TracedReference<v8::Data>` handle used during GC tracing.
    ///
    /// Empty (`ptr == 0`) when the strong handle is active; becomes non-empty
    /// once the parent `Wrappable` is downgraded to traced mode (all strong Rust
    /// `Rc`s dropped). Reset to empty when strong refs are re-acquired.
    ///
    /// `UnsafeCell` is required because `GcVisitor::visit_global` takes `&Global<T>`
    /// (shared reference) but must mutate this field to install the traced handle.
    /// This is sound because GC tracing is always single-threaded within a V8
    /// isolate and `trace` is never re-entrant on the same object.
    traced: UnsafeCell<ffi::TracedReference>,
    _marker: PhantomData<T>,
}

// Common implementations for all Global<T>
impl<T> Global<T> {
    /// Creates a `Global` from an FFI handle.
    ///
    /// # Safety
    /// The caller must ensure that `handle` points to a valid V8 persistent handle.
    pub unsafe fn from_ffi(handle: ffi::Global) -> Self {
        Self {
            handle,
            traced: UnsafeCell::new(ffi::TracedReference { ptr: 0 }),
            _marker: PhantomData,
        }
    }

    /// Returns a reference to the underlying FFI handle.
    ///
    /// # Safety
    /// The caller must ensure the returned reference is not used after this `Global` is dropped.
    pub unsafe fn as_ffi_ref(&self) -> &ffi::Global {
        &self.handle
    }

    /// Returns a mutable reference to the underlying FFI handle.
    ///
    /// # Safety
    /// The caller must ensure the returned reference is not used after this `Global` is dropped.
    pub unsafe fn as_ffi_mut(&mut self) -> *mut ffi::Global {
        &raw mut self.handle
    }

    pub fn as_local<'a>(&self, lock: &mut Lock) -> Local<'a, T> {
        // SAFETY: isolate is valid and locked (guaranteed by Lock); global handle is valid.
        unsafe {
            Local::from_ffi(
                lock.isolate(),
                ffi::global_to_local(lock.isolate().as_ffi(), &self.handle),
            )
        }
    }

    /// Resets this global handle, releasing both the strong and traced V8 handles.
    ///
    /// # Safety
    /// The caller must ensure the global handle is valid.
    pub unsafe fn reset(&mut self) {
        // Reset the strong handle.
        // SAFETY: global handle is valid; Pin is sound because ffi::Global is not moved.
        unsafe {
            ffi::global_reset(Pin::new_unchecked(&mut self.handle));
        }
        // Reset the TracedReference to avoid leaking a live V8 handle when this
        // Global is dropped while in traced mode.
        // SAFETY: traced is valid for the lifetime of self.
        unsafe {
            ffi::traced_reference_reset(self.traced.get_mut());
        }
    }
}

impl<T> From<Local<'_, T>> for Global<T> {
    fn from(local: Local<'_, T>) -> Self {
        Self {
            // SAFETY: isolate is valid (guaranteed by Local's invariant); handle is valid.
            handle: unsafe { ffi::local_to_global(local.isolate.as_ffi(), local.into_ffi()) },
            traced: UnsafeCell::new(ffi::TracedReference { ptr: 0 }),
            _marker: PhantomData,
        }
    }
}

impl Global<FunctionTemplate> {
    /// Returns the constructor function for this function template.
    ///
    /// This is the V8 `Function` object that can be called as a constructor or
    /// used to access static methods, analogous to a JavaScript class reference
    /// (e.g., `URL`, `TextEncoder`).
    pub fn as_local_function<'a>(&self, lock: &mut Lock) -> Local<'a, Function> {
        // SAFETY: `lock` guarantees the isolate is locked and a HandleScope is active.
        // `self.handle` is a valid `Global<FunctionTemplate>` created by `create_resource_template`.
        // The returned `Local` is tied to the current HandleScope via the `'a` lifetime.
        unsafe {
            Local::from_ffi(
                lock.isolate(),
                ffi::function_template_get_function(lock.isolate().as_ffi(), &self.handle),
            )
        }
    }
}

// Allow implicit conversion from ffi::Global
impl<T> From<ffi::Global> for Global<T> {
    fn from(handle: ffi::Global) -> Self {
        Self {
            handle,
            traced: UnsafeCell::new(ffi::TracedReference { ptr: 0 }),
            _marker: PhantomData,
        }
    }
}

impl<T> Drop for Global<T> {
    fn drop(&mut self) {
        // SAFETY: global handle is valid (guaranteed by construction).
        unsafe { self.reset() };
    }
}

/// `Global<T>` is a strong GC handle — visited via `GcVisitor::visit_global`.
impl<T> crate::Traced for Global<T> {
    fn trace(&self, visitor: &mut GcVisitor) {
        visitor.visit_global(self);
    }
}

// Note: Global<T> intentionally does NOT implement the std Clone trait.
// Cloning a V8 persistent handle requires an isolate pointer to create an
// independent handle via v8::Global::New(isolate, original). The Clone trait
// cannot provide this. Use the `clone()` method directly instead.
impl<T> Global<T> {
    /// Creates an independent copy of this persistent handle.
    ///
    /// This properly creates a new V8 persistent handle that references the same
    /// JS object. Both the original and clone can be independently dropped.
    ///
    /// The returned clone always starts in **strong mode** (`traced_ptr = 0`),
    /// regardless of whether `self` is currently in traced mode. If the clone
    /// needs to be traced, `GcVisitor::visit_global` will transition it on the
    /// next GC cycle.
    #[must_use]
    pub fn clone(&self, lock: &mut Lock) -> Self {
        // SAFETY: isolate is valid and locked (guaranteed by Lock); global handle is valid.
        unsafe { ffi::global_clone(lock.isolate().as_ffi(), &self.handle).into() }
    }
}

pub trait ToLocalValue {
    fn to_local<'a>(&self, lock: &mut Lock) -> Local<'a, Value>;
}

macro_rules! impl_to_local_value_integer {
    ($($type:ty),*) => {
        $(
            impl ToLocalValue for $type {
                fn to_local<'a>(&self, lock: &mut Lock) -> Local<'a, Value> {
                    // SAFETY: isolate is valid and locked (guaranteed by Lock).
                    unsafe {
                        Local::from_ffi(
                            lock.isolate(),
                            ffi::local_new_number(lock.isolate().as_ffi(), f64::from(*self)),
                        )
                    }
                }
            }
        )*
    };
}

impl_to_local_value_integer!(u8, u16, u32, i8, i16, i32);

impl ToLocalValue for std::string::String {
    fn to_local<'a>(&self, lock: &mut Lock) -> Local<'a, Value> {
        self.as_str().to_local(lock)
    }
}

impl ToLocalValue for &str {
    fn to_local<'a>(&self, lock: &mut Lock) -> Local<'a, Value> {
        // SAFETY: isolate is valid and locked (guaranteed by Lock).
        unsafe {
            Local::from_ffi(
                lock.isolate(),
                ffi::local_new_string(lock.isolate().as_ffi(), self),
            )
        }
    }
}

impl ToLocalValue for bool {
    fn to_local<'a>(&self, lock: &mut Lock) -> Local<'a, Value> {
        // SAFETY: isolate is valid and locked (guaranteed by Lock).
        unsafe {
            Local::from_ffi(
                lock.isolate(),
                ffi::local_new_boolean(lock.isolate().as_ffi(), *self),
            )
        }
    }
}

impl ToLocalValue for Number {
    fn to_local<'a>(&self, lock: &mut Lock) -> Local<'a, Value> {
        // SAFETY: isolate is valid and locked (guaranteed by Lock).
        unsafe {
            Local::from_ffi(
                lock.isolate(),
                ffi::local_new_number(lock.isolate().as_ffi(), self.value()),
            )
        }
    }
}

pub struct FunctionCallbackInfo<'a>(*mut ffi::FunctionCallbackInfo, PhantomData<&'a ()>);

impl<'a> FunctionCallbackInfo<'a> {
    /// # Safety
    /// The caller must ensure that `info` is a valid pointer to `FunctionCallbackInfo`.
    pub unsafe fn from_ffi(info: *mut ffi::FunctionCallbackInfo) -> Self {
        Self(info, PhantomData)
    }

    /// Returns the V8 isolate associated with this function callback.
    pub fn isolate(&self) -> IsolatePtr {
        // SAFETY: self.0 is a valid FunctionCallbackInfo pointer (guaranteed by constructor).
        unsafe { IsolatePtr::from_ffi(ffi::fci_get_isolate(self.0)) }
    }

    pub fn this(&self) -> Local<'a, Value> {
        // SAFETY: self.0 is a valid FunctionCallbackInfo pointer (guaranteed by constructor).
        unsafe { Local::from_ffi(self.isolate(), ffi::fci_get_this(self.0)) }
    }

    pub fn len(&self) -> usize {
        // SAFETY: self.0 is a valid FunctionCallbackInfo pointer (guaranteed by constructor).
        unsafe { ffi::fci_get_length(self.0) }
    }

    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    pub fn get(&self, index: usize) -> Local<'a, Value> {
        debug_assert!(index <= self.len(), "index out of bounds");
        // SAFETY: self.0 is a valid FunctionCallbackInfo pointer (guaranteed by constructor).
        unsafe { Local::from_ffi(self.isolate(), ffi::fci_get_arg(self.0, index)) }
    }

    pub fn set_return_value(&self, value: Local<Value>) {
        // SAFETY: self.0 is a valid FunctionCallbackInfo pointer (guaranteed by constructor).
        unsafe {
            ffi::fci_set_return_value(self.0, value.into_ffi());
        }
    }
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
    #[expect(clippy::needless_lifetimes)]
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

    /// Visits a `v8::Global<T>` field during GC tracing.
    ///
    /// Implements the same strong↔traced dual-mode switching that `jsg::Data`
    /// / `jsg::V8Ref<T>` use in C++. When the parent `Wrappable` has strong
    /// Rust refs the handle stays strong; once all Rust refs are dropped and
    /// only the JS wrapper keeps it alive, the handle is downgraded to a
    /// `v8::TracedReference` that cppgc can follow — allowing GC to detect
    /// and break reference cycles.
    ///
    /// Accepts `&Global<T>` even though it mutates the `traced` slot inside the
    /// handle. This is safe because:
    /// - GC tracing is always single-threaded within a V8 isolate.
    /// - `trace` is never re-entrant on the same object during a GC cycle.
    /// - The mutation only touches `traced` (the weak traced handle slot),
    ///   never `handle` (the strong handle), so the value observed through any
    ///   other `&Global<T>` reference remains valid.
    pub fn visit_global<T>(&mut self, global: &Global<T>) {
        // SAFETY: `global.traced` is an `UnsafeCell`; accessing it via `get()`
        // is sound under the single-threaded, non-reentrant GC tracing contract
        // documented on `Global<T>::traced`.
        unsafe {
            ffi::wrappable_visit_global(
                &raw mut self.handle,
                (&raw const global.handle.ptr).cast_mut(),
                &mut *global.traced.get(),
            );
        }
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
/// `Ref<R>`, not here.
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
    /// The C++ `unwrap_resource` returns a `KjRc<Wrappable>` whose inner
    /// pointer is null when the value doesn't contain a Rust Wrappable.
    /// We check `get().is_null()` to distinguish that case.
    #[doc(hidden)]
    pub fn from_js(isolate: IsolatePtr, value: Local<Value>) -> Option<Self> {
        // SAFETY: isolate is valid and locked; value handle is valid.
        let handle = unsafe { ffi::unwrap_resource(isolate.as_ffi(), value.into_ffi()) };
        if handle.get().is_null() {
            return None;
        }
        Some(Self { handle })
    }

    /// Wraps this Wrappable as a JavaScript object using the given constructor template.
    pub(crate) fn to_js<'a>(
        &self,
        isolate: IsolatePtr,
        constructor: &Global<FunctionTemplate>,
    ) -> Local<'a, Value> {
        // SAFETY: isolate is valid and locked; constructor global handle is valid.
        unsafe {
            Local::from_ffi(
                isolate,
                ffi::wrap_resource(
                    isolate.as_ffi(),
                    self.handle.clone(),
                    constructor.as_ffi_ref(),
                ),
            )
        }
    }

    /// Attaches this Wrappable to the `this` object in a V8 constructor callback.
    ///
    /// V8 has already created the `this` object from the `FunctionTemplate`'s
    /// `InstanceTemplate`; this method attaches the Wrappable to it via
    /// `CppgcShim` so that instance methods can resolve the resource.
    pub fn attach_to_this(&self, info: &mut FunctionCallbackInfo) {
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
    /// Called when a new `Ref<R>` is created (clone, unwrap) to inform the GC
    /// that this Wrappable has an additional strong reference from Rust.
    pub(crate) fn add_strong_ref(&mut self) {
        // SAFETY: wrappable is valid (guaranteed by KjRc lifetime).
        unsafe { ffi::wrappable_add_strong_ref(self.as_pin_mut()) };
    }

    /// Decrements the strong reference count and potentially defers destruction.
    ///
    /// Called when a `Ref<R>` is dropped. Calls `maybeDeferDestruction` on
    /// the C++ side with the ref's current `strong` flag. If `is_strong` is
    /// true, `~RefToDelete` will call `removeStrongRef()`; if false (the ref
    /// was already weakened by GC tracing), it skips the decrement.
    // The bool maps directly to the C++ FFI parameter; an enum would just
    // convert back to bool immediately before crossing the boundary.
    #[expect(
        clippy::fn_params_excessive_bools,
        reason = "thin wrapper over FFI; bool is dictated by the C++ interface"
    )]
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

    /// Resolves the `Rc::into_raw` pointer stored in the Wrappable as a typed `NonNull<R>`.
    ///
    /// Returns `None` if the trait object has been cleared or the stored `TypeId`
    /// does not match `R`, preventing type confusion in all builds.
    #[doc(hidden)]
    pub fn resolve_resource<R: Resource>(&self) -> Option<NonNull<R>> {
        // SAFETY: wrappable pointer is valid (guaranteed by KjRc lifetime).
        let trait_ptr = unsafe {
            let wrappable = &*self.handle.get();
            ffi::TraitObjectPtr::from_wrappable(wrappable)?
        };

        if trait_ptr.type_id() != std::any::TypeId::of::<R>() {
            return None;
        }

        // SAFETY: TypeId matched, so data_ptr is a valid Rc::into_raw pointer to R.
        Some(unsafe { trait_ptr.data_as::<R>() })
    }
}
