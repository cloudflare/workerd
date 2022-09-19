// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "capnp-mock.h"

namespace workerd {

kj::String canonicalizeCapnpText(capnp::StructSchema schema, kj::StringPtr text,
                                 kj::Maybe<kj::StringPtr> capName) {
  capnp::MallocMessageBuilder message;
  auto root = message.getRoot<capnp::DynamicStruct>(schema);
  TEXT_CODEC.decode(text, root);
  KJ_IF_MAYBE(c, capName) {
    // Fill in dummy capability.
    auto field = schema.getFieldByName(*c);
    root.set(field, capnp::Capability::Client(KJ_EXCEPTION(FAILED, "dummy"))
        .castAs<capnp::DynamicCapability>(field.getType().asInterface()));
  }
  return TEXT_CODEC.encode(root.asReader());
}

} // namespace workerd
