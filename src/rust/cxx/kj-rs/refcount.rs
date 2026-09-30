//! Module for both [`KjRc`] and [`KjArc`], since they're nearly identical types

use static_assertions::assert_eq_align;
use static_assertions::assert_eq_size;

assert_eq_size!(repr::KjRc<()>, [*const (); 2]);
assert_eq_align!(repr::KjRc<()>, *const ());
assert_eq_size!(repr::KjArc<()>, [*const (); 2]);
assert_eq_align!(repr::KjArc<()>, *const ());

pub mod repr {
    use std::ffi::c_void;
    use std::ops::Deref;
    use std::pin::Pin;

    use crate::own::NonNullExceptMaybe;

    /// Bindings to the kj type `kj::Rc`. Represents an owned and reference counted type,
    /// like Rust's [`std::rc::Rc`]. The pointee does not need to inherit `kj::Refcounted`.
    ///
    /// `ptr` is never null in a bare `KjRc<T>`; it is only null as the niche `None`
    /// representation inside a `KjMaybe<KjRc<T>>`, mirroring `kj::MaybeTraits<kj::Rc<T>>`.
    #[repr(C)]
    pub struct KjRc<T> {
        refcounted: *mut c_void,
        pub(crate) ptr: NonNullExceptMaybe<T>,
    }

    /// Bindings to the kj type `kj::Arc`. Represents and owned and atomically reference
    /// counted type, like Rust's [`std::sync::Arc`].
    ///
    /// `ptr` is never null in a bare `KjArc<T>`; it is only null as the niche `None`
    /// representation inside a `KjMaybe<KjArc<T>>`, mirroring `kj::MaybeTraits<kj::Arc<T>>`.
    #[repr(C)]
    pub struct KjArc<T> {
        refcounted: *const c_void,
        pub(crate) ptr: NonNullExceptMaybe<T>,
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
