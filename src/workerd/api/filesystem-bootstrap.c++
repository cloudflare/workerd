// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "filesystem-bootstrap.h"

#include "filesystem.h"

namespace workerd::api {

jsg::JsValue createFileSystemWriteContext(
    jsg::Lock& js, jsg::JsValue fileHandle, KeepExistingData keepExistingData) {
  // Both types are registered in EW_FILESYSTEM_ISOLATE_TYPES, so the handlers are
  // always present. Asserting rather than propagating keeps the caller (a raw
  // bootstrap callback) free of a failure mode it could not act on.
  auto& fileHandler = KJ_ASSERT_NONNULL(js.tryGetTypeHandler<jsg::Ref<FileSystemFileHandle>>(),
      "FileSystemFileHandle is missing from the isolate type list");
  auto& contextHandler =
      KJ_ASSERT_NONNULL(js.tryGetTypeHandler<jsg::Ref<FileSystemWriteContextHandle>>(),
          "FileSystemWriteContextHandle is missing from the isolate type list");
  auto& deHandler = KJ_ASSERT_NONNULL(js.tryGetTypeHandler<jsg::Ref<jsg::DOMException>>(),
      "DOMException is missing from the isolate type list");

  // The receiver reaches here as an ordinary argument because the TypeScript
  // createWritable() is a plain function rather than a JSG method, so nothing has
  // checked it yet. JSG would have rejected a foreign receiver with this message.
  auto file = JSG_REQUIRE_NONNULL(fileHandler.tryUnwrap(js, fileHandle), TypeError,
      "Failed to execute 'createWritable' on 'FileSystemFileHandle': "
      "Illegal invocation");

  auto opened = FileSystemWriteContextHandle::open(
      js, kj::mv(file), keepExistingData == KeepExistingData::YES);
  KJ_IF_SOME(ex, opened.tryGet<jsg::Ref<jsg::DOMException>>()) {
    // The C++ createWritable() reports these by rejecting; the TypeScript one
    // turns a throw into the same rejection, so the observable result matches.
    js.throwException(jsg::JsValue(deHandler.wrap(js, kj::mv(ex))));
  }

  return jsg::JsValue(
      contextHandler.wrap(js, kj::mv(opened.get<jsg::Ref<FileSystemWriteContextHandle>>())));
}

}  // namespace workerd::api
