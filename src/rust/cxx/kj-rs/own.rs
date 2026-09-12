//! The `workerd-cxx` module containing the [`Own<T>`] type, which is bindings to the `kj::Own<T>` C++ type
//!
//! FFI island (see crate-root `#![deny(unsafe_code)]`): `KjOwn` mirrors `kj::Own` — raw-pointer
//! deref and `extern "C"` disposer/refcount calls. A genuine unsafe seam.
#![allow(unsafe_code)]

use std::fmt;
use std::marker::PhantomData;

use static_assertions::assert_eq_align;
use static_assertions::assert_eq_size;

assert_eq_size!(repr::KjOwn<()>, [*const (); 2]);
assert_eq_align!(repr::KjOwn<()>, *const ());

/// When we want to use an `Own`, we want the guarantee of being not null only
/// in direct `Own<T>`, not Maybe<Own<T>>, and using a [`NonNull`] in `Own`
/// but allowing Nulls for Niche Value Optimization is undefined behavior.
#[repr(transparent)]
pub struct NonNullExceptMaybe<T: ?Sized>(pub(crate) *mut T, PhantomData<T>);

impl<T> NonNullExceptMaybe<T> {
    pub fn as_ptr(&self) -> *const T {
        self.0.cast()
    }

    pub unsafe fn as_ref(&self) -> &T {
        // SAFETY: `self.0` is null only when this `NonNullExceptMaybe` lives inside a
        // `Maybe<T>` (which niche-optimizes the null pointer and never dereferences the inner
        // `Own`), so here — reached only through the non-null `Own<T>` API — it is a valid,
        // live pointer. The caller's `unsafe` obligation is that `self` outlives the borrow.
        unsafe { &*self.0 }
    }

    pub unsafe fn as_mut(&mut self) -> &mut T {
        // SAFETY: as in `as_ref`, `self.0` is non-null and live when reached through the
        // `Own<T>` API; `&mut self` gives exclusive access, so the mutable reborrow is unique.
        unsafe { &mut *self.0 }
    }
}

impl<T> fmt::Pointer for NonNullExceptMaybe<T> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        fmt::Pointer::fmt(&self.0, f)
    }
}

pub mod repr {
    use std::ffi::c_void;
    use std::fmt::Debug;
    use std::fmt::Display;
    use std::fmt::{self};
    use std::hash::Hash;
    use std::hash::Hasher;
    use std::ops::Deref;
    use std::ops::DerefMut;
    use std::pin::Pin;

    use super::NonNullExceptMaybe;

    /// A [`KjOwn<T>`] represents the `kj::Own<T>`. It is a smart pointer to an opaque C++ type.
    /// Safety:
    /// - Passing a null `kj::Own` to rust is considered unsafe from the C++ side,
    ///   and it is required that this invariant is upheld in C++ code.
    /// - Currently, it is runtime asserted in the bridge macro that no null Own can be passed
    ///   to Rust
    #[repr(C)]
    pub struct KjOwn<T: ?Sized> {
        pub(crate) disposer: *const c_void,
        pub(crate) ptr: NonNullExceptMaybe<T>,
    }

    /// Public-facing Own api
    impl<T> KjOwn<T> {
        /// Returns a mutable pinned reference to the object owned by this [`Own`]
        /// if any, otherwise None.
        pub fn as_mut(&mut self) -> Pin<&mut T> {
            // Safety: Passing a null kj::Own to Rust from C++ is not supported.
            // Safety: the KJ bridge representation and ownership invariants satisfy this operation.
            unsafe {
                let mut_reference = self.ptr.as_mut();
                Pin::new_unchecked(mut_reference)
            }
        }

        /// Returns a mutable pinned reference to the object owned by this
        /// [`Own`].
        ///
        /// ```compile_fail
        /// let mut own = ffi::cxx_kj_own();
        /// let pin1 = own.pin_mut();
        /// let pin2 = own.pin_mut();
        /// pin1.set_data(12); // Causes a compile fail, because we invalidated the first borrow
        /// ```
        ///
        /// ```compile_fail
        ///
        /// let mut own = ffi::cxx_kj_own();
        /// let pin = own.pin_mut();
        /// let moved  = own;
        /// own.set_data(143); // Compile fail, because we tried using a moved object
        /// ```
        pub fn pin_mut(&mut self) -> Pin<&mut T> {
            self.as_mut()
        }

        /// Returns a raw const pointer to the object owned by this [`Own`]
        #[must_use]
        pub fn as_ptr(&self) -> *const T {
            self.ptr.as_ptr()
        }
    }

    impl<T> AsRef<T> for KjOwn<T> {
        /// Returns a reference to the object owned by this [`Own`] if any,
        /// otherwise None.
        fn as_ref(&self) -> &T {
            // Safety: Passing a null kj::Own to Rust from C++ is not supported.
            // Safety: the KJ bridge representation and ownership invariants satisfy this operation.
            unsafe { self.ptr.as_ref() }
        }
    }

    // NO `Send`/`Sync` impls, deliberately.
    //
    // A `KjOwn<T>` carries a type-erased `kj::Disposer*` alongside the object pointer, and
    // dropping the `KjOwn` runs that disposer on whichever thread the drop happens on. A
    // bound on `T` alone (e.g. `T: Send`) says nothing about the disposer: `kj::Own`s minted
    // from `kj::Rc::toOwn()`/`kj::refcounted` (non-atomic refcount decrement), arena-backed
    // objects, or any other custom disposer are NOT safe to destroy from another thread, and
    // nothing at the bridge boundary guarantees disposer thread-safety.
    //
    // All current consumers keep `KjOwn`s on the KJ event-loop thread that created them, so
    // no impls are needed. If a genuine cross-thread use case appears, it must come with an
    // explicit opt-in mechanism that asserts the *disposer* is thread-safe (not just `T`);
    // do not re-add blanket impls here.

    impl<T> Deref for KjOwn<T> {
        type Target = T;

        fn deref(&self) -> &Self::Target {
            self.as_ref()
        }
    }

    impl<T> DerefMut for KjOwn<T>
    where
        T: Unpin,
    {
        fn deref_mut(&mut self) -> &mut Self::Target {
            Pin::into_inner(self.as_mut())
        }
    }

    // Own<T> is safe to implement Unpin because moving the Own doesn't move the pointee, and
    // the drop implentation doesn't depend on the Own's location, because it's handed by virtual dispatch
    impl<T> Unpin for KjOwn<T> {}

    impl<T> Debug for KjOwn<T> {
        fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
            write!(f, "Own(ptr: {:p}, disposer: {:p})", self.ptr, self.disposer)
        }
    }

    impl<T> Display for KjOwn<T>
    where
        T: Display,
    {
        fn fmt(&self, formatter: &mut fmt::Formatter) -> fmt::Result {
            Display::fmt(self.as_ref(), formatter)
        }
    }

    impl<T> PartialEq for KjOwn<T>
    where
        T: PartialEq,
    {
        fn eq(&self, other: &Self) -> bool {
            self.as_ref() == other.as_ref()
        }
    }

    impl<T> Eq for KjOwn<T> where T: Eq {}

    impl<T> PartialOrd for KjOwn<T>
    where
        T: PartialOrd,
    {
        fn partial_cmp(&self, other: &Self) -> Option<std::cmp::Ordering> {
            PartialOrd::partial_cmp(&self.as_ref(), &other.as_ref())
        }
    }

    impl<T> Ord for KjOwn<T>
    where
        T: Ord,
    {
        fn cmp(&self, other: &Self) -> std::cmp::Ordering {
            Ord::cmp(&self.as_ref(), &other.as_ref())
        }
    }

    impl<T> Hash for KjOwn<T>
    where
        T: Hash,
    {
        fn hash<H: Hasher>(&self, state: &mut H) {
            self.as_ref().hash(state);
        }
    }

    impl<T: ?Sized> Drop for KjOwn<T> {
        fn drop(&mut self) {
            unsafe extern "C" {
                #[link_name = "cxxbridge$kjrs$own$drop"]
                fn __drop(this: *mut c_void);
            }

            let this = std::ptr::from_mut::<Self>(self).cast::<c_void>();
            // SAFETY: `this` points to this live `KjOwn` being dropped exactly once; the C++
            // `own$drop` shim invokes the type-erased `kj::Disposer` stored alongside `ptr`.
            unsafe {
                __drop(this);
            }
        }
    }
}
