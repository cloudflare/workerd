// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

#include <workerd/jsg/jsg.h>

#include <kj/string.h>

namespace workerd::api {

class Immediate;

namespace node {
class TimersUtil final: public jsg::Object {
 public:
  TimersUtil() = default;
  TimersUtil(jsg::Lock&, const jsg::Url&) {}

  jsg::Ref<Immediate> setImmediate(jsg::Lock& js,
      jsg::Function<void(jsg::Arguments<jsg::Value>)> function,
      jsg::Arguments<jsg::Value> args);
  void clearImmediate(jsg::Lock& js, kj::Maybe<jsg::Ref<Immediate>> immediate);

  JSG_RESOURCE_TYPE(TimersUtil) {
    JSG_METHOD(setImmediate);
    JSG_METHOD(clearImmediate);
  }
};

#define EW_NODE_TIMERS_ISOLATE_TYPES api::node::TimersUtil
}  // namespace node

}  // namespace workerd::api
