use std::mem::MaybeUninit;
use std::pin::Pin;

// Based on StackInit from the `pinned-init` crate:
// https://github.com/Rust-for-Linux/pinned-init/blob/67c0a0c35bf23b8584f8e7792f9098de5fe0c8b0/src/__internal.rs#L142
//
// TODO(now): Define this in terms of some trait?

/// # Invariants
///
/// If `self.is_init` is true, then `self.value` is initialized.
pub struct LazyPinInit<T> {
    value: MaybeUninit<T>,
    is_init: bool,
}

impl<T> Drop for LazyPinInit<T> {
    #[inline]
    fn drop(&mut self) {
        if self.is_init {
            // SAFETY: As we are being dropped, we only call this once. And since `self.is_init` is
            // true, `self.value` is initialized.
            unsafe { self.value.assume_init_drop() };
        }
    }
}

impl<T> LazyPinInit<T> {
    /// Creates a new `LazyPinInit<T>` that is uninitialized.
    #[inline]
    pub fn uninit() -> Self {
        Self {
            value: MaybeUninit::uninit(),
            is_init: false,
        }
    }

    /// Initializes the contents and returns the result.
    #[inline]
    pub fn get_or_init(self: Pin<&mut Self>, init: impl FnOnce(*mut T)) -> Pin<&mut T> {
        // SAFETY: We never move out of `this`.
        let this = unsafe { Pin::into_inner_unchecked(self) };
        // The value is currently initialized, so it needs to be dropped before we can reuse
        // the memory (this is a safety guarantee of `Pin`).
        if !this.is_init {
            // SAFETY: The memory slot is valid and this type ensures that it will stay pinned.
            init(this.value.as_mut_ptr());
            // INVARIANT: `this.value` is initialized above.
            this.is_init = true;
        }
        // SAFETY: The slot is now pinned, since we will never give access to `&mut T`.
        unsafe { Pin::new_unchecked(this.value.assume_init_mut()) }
    }
}

// TODO(now): Test, exception-handling
