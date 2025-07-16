// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

#include <workerd/api/node/i18n.h>
#include <workerd/api/node/node-version.h>
#include <workerd/io/worker-fs.h>
#include <workerd/jsg/jsg.h>
#include <workerd/util/mimetype.h>

namespace workerd::api::node {

class ProcessModule final: public jsg::Object {
 public:
  ProcessModule() = default;
  ProcessModule(jsg::Lock&, const jsg::Url&) {}

  jsg::JsValue getBuiltinModule(jsg::Lock& js, kj::String specifier);

// This process.platform implementation is always gated behind the
// 'enable_deprecated_process_actual_platform' compatibility flag
// which is ONLY used in testing environments, and requires 'experimental'.
#ifdef _WIN32
  static constexpr kj::StringPtr platform = "win32"_kj;
#elif defined(__linux__)
  static constexpr kj::StringPtr platform = "linux"_kj;
#elif defined(__APPLE__)
  static constexpr kj::StringPtr platform = "darwin"_kj;
#else
  static constexpr kj::StringPtr platform = "unsupported-platform"_kj;
#endif

  // This is used in the implementation of process.exit(...). Contrary
  // to what the name suggests, it does not actually exit the process.
  // Instead, it will cause the IoContext, if any, and will stop javascript
  // from further executing in that request. If there is no active IoContext,
  // then it becomes a non-op.
  void exitImpl(jsg::Lock& js, int code);

  // IMPORTANT: This function will always return "linux" on production unless
  // the unsupported_process_actual_platform compat flag is enabled.
  // This is only added for Node.js compatibility and running OS specific tests
  kj::StringPtr getPlatform(jsg::Lock& js) const;

  jsg::JsObject getEnvObject(jsg::Lock& js);

  jsg::JsObject getVersions(jsg::Lock& js) const;

  kj::String getCwd(jsg::Lock& js);

  void setCwd(jsg::Lock& js, kj::String path);

  JSG_RESOURCE_TYPE(ProcessModule) {
    JSG_METHOD(getEnvObject);
    JSG_METHOD(getBuiltinModule);
    JSG_METHOD(exitImpl);
    JSG_METHOD(getCwd);
    JSG_METHOD(setCwd);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(versions, getVersions);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(platform, getPlatform);
  }
};

#define EW_NODE_PROCESS_ISOLATE_TYPES api::node::ProcessModule

}  // namespace workerd::api::node
