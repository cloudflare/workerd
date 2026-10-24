// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "ffi.h"

#include <workerd/rust/jsg/ffi-inl.h>

#include <capnp/serialize.h>
#include <kj/io.h>

namespace workerd::rust::io_context {

IoContext& io_context_current(jsg::Isolate* isolate, jsg::MaybeLocal& exception) {
  return jsg::catching(isolate, exception, []() -> IoContext& { return IoContext::current(); });
}

IoContext* io_context_try_current() {
  KJ_IF_SOME(context, IoContext::tryCurrent()) {
    return &context;
  }
  return nullptr;
}

int64_t io_context_now(IoContext& context) {
  return (context.now() - kj::UNIX_EPOCH) / kj::NANOSECONDS;
}

void io_context_write_logfwdr(
    IoContext& context, uint32_t channel, ::rust::Slice<const uint8_t> message) {
  // Rust does not align the bytes to words, so the reader copies them.
  kj::ArrayInputStream input(kj::arrayPtr(message.data(), message.size()));
  capnp::InputStreamMessageReader reader(input);
  context.writeLogfwdr(channel, [&](capnp::AnyPointer::Builder builder) {
    builder.set(reader.getRoot<capnp::AnyPointer>());
  });
}

}  // namespace workerd::rust::io_context
