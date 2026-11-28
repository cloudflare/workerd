use static_assertions::assert_not_impl_any;

use super::CallbacksFuture;
use super::PromiseFuture;

// The raw `*mut c_void` node makes this `!Send`/`!Sync` on its own; guard against a future
// hand-written impl silently introducing cross-thread transfer of a KJ promise node.
assert_not_impl_any!(CallbacksFuture<u32>: Send, Sync);

// After its first poll, `PromiseFuture`'s embedded awaiter memory is self-referential and
// event-loop-linked (see `PromiseAwaiter::_pinned`); it must stay `!Unpin` so safe code
// cannot move it between polls (`&mut`-based awaits require `Unpin`).
assert_not_impl_any!(PromiseFuture<CallbacksFuture<u32>>: Unpin);

// The composed future the comment in promise.rs relies on stays `!Send` as well.
assert_not_impl_any!(PromiseFuture<CallbacksFuture<u32>>: Send);
