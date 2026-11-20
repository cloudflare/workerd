use static_assertions::assert_impl_all;
use static_assertions::assert_not_impl_any;

use super::CallbacksFuture;
use super::PromiseFuture;
use super::SendKjFuture;

// The future of a C++ `async fn` returning `i32`. `into_send()` makes it `Send`, but not `Sync`: a
// `&SendKjFuture` would let another thread use the promise.
type BridgedFuture = PromiseFuture<CallbacksFuture<i32>>;

assert_impl_all!(SendKjFuture<BridgedFuture>: Send);
assert_not_impl_any!(SendKjFuture<BridgedFuture>: Sync);
