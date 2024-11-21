// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

#include <workerd/jsg/jsg.h>

namespace workerd::api::node {

class ModuleUtil final: public jsg::Object {
 public:
  ModuleUtil() = default;
  ModuleUtil(jsg::Lock&, const jsg::Url&) {}

  jsg::JsValue createRequire(jsg::Lock& js, kj::String specifier);

  // Returns true if the specifier is a known node.js built-in module specifier.
  // Ignores whether or not the module actually exists (use process.getBuiltinModule()
  // for that purpose).
  bool isBuiltin(kj::String specifier);

  JSG_RESOURCE_TYPE(ModuleUtil) {
    JSG_METHOD(createRequire);
    JSG_METHOD(isBuiltin);
  }
};

#define EW_NODE_MODULE_ISOLATE_TYPES api::node::ModuleUtil

}  // namespace workerd::api::node
