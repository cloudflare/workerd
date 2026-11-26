// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// The channel classes of `IoChannelFactory` by names in the namespace that the io_channels bridge
// (ffi.rs) declares them in. Rust holds them as `kj::Rc`s.

#include <workerd/io/io-channels.h>

namespace workerd::io_channels {

using SubrequestChannel = IoChannelFactory::SubrequestChannel;
using ActorClassChannel = IoChannelFactory::ActorClassChannel;
using RpcChannel = IoChannelFactory::RpcChannel;

}  // namespace workerd::io_channels
