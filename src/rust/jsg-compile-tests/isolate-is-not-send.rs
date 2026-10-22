// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// An isolate reference stands for holding the isolate lock on this thread; it cannot move to
// another one.
//
// This compiles only if the type is not Send: were it Send, both impls would apply and the call
// would be ambiguous.
trait AmbiguousIfSend<A> {
    fn some_item() {}
}
impl<T: ?Sized> AmbiguousIfSend<()> for T {}
impl<T: ?Sized + Send> AmbiguousIfSend<u8> for T {}

pub fn t() {
    let _ = <jsg::Isolate as AmbiguousIfSend<_>>::some_item;
}
