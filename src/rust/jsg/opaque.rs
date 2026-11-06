// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Rust values carried through JavaScript in opaque wrappers, as C++ JSG's `wrapOpaque()` and
//! `unwrapOpaque()` carry C++ values: through a promise, for example, which V8 settles only
//! with a JavaScript value.
//!
//! The wrapper is an object with no properties and a null prototype. It owns the value through a
//! Rust Wrappable, so the garbage collector traces the value while the wrapper is reachable and
//! drops it once it isn't, unless [`unwrap_opaque`] has taken it. The wrapper must not reach
//! JavaScript code, which could keep it, and the value, alive.

#![allow(
    unsafe_code,
    reason = "reads the value of a Wrappable that the FFI hands back"
)]

use std::any::TypeId;
use std::rc::Rc;

use crate::GarbageCollected;
use crate::Lock;
use crate::RefCell;
use crate::Traced;
use crate::v8::Local;
use crate::v8::Value;
use crate::v8::WrappableRc;
use crate::v8::ffi;

/// The value of an opaque wrapper, until [`unwrap_opaque`] takes it.
struct Opaque<T>(RefCell<Option<T>>);

impl<T: Traced> Traced for Opaque<T> {
    fn trace(&self, visitor: &mut crate::GcVisitor) {
        self.0.trace(visitor);
    }
}

impl<T: Traced> GarbageCollected for Opaque<T> {
    fn memory_name(&self) -> &'static std::ffi::CStr {
        // C++ JSG's name for the object behind an opaque wrapper.
        c"OpaqueWrappable"
    }
}

/// Returns an opaque wrapper that owns `value`.
///
/// # Errors
///
/// While an exception or a termination is pending, creates nothing, drops `value` and returns an
/// error for which [`Error::is_pending`](crate::Error::is_pending) is true.
pub fn wrap_opaque<'s, T: Traced + 'static>(
    lock: &Lock<'s>,
    value: T,
) -> crate::Result<Local<'s, Value>> {
    // As in `jsg::Rc::new`, the Wrappable owns a leaked `Rc` of the value, released when the
    // Wrappable is destroyed.
    let raw: *const Opaque<T> = Rc::into_raw(Rc::new(Opaque(RefCell::new(Some(value)))));
    let fat: *mut dyn GarbageCollected = raw as *const dyn GarbageCollected as *mut _;
    let mut wrappable: WrappableRc =
        ffi::TraitObjectPtr::from_raw(fat, TypeId::of::<Opaque<T>>()).into();
    let wrapper = wrappable.attach_opaque_wrapper(lock);
    // A new Wrappable starts with one strong reference, which `jsg::Rc` would hold. Here only the
    // wrapper owns it, so the reference is dropped and the garbage collector decides.
    wrappable.remove_strong_ref();
    wrapper
}

/// Takes the value of `wrapper`, an opaque wrapper that [`wrap_opaque`] returned for a `T`.
/// Returns `None` if `wrapper` is anything else, or if its value was taken already.
///
/// The value is detached from the wrapper, as a C++ `jsg::Ref` moved out of its holder is (see
/// [`GcVisitor::is_detaching`](crate::GcVisitor::is_detaching)): the garbage collector may have
/// left the references it holds weak, for the wrapper to trace, and the wrapper no longer does.
pub fn unwrap_opaque<'s, T: Traced + 'static>(
    lock: &mut Lock<'s>,
    wrapper: Local<'s, Value>,
) -> Option<T> {
    let wrappable = WrappableRc::of_opaque(lock, wrapper)?;
    let opaque = wrappable.resolve::<Opaque<T>>()?;
    // SAFETY: `wrappable` keeps the Wrappable, and with it the `Opaque`, alive while the reference
    // is used. Only shared references to the `Opaque` are ever created.
    let opaque = unsafe { opaque.as_ref() };
    // The borrow detaches the value before it is taken.
    opaque.0.try_borrow_mut(lock).ok()?.take()
}
