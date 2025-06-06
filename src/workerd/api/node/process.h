// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

#include "external/generated_versions/versions.h"

#include <workerd/api/node/i18n.h>
#include <workerd/jsg/jsg.h>
#include <workerd/util/mimetype.h>

namespace workerd::api::node {

class ProcessModule final: public jsg::Object {
 public:
  ProcessModule() = default;
  ProcessModule(jsg::Lock&, const jsg::Url&) {}

#ifdef _WIN32
  static constexpr kj::StringPtr processPlatform = "win32"_kj;
#elif defined(__linux__)
  static constexpr kj::StringPtr processPlatform = "linux"_kj;
#elif defined(__APPLE__)
  static constexpr kj::StringPtr processPlatform = "darwin"_kj;
#else
  static constexpr kj::StringPtr processPlatform = "unsupported-platform"_kj;
#endif

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
    versions.set(js, "ada"_kj, js.str(workerd::api::node::adaVersion));
    versions.set(js, "node"_kj, js.str(workerd::api::node::nodeVersion));
    versions.set(js, "brotli"_kj, js.str(workerd::api::node::brotliVersion));
    versions.set(js, "simdutf"_kj, js.str(workerd::api::node::simdutfVersion));
    versions.set(js, "sqlite"_kj, js.str(workerd::api::node::sqliteVersion));
    versions.set(js, "nbytes"_kj, js.str(workerd::api::node::nbytesVersion));
    versions.set(js, "ncrypto"_kj, js.str(workerd::api::node::ncryptoVersion));
    versions.set(js, "v8"_kj, js.str(workerd::api::node::v8Version));

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
