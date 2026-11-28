//! Module for [`KjRc`] and [`KjArc`], since they're nearly identical types, and [`KjWeakRc`]

#![allow(
    unsafe_code,
    reason = "`KjRc`, `KjArc` and `KjWeakRc` mirror `kj::Rc<T>`, `kj::Arc<T>` and `kj::WeakRc<T>`"
)]

use static_assertions::assert_eq_align;
use static_assertions::assert_eq_size;

assert_eq_size!(repr::KjRc<()>, [*const (); 2]);
assert_eq_align!(repr::KjRc<()>, *const ());
assert_eq_size!(repr::KjArc<()>, [*const (); 2]);
assert_eq_align!(repr::KjArc<()>, *const ());
assert_eq_size!(repr::KjWeakRc<()>, [*const (); 2]);
assert_eq_align!(repr::KjWeakRc<()>, *const ());

pub mod repr {
    use std::ffi::c_void;
    use std::marker::PhantomData;
    use std::ops::Deref;
    use std::pin::Pin;
    use std::ptr::NonNull;

    use crate::own::NonNullExceptMaybe;

    /// Bindings to the kj type `kj::Rc`. Represents an owned and reference counted type,
    /// like Rust's [`std::rc::Rc`].
    ///
    /// The pointee does not need to inherit `kj::Refcounted`.
    /// Only ordinary object targets are supported; KJ pointer types are stored inline instead.
    ///
    /// `ptr` is never null in a bare `KjRc<T>`; it is only null as the niche `None`
    /// representation inside a `KjMaybe<KjRc<T>>`, mirroring `kj::MaybeTraits<kj::Rc<T>>`.
    #[repr(C)]
    pub struct KjRc<T> {
        pub(crate) ptr: NonNullExceptMaybe<T>,
        refcounted: *mut c_void,
    }

    /// Bindings to the kj type `kj::Arc`. Represents an owned and atomically reference
    /// counted type, like Rust's [`std::sync::Arc`].
    ///
    /// Only ordinary object targets are supported; KJ pointer types are stored inline instead.
    ///
    /// `ptr` is never null in a bare `KjArc<T>`; it is only null as the niche `None`
    /// representation inside a `KjMaybe<KjArc<T>>`, mirroring `kj::MaybeTraits<kj::Arc<T>>`.
    #[repr(C)]
    pub struct KjArc<T> {
        pub(crate) ptr: NonNullExceptMaybe<T>,
        refcounted: *const c_void,
    }

    /// Bindings to the kj type `kj::WeakRc`, the weak companion of [`KjRc`], like Rust's
    /// [`std::rc::Weak`]. Made by [`KjRc::downgrade`].
    ///
    /// It does not keep its referent alive: it expires once the last `kj::Rc` reference to the
    /// referent is dropped, before the referent is destroyed. It keeps only the referent's weak
    /// cell, which the `kj::Refcounted` owner allocates on its first downgrade. Like `kj::Rc`, its
    /// count is not atomic, so it is neither `Send` nor `Sync`.
    ///
    /// A bare `KjWeakRc<T>` always has a cell. Rust code never reads its fields, which only the
    /// C++ helpers interpret, so their order is not mirrored. Like `KjRc<T>`, it is invariant in
    /// `T`: [`upgrade`](Self::upgrade) returns a `KjRc<T>`, whose [`get_mut`](KjRc::get_mut)
    /// can write a `T`.
    #[repr(C)]
    pub struct KjWeakRc<T> {
        fields: [*mut c_void; 2],
        _referent: PhantomData<*mut T>,
    }

    // Safety: the KJ bridge representation and ownership invariants satisfy this operation.
    unsafe impl<T> Send for KjArc<T> where T: Send {}
    // Safety: the KJ bridge representation and ownership invariants satisfy this operation.
    unsafe impl<T> Sync for KjArc<T> where T: Sync {}

    impl<T> KjRc<T> {
        #[must_use]
        pub fn is_shared(&self) -> bool {
            unsafe extern "C" {
                #[link_name = "cxxbridge$kjrs$rc$is_shared"]
                fn __is_shared(this: *const c_void) -> bool;
            }

            // Safety: the KJ bridge representation and ownership invariants satisfy this operation.
            unsafe { __is_shared(std::ptr::from_ref(self).cast::<c_void>()) }
        }

        #[must_use]
        pub fn get(&self) -> *const T {
            self.ptr.as_ptr()
        }

        // The return value here represents exclusive access to the pointee.
        // This allows for exclusive mutation of the inner value.
        pub fn get_mut(&mut self) -> Option<Pin<&mut T>> {
            if self.is_shared() {
                None
            } else {
                // Safety: moving the `KjRc` does not move the pointee, `is_shared()` proves that
                // this is the only active `KjRc` reference to it. A bare `KjRc` is never null.
                // Safety: the KJ bridge representation and ownership invariants satisfy this operation.
                unsafe { Some(Pin::new_unchecked(self.ptr.as_mut())) }
            }
        }

        /// Makes a weak reference to the referent, as `kj::Rc<T>::downgrade()` does.
        #[must_use]
        pub fn downgrade(&self) -> KjWeakRc<T> {
            unsafe extern "C" {
                #[link_name = "cxxbridge$kjrs$rc$downgrade"]
                fn __downgrade(this: *const c_void, out: *mut c_void);
            }

            let mut ret = std::mem::MaybeUninit::<KjWeakRc<T>>::uninit();
            // Safety: `self` is a valid `kj::Rc`, and `ret` is storage for a `kj::WeakRc`, which
            // the helper initializes.
            unsafe {
                __downgrade(
                    std::ptr::from_ref(self).cast::<c_void>(),
                    ret.as_mut_ptr().cast::<c_void>(),
                );
                ret.assume_init()
            }
        }
    }

    impl<T> KjWeakRc<T> {
        /// The referent, if it has not expired, as `kj::WeakRc<T>::tryGet()` returns it. The
        /// pointer is valid only while the referent is kept alive some other way.
        #[must_use]
        pub fn try_get(&self) -> Option<NonNull<T>> {
            unsafe extern "C" {
                #[link_name = "cxxbridge$kjrs$weak_rc$try_get"]
                fn __try_get(this: *const c_void) -> *mut c_void;
            }

            // Safety: `self` is a valid `kj::WeakRc`.
            NonNull::new(unsafe { __try_get(std::ptr::from_ref(self).cast::<c_void>()) })
                .map(NonNull::cast)
        }

        /// Whether the referent has not expired.
        #[must_use]
        pub fn is_alive(&self) -> bool {
            self.try_get().is_some()
        }

        /// A strong reference to the referent, if it has not expired, as
        /// `kj::WeakRc<T>::upgrade()` makes.
        ///
        /// # Safety
        ///
        /// No reference that [`KjRc::get_mut`] returned for the referent is live. `get_mut` counts
        /// only strong references, as `kj::Refcounted::isShared()` does, so it does not refuse a
        /// referent that has weak references. Once this returns a `KjRc`, the referent is shared,
        /// and `get_mut` refuses it.
        #[must_use]
        pub unsafe fn upgrade(&self) -> Option<KjRc<T>> {
            unsafe extern "C" {
                #[link_name = "cxxbridge$kjrs$weak_rc$upgrade"]
                fn __upgrade(this: *const c_void, out: *mut c_void) -> bool;
            }

            let mut ret = std::mem::MaybeUninit::<KjRc<T>>::uninit();
            // Safety: `self` is a valid `kj::WeakRc`, and `ret` is storage for a `kj::Rc`, which
            // the helper initializes when it returns true.
            unsafe {
                __upgrade(
                    std::ptr::from_ref(self).cast::<c_void>(),
                    ret.as_mut_ptr().cast::<c_void>(),
                )
                .then(|| ret.assume_init())
            }
        }
    }

    impl<T> Clone for KjWeakRc<T> {
        fn clone(&self) -> Self {
            unsafe extern "C" {
                #[link_name = "cxxbridge$kjrs$weak_rc$clone"]
                fn __clone(this: *const c_void, out: *mut c_void);
            }

            let mut ret = std::mem::MaybeUninit::<Self>::uninit();
            // Safety: `self` is a valid `kj::WeakRc`, and `ret` is storage for another, which the
            // helper initializes.
            unsafe {
                __clone(
                    std::ptr::from_ref(self).cast::<c_void>(),
                    ret.as_mut_ptr().cast::<c_void>(),
                );
                ret.assume_init()
            }
        }
    }

    impl<T> Drop for KjWeakRc<T> {
        fn drop(&mut self) {
            unsafe extern "C" {
                #[link_name = "cxxbridge$kjrs$weak_rc$drop"]
                fn __drop(this: *mut c_void);
            }

            // Safety: `self` is a valid `kj::WeakRc`, not used after this.
            unsafe {
                __drop(std::ptr::from_mut(self).cast::<c_void>());
            }
        }
    }

    impl<T> Drop for KjRc<T> {
        fn drop(&mut self) {
            unsafe extern "C" {
                #[link_name = "cxxbridge$kjrs$rc$drop"]
                fn __drop(this: *mut c_void);
            }

            // Safety: the KJ bridge representation and ownership invariants satisfy this operation.
            unsafe {
                __drop(std::ptr::from_mut(self).cast::<c_void>());
            }
        }
    }

    impl<T> KjArc<T> {
        #[must_use]
        pub fn is_shared(&self) -> bool {
            unsafe extern "C" {
                #[link_name = "cxxbridge$kjrs$arc$is_shared"]
                fn __is_shared(this: *const c_void) -> bool;
            }

            // Safety: the KJ bridge representation and ownership invariants satisfy this operation.
            unsafe { __is_shared(std::ptr::from_ref(self).cast::<c_void>()) }
        }

        #[must_use]
        pub fn get(&self) -> *const T {
            self.ptr.as_ptr()
        }

        // The return value here represents exclusive access to the pointee.
        // This allows for exclusive mutation of the inner value.
        pub fn get_mut(&mut self) -> Option<Pin<&mut T>> {
            if self.is_shared() {
                None
            } else {
                // Safety: moving the `KjArc` does not move the pointee, `is_shared()` proves that
                // this is the only active `KjArc` reference to it. A bare `KjArc` is never null.
                // Safety: the KJ bridge representation and ownership invariants satisfy this operation.
                unsafe { Some(Pin::new_unchecked(self.ptr.as_mut())) }
            }
        }
    }

    impl<T> Deref for KjRc<T> {
        type Target = T;

        fn deref(&self) -> &Self::Target {
            // Safety: `KjRc` does not allow null pointees to cross into Rust.
            // Safety: the KJ bridge representation and ownership invariants satisfy this operation.
            unsafe { self.ptr.as_ref() }
        }
    }

    impl<T> Deref for KjArc<T> {
        type Target = T;

        fn deref(&self) -> &Self::Target {
            // Safety: `KjArc` does not allow null pointees to cross into Rust.
            // Safety: the KJ bridge representation and ownership invariants satisfy this operation.
            unsafe { self.ptr.as_ref() }
        }
    }

    /// Using clone to create another count, like how Rust does it.
    impl<T> Clone for KjRc<T> {
        fn clone(&self) -> Self {
            unsafe extern "C" {
                #[link_name = "cxxbridge$kjrs$rc$clone"]
                fn __clone(this: *const c_void, out: *mut c_void);
            }

            let mut ret = std::mem::MaybeUninit::<Self>::uninit();
            // Safety: the KJ bridge representation and ownership invariants satisfy this operation.
            unsafe {
                __clone(
                    std::ptr::from_ref(self).cast::<c_void>(),
                    ret.as_mut_ptr().cast::<c_void>(),
                );
                ret.assume_init()
            }
        }
    }

    impl<T> Clone for KjArc<T> {
        fn clone(&self) -> Self {
            unsafe extern "C" {
                #[link_name = "cxxbridge$kjrs$arc$clone"]
                fn __clone(this: *const c_void, out: *mut c_void);
            }

            let mut ret = std::mem::MaybeUninit::<Self>::uninit();
            // Safety: the KJ bridge representation and ownership invariants satisfy this operation.
            unsafe {
                __clone(
                    std::ptr::from_ref(self).cast::<c_void>(),
                    ret.as_mut_ptr().cast::<c_void>(),
                );
                ret.assume_init()
            }
        }
    }

    impl<T> Drop for KjArc<T> {
        fn drop(&mut self) {
            unsafe extern "C" {
                #[link_name = "cxxbridge$kjrs$arc$drop"]
                fn __drop(this: *mut c_void);
            }

            // Safety: the KJ bridge representation and ownership invariants satisfy this operation.
            unsafe {
                __drop(std::ptr::from_mut(self).cast::<c_void>());
            }
        }
    }
}
