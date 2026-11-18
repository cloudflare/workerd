// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The backing stores in a `jsg::ser::Serialized` are read in the isolate that wrote them. A
// transferred buffer's store may still be held by JavaScript there, as when a getter that
// serialization runs transfers the buffer again, so memory that isn't shared must not be
// reached from another thread.
//
// This compiles only if the type is not Send: were it Send, both impls would apply and the call
// would be ambiguous.
trait AmbiguousIfSend<A> {
    fn some_item() {}
}
impl<T: ?Sized> AmbiguousIfSend<()> for T {}
impl<T: ?Sized + Send> AmbiguousIfSend<u8> for T {}

pub fn t() {
    let _ = <jsg::ser::Serialized as AmbiguousIfSend<_>>::some_item;
}
