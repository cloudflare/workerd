// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A channel is an I/O object of the thread it was made on, so a reference to one is neither
// `Send` nor `Sync`.
//
// This asserts the missing impls rather than failing to compile where a channel crosses threads:
// rustc reports one error for each field of `KjRc` that is not `Send`, and a snippet's
// annotations must not depend on how many errors rustc reports.
use io_channels::ActorClassChannel;
use io_channels::RpcChannel;
use io_channels::SubrequestChannel;
use static_assertions::assert_not_impl_any;

assert_not_impl_any!(SubrequestChannel: Send, Sync);
assert_not_impl_any!(ActorClassChannel: Send, Sync);
assert_not_impl_any!(RpcChannel: Send, Sync);
