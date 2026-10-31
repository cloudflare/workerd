// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#![allow(
    unsafe_code,
    reason = "the cxx bridge expands to unsafe FFI glue, and `Ref<T>` dereferences the C++ object it \
              owns"
)]

//! The bridge to `jsg-cpp.h`, and [`Ref<T>`](crate::Ref), which needs `unsafe` to dereference
//! the C++ object it holds and to move handles across the bridge.

use std::marker::PhantomData;
use std::ops::Deref;

use jsg::v8;
use kj_rs::KjMaybe;
use kj_rs::KjOwn;

#[expect(
    clippy::missing_safety_doc,
    reason = "cxx does not carry the declarations' `# Safety` docs over to its wrappers"
)]
#[cxx::bridge(namespace = "workerd::rust::jsg_cpp")]
pub mod bridge {
    #[namespace = "workerd::rust::jsg"]
    unsafe extern "C++" {
        include!("workerd/rust/jsg/ffi.h");
        type GcVisitor = jsg::v8::ffi::GcVisitor;
    }

    unsafe extern "C++" {
        include!("workerd/rust/jsg-cpp/jsg-cpp.h");

        /// A `jsg::Ref` to a C++ JSG resource, at a stable address; see `jsg-cpp.h`.
        type CppRef;

        /// A new strong reference to the same object. The isolate must be locked by this
        /// thread, as for copying a `jsg::Ref`.
        pub fn cpp_ref_clone(cpp_ref: &CppRef) -> KjOwn<CppRef>;
        /// The address of the C++ object, of the type the reference was made for.
        pub fn cpp_ref_object(cpp_ref: &CppRef) -> usize;
        /// Visits the reference as C++ GC tracing visits a `jsg::Ref` field.
        ///
        /// # Safety
        ///
        /// `visitor` is the visitor of a visit in progress.
        pub unsafe fn cpp_ref_visit(visitor: *mut GcVisitor, cpp_ref: &CppRef);
    }

    impl KjOwn<CppRef> {}
}

pub use bridge::CppRef;

/// A C++ JSG resource type that Rust can hold as a [`Ref<T>`](Ref), receive as an argument and
/// return to JavaScript.
///
/// `Self` is the opaque type that a crate's `#[cxx::bridge]` declares for the C++ class (`type
/// Fetcher;`), so a `Ref<Self>` dereferences to it and the bridge's per-method shims can take
/// `&Self`. The two functions are that bridge's per-type shims, which instantiate
/// `cppRefTryUnwrap<T>()` and `cppRefWrap<T>()` of `jsg-cpp.h`:
///
/// ```c++
/// inline kj::Maybe<kj::Own<CppRef>> fetcher_try_unwrap(v8::Isolate* isolate, Local value) {
///   return cppRefTryUnwrap<api::Fetcher>(isolate, kj::mv(value));
/// }
/// inline MaybeLocal fetcher_wrap(v8::Isolate* isolate, const CppRef& ref) {
///   return cppRefWrap<api::Fetcher>(isolate, ref);
/// }
/// ```
///
/// # Safety
///
/// `try_unwrap` only returns references made for the C++ class `Self` stands for, and `wrap`
/// instantiates `cppRefWrap` with that class: `Ref<Self>` dereferences the object as `Self`.
pub unsafe trait CppResource: Sized + 'static {
    /// The JavaScript class name, for error messages.
    fn class_name() -> &'static str;

    /// A reference to the object that `value` wraps, if it wraps an instance of the class or of
    /// a subclass. Fails only if the class is not registered with the isolate.
    ///
    /// # Safety
    /// The isolate is locked by this thread with a context entered and a `HandleScope` open, and
    /// `value` is a handle in that scope.
    unsafe fn try_unwrap(
        isolate: *mut v8::ffi::Isolate,
        value: v8::ffi::Local,
    ) -> Result<KjMaybe<KjOwn<CppRef>>, cxx::KjException>;

    /// The JavaScript wrapper of `cpp_ref`, or empty with an exception pending.
    ///
    /// # Safety
    /// As for `try_unwrap`, and `cpp_ref` was made by this type's `try_unwrap`, or by a shim
    /// that returns a `jsg::Ref` of this class.
    unsafe fn wrap(isolate: *mut v8::ffi::Isolate, cpp_ref: &CppRef) -> v8::ffi::MaybeLocal;
}

/// Calls a per-method shim, which runs its C++ in `callCpp()` (`jsg-cpp.h`).
///
/// `shim` is given the isolate and returns whether the C++ completed. If it did not, the
/// exception is pending, and the error is one for which [`jsg::Error::is_pending`] is true. While
/// execution is terminating, `shim` is not called and the error is the same, as for a call into
/// JavaScript.
///
/// # Errors
///
/// A pending error, if the C++ threw or execution is terminating.
pub fn call_cpp(
    lock: &mut jsg::Lock<'_>,
    shim: impl FnOnce(*mut v8::ffi::Isolate) -> bool,
) -> Result<(), jsg::Error> {
    if lock.is_execution_terminating() {
        return Err(jsg::Error::pending());
    }
    if shim(lock.isolate().as_ffi()) {
        Ok(())
    } else {
        Err(jsg::Error::pending())
    }
}

/// [`call_cpp`] for a shim that returns a C++ resource through `callCppForRef<Class>()`
/// (`jsg-cpp.h`), which stores it in the slot `shim` is given.
///
/// # Errors
///
/// As for [`call_cpp`].
///
/// # Safety
///
/// The shim instantiates `callCppForRef` with the class `T` stands for, or a subclass.
pub unsafe fn call_cpp_for_ref<T: CppResource>(
    lock: &mut jsg::Lock<'_>,
    shim: impl FnOnce(*mut v8::ffi::Isolate, &mut KjMaybe<KjOwn<CppRef>>) -> bool,
) -> Result<Option<Ref<T>>, jsg::Error> {
    let mut slot = KjMaybe::None;
    call_cpp(lock, |isolate| shim(isolate, &mut slot))?;
    // SAFETY: the shim made the reference for `T`'s class (this function's contract).
    Ok(Option::from(slot).map(|inner| unsafe { Ref::from_cpp(inner) }))
}

/// A strong reference from Rust to a C++ JSG resource, like a `jsg::Ref<T>` field of a C++
/// resource.
///
/// - It dereferences to `T`, the bridge's opaque type for the C++ class, whose per-method shims
///   take `&T`. They run the C++ in `callCpp()` (`jsg-cpp.h`) and need the isolate lock, so they
///   take the `Lock`.
/// - As an argument, it accepts an instance of the class or of a subclass, as a C++ `jsg::Ref<T>`
///   parameter does, and runs no JavaScript to check it.
/// - To JavaScript, it is the object's existing wrapper, or a new one for its dynamic type.
/// - A resource field holding one is traced like a C++ `jsg::Ref` field. As for a `jsg::Rc`, a
///   `Ref` moved out of a traced field must be cloned before use; see `src/rust/jsg/README.md`
///   §Safety rules, rule 2.
///
/// It is neither `Send` nor `Sync`: cloning it changes a refcount that the isolate lock guards.
/// It may be dropped on any thread, as a `jsg::Ref` may.
pub struct Ref<T: CppResource> {
    inner: KjOwn<CppRef>,
    _type: PhantomData<*const T>,
}

impl<T: CppResource> Ref<T> {
    /// Takes the reference a per-type shim returns, such as a getter of the C++ resource that
    /// returns a `jsg::Ref<T>`.
    ///
    /// # Safety
    /// `inner` was made for an object of the C++ class `T` stands for, or of a subclass, through a
    /// `jsg::Ref<Class>`.
    pub unsafe fn from_cpp(inner: KjOwn<CppRef>) -> Self {
        Self {
            inner,
            _type: PhantomData,
        }
    }

    /// The `CppRef`, to pass to a shim that takes `const CppRef&`.
    pub fn as_cpp(&self) -> &CppRef {
        &self.inner
    }
}

impl<T: CppResource> Deref for Ref<T> {
    type Target = T;

    fn deref(&self) -> &T {
        // SAFETY: the object is a live instance of the class `T` stands for (`from_cpp` and
        // `CppResource`'s contracts), which `self.inner` keeps alive. `T` is a cxx opaque type:
        // zero-sized and interior-mutable, so the reference claims no bytes of the object.
        unsafe { &*(bridge::cpp_ref_object(&self.inner) as *const T) }
    }
}

impl<T: CppResource> Clone for Ref<T> {
    fn clone(&self) -> Self {
        Self {
            inner: bridge::cpp_ref_clone(&self.inner),
            _type: PhantomData,
        }
    }
}

impl<T: CppResource> jsg::Traced for Ref<T> {
    fn trace(&self, visitor: &mut v8::GcVisitor) {
        // SAFETY: the visitor's pointer is valid for the duration of the visit.
        unsafe { bridge::cpp_ref_visit(visitor.as_ffi(), &self.inner) };
    }
}

impl<T: CppResource> jsg::Type for Ref<T> {
    fn class_name() -> &'static str {
        T::class_name()
    }

    fn is_exact(value: v8::Local<'_, v8::Value>) -> bool {
        value.is_object()
    }
}

impl<'s, T: CppResource> jsg::ToJS<'s> for Ref<T> {
    fn to_js(self, lock: &mut jsg::Lock<'s>) -> Result<v8::Local<'s, v8::Value>, jsg::Error> {
        if lock.is_execution_terminating() {
            return Err(jsg::Error::pending());
        }
        // SAFETY: `lock` stands for the isolate being locked with a context entered and the
        // HandleScope `'s` open; `self.inner` was made for `T`'s class.
        let wrapper = unsafe { T::wrap(lock.isolate().as_ffi(), &self.inner) };
        // SAFETY: the wrapper is a handle in the scope `'s`.
        unsafe { v8::Local::from_ffi_maybe(wrapper) }.ok_or_else(jsg::Error::pending)
    }
}

impl<'s, T: CppResource> jsg::FromJS<'s> for Ref<T> {
    type ResultType = Self;

    fn type_name() -> String {
        T::class_name().to_owned()
    }

    fn from_js(
        lock: &mut jsg::Lock<'s>,
        value: v8::Local<'s, v8::Value>,
    ) -> Result<Self, jsg::Error> {
        let type_name = value.type_of(lock);
        Self::try_from_js(lock, value)?.ok_or_else(|| {
            jsg::Error::new_type_error(format!("expected {}, got {type_name}", T::class_name()))
        })
    }

    fn try_from_js(
        lock: &mut jsg::Lock<'s>,
        value: v8::Local<'s, v8::Value>,
    ) -> Result<Option<Self>, jsg::Error> {
        // SAFETY: as in `to_js`, and `value` is a handle in the scope `'s`.
        let inner = unsafe { T::try_unwrap(lock.isolate().as_ffi(), value.into_ffi()) }?;
        // SAFETY: `try_unwrap` makes references for `T`'s class (`CppResource`'s contract).
        Ok(Option::from(inner).map(|inner| unsafe { Self::from_cpp(inner) }))
    }
}
