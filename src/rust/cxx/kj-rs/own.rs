//! The `workerd-cxx` module containing the [`Own<T>`] type, which is bindings to the `kj::Own<T>` C++ type

use std::fmt;
use std::marker::PhantomData;

use static_assertions::assert_eq_align;
use static_assertions::assert_eq_size;

use crate::repr::KjOwn;

assert_eq_size!(KjOwn<i64>, [*const (); 2]);
assert_eq_align!(KjOwn<i64>, *const ());

/// A type that Rust may hold in a [`KjOwn`].
///
/// Dropping a `kj::Own<T>` is a typed operation: `kj::Own<T>::~Own()` hands the disposer the
/// address of the complete object (`dynamic_cast<void*>` for a polymorphic `T`), which differs
/// from the `T*` when `T` is a base at a nonzero offset of the object it points into. Only C++
/// code that knows `T` can do this, so every `T` gets its own C++ drop function,
/// `cxxbridge$kjrs$own$<mangled T>$drop(kj::Own<T>*)`, and this trait's [`__drop`] calls it.
///
/// A bridge generates the implementation, with the C++ function beside it, for every `T` whose
/// `KjOwn<T>` appears in it and that it declares as an `extern "C++"` type. A `KjOwn<T>` of a type
/// the bridge only aliases (`type T = other::ffi::T;`) borrows the implementation from the bridge
/// that declares `T`; when that bridge never names `KjOwn<T>`, it must ask for one explicitly with
/// `impl KjOwn<T> {}`. The primitive types a bridge accepts in a `KjOwn` are implemented here, over
/// one untyped drop.
///
/// # Safety
/// Implementations come from bridge expansion. `__drop` must run `kj::Own<T>::~Own()` on the
/// object `own` points to, exactly once.
///
/// [`__drop`]: OwnTarget::__drop
pub unsafe trait OwnTarget: Sized {
    /// Runs `kj::Own<Self>::~Own()` on `*own`.
    ///
    /// # Safety
    /// `own` must point to a live, initialized `kj::Own<Self>` that nothing uses afterwards.
    #[doc(hidden)]
    unsafe fn __drop(own: *mut KjOwn<Self>);
}

/// Implements [`OwnTarget`] for the primitive types a bridge accepts in a `KjOwn`. They share one
/// untyped drop (`own.c++`): a primitive is never polymorphic, so `kj::Own<T>::~Own()` hands the
/// disposer the pointer unchanged, the same as `kj::Own<void>::~Own()`.
macro_rules! impl_own_target_for_primitive {
    ($($ty:ident),* $(,)?) => {$(
        // Safety: `$ty` is not polymorphic, so the untyped drop runs `kj::Own<$ty>::~Own()`.
        unsafe impl OwnTarget for $ty {
            unsafe fn __drop(own: *mut KjOwn<Self>) {
                // Safety: forwarded from the caller's contract.
                unsafe { drop_primitive(own.cast()) }
            }
        }
    )*};
}

unsafe extern "C" {
    /// `kj::Own<void>::~Own()` on `*own`.
    #[link_name = "cxxbridge$kjrs$own$primitive$drop"]
    fn drop_primitive(own: *mut std::ffi::c_void);
}

impl_own_target_for_primitive!(
    bool, u8, u16, u32, u64, usize, i8, i16, i32, i64, isize, f32, f64
);

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
        // Safety:
        //     This value will only be null when in a [`Maybe<T>`], which does niche value optimization
        //     for a null pointer, so the inner [`Own<T>`] can never be accessed if it is null
        // Safety: the KJ bridge representation and ownership invariants satisfy this operation.
        unsafe { &*self.0 }
    }

    pub unsafe fn as_mut(&mut self) -> &mut T {
        // Safety:
        //     This value will only be null when in a [`Maybe<T>`], which does niche value optimization
        //     for a null pointer, so the inner [`Own<T>`] can never be accessed if it is null
        // Safety: the KJ bridge representation and ownership invariants satisfy this operation.
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
    use super::OwnTarget;

    /// A [`KjOwn<T>`] represents the `kj::Own<T>`. It is a smart pointer to an opaque C++ type.
    /// Dropping it runs `kj::Own<T>::~Own()` through [`OwnTarget::__drop`].
    /// Safety:
    /// - Passing a null `kj::Own` to rust is considered unsafe from the C++ side,
    ///   and it is required that this invariant is upheld in C++ code.
    /// - Currently, it is runtime asserted in the bridge macro that no null Own can be passed
    ///   to Rust
    #[repr(C)]
    pub struct KjOwn<T: OwnTarget> {
        pub(crate) disposer: *const c_void,
        pub(crate) ptr: NonNullExceptMaybe<T>,
    }

    /// Public-facing Own api
    impl<T: OwnTarget> KjOwn<T> {
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

    impl<T: OwnTarget> AsRef<T> for KjOwn<T> {
        /// Returns a reference to the object owned by this [`Own`] if any,
        /// otherwise None.
        fn as_ref(&self) -> &T {
            // Safety: Passing a null kj::Own to Rust from C++ is not supported.
            // Safety: the KJ bridge representation and ownership invariants satisfy this operation.
            unsafe { self.ptr.as_ref() }
        }
    }

    // Safety: the KJ bridge representation and ownership invariants satisfy this operation.
    unsafe impl<T: OwnTarget + Send> Send for KjOwn<T> {}

    // Safety: the KJ bridge representation and ownership invariants satisfy this operation.
    unsafe impl<T: OwnTarget + Sync> Sync for KjOwn<T> {}

    impl<T: OwnTarget> Deref for KjOwn<T> {
        type Target = T;

        fn deref(&self) -> &Self::Target {
            self.as_ref()
        }
    }

    impl<T: OwnTarget + Unpin> DerefMut for KjOwn<T> {
        fn deref_mut(&mut self) -> &mut Self::Target {
            Pin::into_inner(self.as_mut())
        }
    }

    // Own<T> is safe to implement Unpin because moving the Own doesn't move the pointee, and
    // the drop implementation doesn't depend on the Own's location
    impl<T: OwnTarget> Unpin for KjOwn<T> {}

    impl<T: OwnTarget> Debug for KjOwn<T> {
        fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
            write!(f, "Own(ptr: {:p}, disposer: {:p})", self.ptr, self.disposer)
        }
    }

    impl<T: OwnTarget + Display> Display for KjOwn<T> {
        fn fmt(&self, formatter: &mut fmt::Formatter) -> fmt::Result {
            Display::fmt(self.as_ref(), formatter)
        }
    }

    impl<T: OwnTarget + PartialEq> PartialEq for KjOwn<T> {
        fn eq(&self, other: &Self) -> bool {
            self.as_ref() == other.as_ref()
        }
    }

    impl<T: OwnTarget + Eq> Eq for KjOwn<T> {}

    impl<T: OwnTarget + PartialOrd> PartialOrd for KjOwn<T> {
        fn partial_cmp(&self, other: &Self) -> Option<std::cmp::Ordering> {
            PartialOrd::partial_cmp(&self.as_ref(), &other.as_ref())
        }
    }

    impl<T: OwnTarget + Ord> Ord for KjOwn<T> {
        fn cmp(&self, other: &Self) -> std::cmp::Ordering {
            Ord::cmp(&self.as_ref(), &other.as_ref())
        }
    }

    impl<T: OwnTarget + Hash> Hash for KjOwn<T> {
        fn hash<H: Hasher>(&self, state: &mut H) {
            self.as_ref().hash(state);
        }
    }

    impl<T: OwnTarget> Drop for KjOwn<T> {
        fn drop(&mut self) {
            // Safety: `self` is a live `kj::Own<T>` that is being destroyed; it is not used again.
            unsafe { T::__drop(self) }
        }
    }
}
