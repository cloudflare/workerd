#pragma once

#include <capnp/message.h>
#include <kj/refcount.h>

namespace workerd {

// Builds a new message, initializes its root with `init(Root::Builder)`, and returns a shared
// read-only view of that root. The view keeps the message alive.
template <typename Root, typename InitFunc>
kj::Arc<typename Root::Reader> buildArcMessage(InitFunc&& init) {
  auto message = kj::uniqueArc<capnp::MallocMessageBuilder>();
  auto builder = message->initRoot<Root>();
  kj::fwd<InitFunc>(init)(builder);
  auto reader = builder.asReader();
  return kj::mv(message).toArc().project(
      [reader](const capnp::MallocMessageBuilder&) { return reader; });
}

// Deep-copies `source` into a compact message and returns a shared read-only view of the copy.
template <typename Root>
kj::Arc<typename Root::Reader> cloneArcMessage(typename Root::Reader source) {
  return kj::Arc<typename Root::Reader>(capnp::clone(source));
}

// Returns a shared view of `root`, which must point into a message kept alive by `owner`.
template <typename Reader, typename Owner>
kj::Arc<Reader> attachReader(Reader root, kj::Own<Owner> owner) {
  return kj::Arc<Owner>(kj::mv(owner)).project([root](const Owner&) { return root; });
}

}  // namespace workerd
