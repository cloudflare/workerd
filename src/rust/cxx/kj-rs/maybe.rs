//! FFI island: the `KjMaybe` representation of `kj::Maybe`.
//!
//! (See crate-root `#![deny(unsafe_code)]`.) Carries the `unsafe trait` niche contracts
//! (`HasNiche`/`MaybeItem`) and `assume_init` on the discriminated union. A genuine unsafe seam.
#![allow(unsafe_code)]

use std::mem::MaybeUninit;
use std::pin::Pin;

use repr::KjMaybe;

/// # Safety
/// This trait should only be implemented in `workerd-cxx` on types
/// which contain a specialization of `kj::Maybe` that needs to be represented in
/// Rust.
///
/// This trait represents types which have a "niche", a value which represents
/// an invalid instance of the type or can reasonably be interpreted as the absence
/// of that type. This trait is implmented for 2 types, references and `Owns`.
///
/// References have a niche where they are null. It's invalid and ensured by the
/// compiler that this is impossible, so we can optimize an optional type by
/// eliminating a flag that checks whether the item is set or not, and instead
/// checking if it is null.
///
/// `Own`s have a niche where the pointer to the owned data is null. This is
/// a valid instance of `Own`, but was decided by the `kj` authors to represent
/// `kj::none`. In Rust, it is guaranteed that an `Own` is nonnull, requiring
/// `KjMaybe<Own<T>>` to represent a null `Own`.
///
/// Pointers are not optimized in this way, as `null` is a valid and meaningful
/// instance of a pointer.
///
/// An invalid implementation of this trait for any of the 3 types it is for
/// could result in undefined behavior when passed between languages.
unsafe trait HasNiche: Sized {
    fn is_niche(value: *const Self) -> bool;
}

// SAFETY: in Rust, references are not allowed to be null, so a null `MaybeUninit<&T>` is a
// niche (see the `HasNiche` trait contract above).
unsafe impl<T> HasNiche for &T {
    fn is_niche(value: *const &T) -> bool {
        // SAFETY: `value` points to a valid `&T`; we read it as a `*const *const T` (never as a
        // reference, which the compiler assumes non-null) to test the pointer for null.
        unsafe {
            // We must cast it as pointing to a pointer, as opposed to a reference,
            // because the rust compiler assumes a reference is never null, and
            // therefore will optimize any null check on that reference.
            (*(value.cast::<*const T>())).is_null()
        }
    }
}

// SAFETY: as for `&T` — a null `&mut T` is the niche (see the `HasNiche` trait contract).
unsafe impl<T> HasNiche for &mut T {
    fn is_niche(value: *const &mut T) -> bool {
        // SAFETY: `value` points to a valid `&mut T`; read as `*const *mut T` to null-check.
        unsafe {
            // We must cast it as pointing to a pointer, as opposed to a reference,
            // because the rust compiler assumes a reference is never null, and
            // therefore will optimize any null check on that reference.
            (*(value.cast::<*mut T>())).is_null()
        }
    }
}

// SAFETY: as for `&mut T` — a null pointee is the niche (see the `HasNiche` trait contract).
unsafe impl<T> HasNiche for Pin<&mut T> {
    fn is_niche(value: *const Pin<&mut T>) -> bool {
        // SAFETY: `value` points to a valid `Pin<&mut T>` (layout-identical to `&mut T`); read
        // as `*const *mut T` to null-check.
        unsafe {
            // We must cast it as pointing to a pointer, as opposed to a reference,
            // because the rust compiler assumes a reference is never null, and
            // therefore will optimize any null check on that reference.
            (*(value.cast::<*mut T>())).is_null()
        }
    }
}

// In `kj`, `kj::Own<T>` are considered `none` in a `Maybe` if the data pointer is null
// SAFETY: a `KjOwn` with a null data pointer is `kj::none` (see the `HasNiche` trait contract).
unsafe impl<T> HasNiche for crate::repr::KjOwn<T> {
    fn is_niche(value: *const Self) -> bool {
        // SAFETY: `value` points to a valid `KjOwn<T>`; querying its data pointer is sound.
        unsafe { (*value).as_ptr().is_null() }
    }
}

/// Trait that is used as the bounds for what can be in a `kj_rs::KjMaybe`.
///
/// # Safety
/// This trait should only be implemented from macro expansion and should
/// never be manually implemented. An unsound implementation of this trait
/// could result in undefined behavior when passed between languages.
///
/// This trait contains all behavior we need to implement `KjMaybe<T: MaybeItem>`
/// for every `T` we use, and additionally determines the type layout of
/// the `KjMaybe<T>`. The only information we can know about `T` comes from
/// this trait, so it must be capable of handling all behavior we want in
/// `kj_rs::KjMaybe`.
///
/// Every function without a default depends on `MaybeItem::Discriminant`
/// and whether or not `T` implements [`HasNiche`]. Functions with defaults
/// use those functions to implement shared behavior, and simplfy the actual
/// `KjMaybe<T>` implementation.
pub unsafe trait MaybeItem: Sized {
    type Discriminant: Copy;
    const NONE: KjMaybe<Self>;
    fn some(value: Self) -> KjMaybe<Self>;
    fn is_some(value: &KjMaybe<Self>) -> bool;
    fn is_none(value: &KjMaybe<Self>) -> bool;
    fn from_option(value: Option<Self>) -> KjMaybe<Self> {
        match value {
            None => <Self as MaybeItem>::NONE,
            Some(val) => <Self as MaybeItem>::some(val),
        }
    }
    fn drop_in_place(value: &mut KjMaybe<Self>) {
        if <Self as MaybeItem>::is_some(value) {
            // SAFETY: `is_some` just confirmed the `some` union member is initialized, so
            // dropping it in place is sound. `KjMaybe`'s `Drop` calls this exactly once.
            unsafe {
                value.some.assume_init_drop();
            }
        }
    }
}

/// Macro to implement [`MaybeItem`] for `T` which implment [`HasNiche`].
/// Avoids running into generic specialization problems.
macro_rules! impl_maybe_item_for_has_niche {
    ($ty:ty) => {
        // SAFETY: `$ty` is only ever a `HasNiche` type (enforced at the macro's use sites), so
        // it carries a `()` discriminant and detects `none` via its null niche — matching kj's
        // niche-value-optimized `Maybe` layout, as the `MaybeItem` trait contract requires.
        unsafe impl<T> MaybeItem for $ty {
            type Discriminant = ();

            fn is_some(value: &KjMaybe<Self>) -> bool {
                !<$ty as HasNiche>::is_niche(value.some.as_ptr())
            }

            fn is_none(value: &KjMaybe<Self>) -> bool {
                <$ty as HasNiche>::is_niche(value.some.as_ptr())
            }

            const NONE: KjMaybe<Self> = {
                KjMaybe {
                    is_set: (),
                    some: MaybeUninit::zeroed(),
                }
            };

            fn some(value: Self) -> KjMaybe<Self> {
                KjMaybe {
                    is_set: (),
                    some: MaybeUninit::new(value)
                }
            }
        }
    };
    ($ty:ty, $($tail:ty),+) => {
        impl_maybe_item_for_has_niche!($ty);
        impl_maybe_item_for_has_niche!($($tail),*);
    };
}

/// Macro to implement [`MaybeItem`] for primitives
/// Avoids running into generic specialization problems.
macro_rules! impl_maybe_item_for_primitive {
    ($ty:ty) => {
        // SAFETY: primitives have no niche, so this mirrors kj's non-niche
        // `kj::_::NullableValue` layout with an explicit `bool` discriminant (`is_set`)
        // followed by the value, exactly as the `MaybeItem` trait contract requires.
        unsafe impl MaybeItem for $ty {
            type Discriminant = bool;

            fn is_some(value: &KjMaybe<Self>) -> bool {
                value.is_set
            }

            fn is_none(value: &KjMaybe<Self>) -> bool {
                !value.is_set
            }

            const NONE: KjMaybe<Self> = {
                KjMaybe {
                    is_set: false,
                    some: MaybeUninit::uninit(),
                }
            };

            fn some(value: Self) -> KjMaybe<Self> {
                KjMaybe {
                    is_set: true,
                    some: MaybeUninit::new(value)
                }
            }
        }
    };
    ($ty:ty, $($tail:ty),+) => {
        impl_maybe_item_for_primitive!($ty);
        impl_maybe_item_for_primitive!($($tail),*);
    };
}

impl_maybe_item_for_has_niche!(crate::KjOwn<T>, &T, &mut T, Pin<&mut T>);
impl_maybe_item_for_primitive!(
    u8, u16, u32, u64, u128, usize, i8, i16, i32, i64, i128, isize, f32, f64, bool, &str, String
);

// SAFETY: `&[T]` is a fat pointer with no usable niche here, so it uses the explicit
// `bool`-discriminant (non-niche) `MaybeItem` representation, matching kj's layout.
unsafe impl<T> MaybeItem for &[T] {
    type Discriminant = bool;

    fn is_some(value: &KjMaybe<Self>) -> bool {
        value.is_set
    }

    fn is_none(value: &KjMaybe<Self>) -> bool {
        !value.is_set
    }

    const NONE: KjMaybe<Self> = {
        KjMaybe {
            is_set: false,
            some: MaybeUninit::uninit(),
        }
    };

    fn some(value: Self) -> KjMaybe<Self> {
        KjMaybe {
            is_set: true,
            some: MaybeUninit::new(value),
        }
    }
}

// Unlike `kj::Own<T>`, the `kj::Rc<T>` and `kj::Arc<T>` types do NOT define
// `kj::MaybeTraits` niche members (`initNone`/`isNone`). This means
// `kj::Maybe<kj::Rc<T>>` does not use niche-value optimization: it uses the
// non-niche `kj::_::NullableValue<T>` representation, which stores a separate
// `bool` flag followed by the value (`bool isSet; union { T value; };`).
//
// We therefore mirror that layout with a `bool` discriminant here, exactly like
// the primitive types above, rather than implementing [`HasNiche`].
//
// SAFETY: `kj::Rc<T>` defines no `Maybe` niche members, so `kj::Maybe<kj::Rc<T>>` uses the
// non-niche `bool`-discriminant `NullableValue` layout mirrored here (see comment above).
unsafe impl<T> MaybeItem for crate::KjRc<T> {
    type Discriminant = bool;

    fn is_some(value: &KjMaybe<Self>) -> bool {
        value.is_set
    }

    fn is_none(value: &KjMaybe<Self>) -> bool {
        !value.is_set
    }

    const NONE: KjMaybe<Self> = {
        KjMaybe {
            is_set: false,
            some: MaybeUninit::uninit(),
        }
    };

    fn some(value: Self) -> KjMaybe<Self> {
        KjMaybe {
            is_set: true,
            some: MaybeUninit::new(value),
        }
    }
}

// SAFETY: like `kj::Rc<T>`, `kj::Arc<T>` defines no `Maybe` niche members, so
// `kj::Maybe<kj::Arc<T>>` uses the non-niche `bool`-discriminant `NullableValue` layout
// mirrored here.
unsafe impl<T> MaybeItem for crate::KjArc<T> {
    type Discriminant = bool;

    fn is_some(value: &KjMaybe<Self>) -> bool {
        value.is_set
    }

    fn is_none(value: &KjMaybe<Self>) -> bool {
        !value.is_set
    }

    const NONE: KjMaybe<Self> = {
        KjMaybe {
            is_set: false,
            some: MaybeUninit::uninit(),
        }
    };

    fn some(value: Self) -> KjMaybe<Self> {
        KjMaybe {
            is_set: true,
            some: MaybeUninit::new(value),
        }
    }
}

pub(crate) mod repr {
    use std::fmt::Debug;
    use std::mem::MaybeUninit;

    use static_assertions::assert_eq_size;

    use super::MaybeItem;

    /// A [`KjMaybe`] represents bindings to the `kj::Maybe` class.
    /// It is an optional type, but represented using a struct, for alignment with kj.
    ///
    /// # Layout
    /// In kj, `Maybe` has 3 specializations, one without niche value optimization, and
    /// two with it. In order to maintain an identical layout in Rust, we include an associated type
    /// in the [`MaybeItem`] trait, which determines the discriminant of the `KjMaybe<T: MaybeItem>`.
    ///
    /// ## Niche Value Optimization
    /// This discriminant is used in tandem with the [`crate::maybe::HasNiche`] to implement
    /// [`MaybeItem`] properly for values which have a niche, which use a discriminant of [`()`],
    /// the unit type. All other types use [`bool`].
    #[repr(C)]
    pub struct KjMaybe<T: MaybeItem> {
        pub(super) is_set: T::Discriminant,
        pub(super) some: MaybeUninit<T>,
    }

    assert_eq_size!(KjMaybe<isize>, [usize; 2]);
    assert_eq_size!(KjMaybe<&isize>, usize);
    assert_eq_size!(KjMaybe<crate::KjOwn<isize>>, [usize; 2]);
    // `kj::Rc<T>` has no niche, so `kj::Maybe<kj::Rc<T>>` carries a separate flag:
    // a `bool` discriminant followed by the two-pointer `kj::Rc` value.
    assert_eq_size!(KjMaybe<crate::KjRc<isize>>, [usize; 3]);
    // `String` (three-pointer struct, no niche) uses the same layout: `bool` discriminant plus
    // padding, followed by three pointers.
    assert_eq_size!(KjMaybe<String>, [usize; 4]);

    impl<T: MaybeItem> KjMaybe<T> {
        /// # Safety
        /// This function shouldn't be used except by macro generation.
        pub unsafe fn is_set(&self) -> T::Discriminant {
            self.is_set
        }

        /// # Safety
        /// This function shouldn't be used except by macro generation.
        #[inline]
        pub const unsafe fn from_parts_unchecked(
            is_set: T::Discriminant,
            some: MaybeUninit<T>,
        ) -> Self {
            Self { is_set, some }
        }

        pub fn is_some(&self) -> bool {
            T::is_some(self)
        }

        pub fn is_none(&self) -> bool {
            T::is_none(self)
        }

        // # CONSTRUCTORS
        // These emulate Rust's enum api, which offers constructors for each variant.
        // This mean matching cases, syntax, and behavior.
        // The only place this may be an issue is pattern matching, which will not work,
        // but should produce an error.
        //
        // The following fails to compile:
        // ```{rust,compile_fail}
        // match maybe {
        //     Maybe::Some(_) => ...,
        //     Maybe::None => ...,
        // }
        // ```

        /// The [`Maybe::Some`] function serves the same purpose as an enum constructor.
        ///
        /// Constructing a `KjMaybe<T>::Some(val)` should only be possible with a valid
        /// instance of `T` from Rust.
        #[expect(non_snake_case, reason = "KjMaybe emulates Option's variant API")]
        pub fn Some(value: T) -> Self {
            T::some(value)
        }

        /// [`Maybe::None`] functions as a constructor for the none variant. It uses
        /// a `const` instead of a function to match syntax with normal Rust enums.
        ///
        /// Constructing a `KjMaybe<T>::None` variant should always be possible from Rust.
        #[expect(
            non_upper_case_globals,
            reason = "KjMaybe emulates Option's variant API"
        )]
        pub const None: Self = T::NONE;
    }

    impl<T: MaybeItem> From<KjMaybe<T>> for Option<T> {
        fn from(value: KjMaybe<T>) -> Self {
            if value.is_some() {
                // We can't move out of value so we copy it and forget it in
                // order to perform a "manual" move out of value
                // SAFETY: `is_some` confirmed `some` is initialized; `assume_init_read` copies
                // it out, and the immediately following `mem::forget(value)` prevents the
                // source from being dropped, so ownership moves out exactly once.
                let ret = unsafe { Some(value.some.assume_init_read()) };
                std::mem::forget(value);
                ret
            } else {
                None
            }
        }
    }

    impl<T: MaybeItem> From<Option<T>> for KjMaybe<T> {
        fn from(value: Option<T>) -> Self {
            <T as MaybeItem>::from_option(value)
        }
    }

    impl<T: MaybeItem + Debug> Debug for KjMaybe<T> {
        fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
            if self.is_none() {
                write!(f, "Maybe::None")
            } else {
                // SAFETY: the `is_none()` branch above is false here, so `some` is
                // initialized and may be borrowed for formatting.
                let value = unsafe { self.some.assume_init_ref() };
                write!(f, "Maybe::Some({value:?})")
            }
        }
    }

    impl<T: MaybeItem> Default for KjMaybe<T> {
        fn default() -> Self {
            T::NONE
        }
    }

    impl<T: MaybeItem> Drop for KjMaybe<T> {
        fn drop(&mut self) {
            T::drop_in_place(self);
        }
    }
}
