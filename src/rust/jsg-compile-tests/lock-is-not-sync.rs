// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A lock cannot be shared with another thread.
//
// This compiles only if the type is not Sync: were it Sync, both impls would apply and the call
// would be ambiguous. A plain `T: Sync` bound would fail with one error per field that is not
// Sync, and how many of those rustc reports differs between versions.
trait AmbiguousIfSync<A> {
    fn some_item() {}
}
impl<T: ?Sized> AmbiguousIfSync<()> for T {}
impl<T: ?Sized + Sync> AmbiguousIfSync<u8> for T {}

pub fn t() {
    let _ = <jsg::Lock<'static> as AmbiguousIfSync<_>>::some_item;
}
