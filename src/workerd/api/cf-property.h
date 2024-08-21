// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// Common functionality to manage cf headers and properties.

#include <workerd/jsg/jsg.h>

namespace workerd::api {

// A holder for Cf header property value.
// The string header is parsed on demand and the parsed value cached.
class CfProperty {

public:
  KJ_DISALLOW_COPY(CfProperty);

  explicit CfProperty() {}
  CfProperty(decltype(nullptr)) {}
  CfProperty(CfProperty&&) = default;
  CfProperty& operator=(CfProperty&&) = default;

  explicit CfProperty(kj::Maybe<kj::StringPtr> unparsed);

  explicit CfProperty(jsg::Lock& js, const jsg::JsObject& object);

  explicit CfProperty(kj::Maybe<jsg::JsRef<jsg::JsObject>>&& parsed);

  // Get parsed value
  jsg::Optional<jsg::JsObject> get(jsg::Lock& js);

  // Get parsed value as a global ref
  jsg::Optional<jsg::JsRef<jsg::JsObject>> getRef(jsg::Lock& js);

  // Serialize to string
  kj::Maybe<kj::String> serialize(jsg::Lock& js);

  // Clone by deep cloning parsed v8 object (if any).
  CfProperty deepClone(jsg::Lock& js);

  void visitForGc(jsg::GcVisitor& visitor);

  JSG_MEMORY_INFO(CfProperty) {
    KJ_IF_SOME(v, value) {
      KJ_SWITCH_ONEOF(v) {
        KJ_CASE_ONEOF(str, kj::String) {
          tracker.trackField("value", str);
        }
        KJ_CASE_ONEOF(obj, jsg::JsRef<jsg::JsObject>) {
          tracker.trackField("value", obj);
        }
      }
    }
  }

private:
  kj::Maybe<kj::OneOf<kj::String, jsg::JsRef<jsg::JsObject>>> value;
};

}  // namespace workerd::api
