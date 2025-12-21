// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/io/worker-interface.capnp.h>

#include <capnp/compat/byte-stream.h>
#include <kj/async-io.h>

namespace workerd {

// Implements JsValue.ExternalPusher from worker-interface.capnp.
//
// ExternalPusher allows a remote peer to "push" certain kinds of objects into our address space
// so that they can then be embedded in `JsValue` as `External` values.
class ExternalPusherImpl: public rpc::JsValue::ExternalPusher::Server, public kj::Refcounted {
 public:
  ExternalPusherImpl(capnp::ByteStreamFactory& byteStreamFactory) {}

  // TODO(now): Implement methods.
};

}  // namespace workerd
