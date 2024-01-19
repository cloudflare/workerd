// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "pyodide.h"

#include <capnp/common.h>
#include <capnp/compat/json.h>

namespace workerd::api::pyodide {

namespace _ {
kj::StringPtr lookupModule(kj::StringPtr name) {
  for (auto m : PYODIDE_BUNDLE->getModules()) {
    if (m.getName() == name) {
      return m.getSrc().asChars().begin();
    }
  }
  KJ_UNREACHABLE;
}
}

kj::StringPtr getPyodideBootstrap() {
  return _::lookupModule("pyodide-internal:pyodide-bootstrap");
}

kj::StringPtr getPyodideLock() {
  return _::lookupModule("pyodide-internal:pyodide-lock");
}

kj::StringPtr getPyodidePatch(kj::StringPtr name) {
  return _::lookupModule(kj::str("pyodide:internal/patches/", name));
}

kj::String generatePyodideMetadata(server::config::Worker::Reader conf) {
  kj::String result = kj::str("export function getMetadata() { return ");
  capnp::JsonCodec jsonCodec;
  jsonCodec.setPrettyPrint(false);
  result = kj::str(result, jsonCodec.encode(conf));
  result = kj::str(result, "; }");
  return result;
}

kj::String generatePyodidePatches() {
  capnp::JsonCodec jsonCodec;
  jsonCodec.setPrettyPrint(false);
  jsonCodec.handleByAnnotation<capnp::json::Value>();
  capnp::MallocMessageBuilder arena;
  auto jsonRoot = arena.getRoot<capnp::JsonValue>();
  auto obj = jsonRoot.initObject(1);
  obj[0].setName("aiohttp_fetch_patch.py");
  obj[0].initValue().setString(getPyodidePatch("aiohttp_fetch_patch.py"));

  kj::String result = kj::str("export function getPatches() { return ");
  result = kj::str(result, jsonCodec.encode(jsonRoot.asReader()));
  result = kj::str(result, "; }");
  return result;
}

bool hasPythonModules(capnp::List<server::config::Worker::Module>::Reader modules) {
  for (auto module: modules) {
    if (module.isPythonModule()) {
      return true;
    }
  }
  return false;
}

capnp::Data::Reader getPyodideEmbeddedPackages() {
  // TODO(later): strip the version from this.
  auto moduleName = "pyodide:generated/pyodide_packages_unzipped_0.2.tar";
  for (auto m : PYODIDE_BUNDLE->getModules()) {
    if (m.getName() == moduleName) {
      return m.getSrc();
    }
  }
  KJ_UNREACHABLE;
}

}  // namespace workerd
