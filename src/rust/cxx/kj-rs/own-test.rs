use static_assertions::assert_impl_all;
use static_assertions::assert_not_impl_any;

use super::DisposableOnAnyThread;
use super::OwnTarget;
use crate::repr::KjOwn;

// `u8` is `Send` and `Sync`, but a `kj::Own<uint8_t>` may have any disposer, so only `Sync`
// carries over.
assert_not_impl_any!(KjOwn<u8>: Send);
assert_impl_all!(KjOwn<u8>: Sync);

/// A type whose owner has vouched for its disposers. Never constructed.
struct OnAnyThread;

// SAFETY: no `KjOwn<OnAnyThread>` is ever made, so `__drop` is never called.
unsafe impl OwnTarget for OnAnyThread {
    unsafe fn __drop(_own: *mut KjOwn<Self>) {}
}

// SAFETY: no `kj::Own<OnAnyThread>` exists.
unsafe impl DisposableOnAnyThread for OnAnyThread {}

assert_impl_all!(KjOwn<OnAnyThread>: Send, Sync);
