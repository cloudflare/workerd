// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#include "util.h"
#include <kj/vector.h>

namespace workerd::api::node {

MIMEParams::MIMEParams(kj::Maybe<MimeType&> mimeType) : mimeType(mimeType) {}

// Oddly, Node.js allows creating MIMEParams directly but it's not actually
// functional. But, to match, we'll go ahead and allow it.
jsg::Ref<MIMEParams> MIMEParams::constructor() {
  return jsg::alloc<MIMEParams>(nullptr);
}

void MIMEParams::delete_(kj::String name) {
  KJ_IF_MAYBE(inner, mimeType) {
    inner->eraseParam(name);
  }
}

kj::Maybe<kj::StringPtr> MIMEParams::get(kj::String name) {
  KJ_IF_MAYBE(inner, mimeType) {
    return inner->params().find(name);
  }
  return nullptr;
}

bool MIMEParams::has(kj::String name) {
  KJ_IF_MAYBE(inner, mimeType) {
    return inner->params().find(name) != nullptr;
  }
  return false;
}

void MIMEParams::set(kj::String name, kj::String value) {
  KJ_IF_MAYBE(inner, mimeType) {
    JSG_REQUIRE(inner->addParam(name, value), TypeError,
        "Not a valid MIME parameter");
  }
}

kj::String MIMEParams::toString() {
  KJ_IF_MAYBE(inner, mimeType) {
    return inner->paramsToString();
  }
  return kj::str();
}

jsg::Ref<MIMEParams::EntryIterator> MIMEParams::entries(jsg::Lock&) {
  kj::Vector<kj::Array<kj::String>> vec;
  KJ_IF_MAYBE(inner, mimeType) {
    for (const auto& entry : inner->params()) {
      vec.add(kj::arr(kj::str(entry.key), kj::str(entry.value)));
    }
  }
  return jsg::alloc<EntryIterator>(IteratorState<kj::Array<kj::String>> {
    vec.releaseAsArray()
  });
}

jsg::Ref<MIMEParams::KeyIterator> MIMEParams::keys(jsg::Lock&) {
  kj::Vector<kj::String> vec;
  KJ_IF_MAYBE(inner, mimeType) {
    for (const auto& entry : inner->params()) {
      vec.add(kj::str(entry.key));
    }
  }
  return jsg::alloc<KeyIterator>(IteratorState<kj::String> {
    vec.releaseAsArray()
  });
}

jsg::Ref<MIMEParams::ValueIterator> MIMEParams::values(jsg::Lock&) {
  kj::Vector<kj::String> vec;
  KJ_IF_MAYBE(inner, mimeType) {
    for (const auto& entry : inner->params()) {
      vec.add(kj::str(entry.value));
    }
  }
  return jsg::alloc<ValueIterator>(IteratorState<kj::String> {
    vec.releaseAsArray()
  });
}

MIMEType::MIMEType(MimeType inner)
    : inner(kj::mv(inner)),
      params(jsg::alloc<MIMEParams>(this->inner)) {}

MIMEType::~MIMEType() noexcept(false) {
  // Break the connection with the MIMEParams
  params->mimeType = nullptr;
}

jsg::Ref<MIMEType> MIMEType::constructor(kj::String input) {
  auto parsed = JSG_REQUIRE_NONNULL(MimeType::tryParse(input), TypeError,
      "Not a valid MIME type: ", input);
  return jsg::alloc<MIMEType>(kj::mv(parsed));
}

kj::StringPtr MIMEType::getType() {
  return inner.type();
}

void MIMEType::setType(kj::String type) {
  JSG_REQUIRE(inner.setType(type), TypeError, "Not a valid MIME type");
}

kj::StringPtr MIMEType::getSubtype() {
  return inner.subtype();
}

void MIMEType::setSubtype(kj::String subtype) {
  JSG_REQUIRE(inner.setSubtype(subtype), TypeError, "Not a valid MIME subtype");
}

kj::String MIMEType::getEssence() {
  return inner.essence();
}

jsg::Ref<MIMEParams> MIMEType::getParams() {
  return params.addRef();
}

kj::String MIMEType::toString() {
  return inner.toString();
}

}  // namespace workerd::api::node
