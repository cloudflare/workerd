// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// Common functionality to manage cf headers and properties.

#include <workerd/jsg/jsg.h>

namespace workerd::api {

class CfProperty {
  // A holder for Cf header property value.
  // The string header is parsed on demand and the parsed value cached.

public:
  KJ_DISALLOW_COPY(CfProperty);

  explicit CfProperty() {}
  CfProperty(decltype(nullptr)) {}
  CfProperty(CfProperty&&) = default;
  CfProperty& operator=(CfProperty&&) = default;

  explicit CfProperty(kj::Maybe<kj::StringPtr> unparsed) {
    KJ_IF_MAYBE(str, unparsed) {
      value = kj::str(*str);
    }
  }

  explicit CfProperty(kj::Maybe<jsg::V8Ref<v8::Object>>&& parsed) {
    KJ_IF_MAYBE(v, parsed) {
      value = kj::mv(*v);
    }
  }

  jsg::Optional<v8::Local<v8::Object>> get(jsg::Lock& js);
  // Get parsed value

  jsg::Optional<jsg::V8Ref<v8::Object>> getRef(jsg::Lock& js);
  // Get parsed value as a global ref

  kj::Maybe<kj::String> serialize(jsg::Lock& js);
  // Serialize to string

  CfProperty deepClone(jsg::Lock& js);
  // Clone by deep cloning parsed v8 object (if any).

  void visitForGc(jsg::GcVisitor& visitor);

private:
  kj::Maybe<kj::OneOf<kj::String, jsg::V8Ref<v8::Object>>> value;
};


} // namespace workerd::api
