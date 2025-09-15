#pragma once
#include <workerd/jsg/jsg.h>

namespace workerd::api {

// A special purpose module used for internal debugging and testing only.
// **This module must not be available in production deployments**
class InternalDebugModule: public jsg::Object {
 public:
  InternalDebugModule() = default;

  bool autogateIsEnabled(jsg::Lock&, kj::String name);

  JSG_RESOURCE_TYPE(InternalDebugModule) {
    JSG_METHOD(autogateIsEnabled);
  }
};
}  // namespace workerd::api
#define EW_DEBUG_ISOLATE_TYPES workerd::api::InternalDebugModule
