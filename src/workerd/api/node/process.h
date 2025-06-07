// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

#include <workerd/api/node/i18n.h>
#include <workerd/api/node/node-version.h>
#include <workerd/jsg/jsg.h>
#include <workerd/util/mimetype.h>

namespace workerd::api::node {

class ProcessModule final: public jsg::Object {
 public:
  ProcessModule() = default;
  ProcessModule(jsg::Lock&, const jsg::Url&) {}

  static constexpr kj::StringPtr processPlatform = "linux"_kj;

  jsg::JsValue getBuiltinModule(jsg::Lock& js, kj::String specifier);

  // This is used in the implementation of process.exit(...). Contrary
  // to what the name suggests, it does not actually exit the process.
  // Instead, it will cause the IoContext, if any, and will stop javascript
  // from further executing in that request. If there is no active IoContext,
  // then it becomes a non-op.
  void processExitImpl(jsg::Lock& js, int code);

  // IMPORTANT: This function will always return "linux" on production.
  // This is only added for Node.js compatibility and running OS specific tests
  kj::StringPtr getProcessPlatform() const {
    return processPlatform;
  }

  jsg::JsObject getEnvObject(jsg::Lock& js);

  // Version getters
  jsg::JsObject getVersions(jsg::Lock& js) const {
    auto versions = js.obj();
    // Node.js version - represents the most current Node.js version supported
    // by the platform and will change as Node.js release updates ship
    versions.set(js, "node"_kj, js.str(workerd::api::node::nodeVersion));

    // Get ICU version dynamically from the ICU library
    versions.set(js, "icu"_kj, js.str(i18n::getIcuVersion()));

    return versions;
  }

  JSG_RESOURCE_TYPE(ProcessModule) {
    JSG_METHOD(getEnvObject);
    JSG_METHOD(getBuiltinModule);
    JSG_METHOD(processExitImpl);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(processPlatform, getProcessPlatform);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(versions, getVersions);
  }
};

#define EW_NODE_PROCESS_ISOLATE_TYPES api::node::ProcessModule

}  // namespace workerd::api::node
