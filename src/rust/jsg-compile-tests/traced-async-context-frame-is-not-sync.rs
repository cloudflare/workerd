// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A `TracedAsyncContextFrame` holds a `TracedGlobal`, which can move to another thread but not be
// shared with one.
//
// This compiles only if the type is not Sync; see `traced-global-is-not-sync.rs`.
trait AmbiguousIfSync<A> {
    fn some_item() {}
}
impl<T: ?Sized> AmbiguousIfSync<()> for T {}
impl<T: ?Sized + Sync> AmbiguousIfSync<u8> for T {}

pub fn t() {
    let _ = <jsg::TracedAsyncContextFrame as AmbiguousIfSync<_>>::some_item;
}
