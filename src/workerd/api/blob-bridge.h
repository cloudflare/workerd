// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/jsg/jsg.h>

namespace workerd::api {

class Blob;
class File;
class JsFile;

// A Blob of either implementation: the C++ class in blob.h, or the TypeScript one
// (src/per_isolate/blob/), which replaces it under the TS_BLOB autogate. C++ takes and makes Blobs
// through this type, so that it need not know which implementation the global Blob is.
//
// A TypeScript Blob keeps its state under V8-private properties, which C++ reads and writes
// without running JavaScript: `workerd:Blob:bytes`, a Uint8Array nothing else can reach and
// nothing ever writes to, and `workerd:Blob:type`, a string. A File adds `workerd:File:name` and
// `workerd:File:lastModified`. The bytes are the brand: only a Blob has them. Neither
// recognizes a proxy.
//
// Unlike JsUrl, a JsBlob can be read and made in a no-JS scope, such as V8's deserializer's.
class JsBlob {
 public:
  using Impl = kj::OneOf<jsg::Ref<Blob>, jsg::JsRef<jsg::JsObject>>;

  explicit JsBlob(jsg::Ref<Blob> blob);

  // Defined in blob-bridge.c++, where Blob is complete.
  JsBlob(JsBlob&&);
  JsBlob& operator=(JsBlob&&);
  ~JsBlob() noexcept(false);
  KJ_DISALLOW_COPY(JsBlob);

  JsBlob addRef(jsg::Lock& js);

  // The Blob's bytes, valid while the lock is held and this JsBlob lives.
  kj::ArrayPtr<const kj::byte> getData(jsg::Lock& js) const KJ_LIFETIMEBOUND;

  // The size of the Blob's bytes.
  size_t getSize(jsg::Lock& js) const;

  // The Blob's type, the empty string if it has none.
  kj::String getType(jsg::Lock& js) const;

  // A File if this is one.
  kj::Maybe<JsFile> tryGetFile(jsg::Lock& js);

  // The Blob as a JavaScript value, for code that hands it to JavaScript without a type wrapper.
  jsg::JsValue toJsValue(jsg::Lock& js) &&;

  // A Blob of whichever implementation the global Blob is, over `data`, which it keeps rather than
  // copies: nothing may write to it again. `type` is taken as it is, unnormalized.
  static JsBlob create(jsg::Lock& js, jsg::JsBufferSource data, kj::String type);

  // An empty Blob of whichever implementation the global Blob is.
  static JsBlob createEmpty(jsg::Lock& js, kj::String type);

  // None unless `value` is a Blob of either implementation.
  static kj::Maybe<JsBlob> tryUnwrap(jsg::Lock& js, jsg::JsValue value);

  // Whether the TypeScript Blob is the global Blob: whether the TS_BLOB autogate is enabled.
  static bool isTsImplementation(jsg::Lock& js);

  // Described to RTTI, and so to the generated types, as the C++ Blob, and named as it is in error
  // messages.
  using JsgRttiDelegate = jsg::Ref<Blob>;
  static constexpr const char* jsgTypeName = "Blob";

  static v8::Local<v8::Value> jsgWrap(auto& typeWrapper,
      jsg::Lock& js,
      v8::Local<v8::Context> context,
      kj::Maybe<v8::Local<v8::Object>> creator,
      JsBlob value) {
    KJ_SWITCH_ONEOF(value.impl) {
      KJ_CASE_ONEOF(blob, jsg::Ref<Blob>) {
        return typeWrapper.wrap(js, context, creator, kj::mv(blob));
      }
      KJ_CASE_ONEOF(object, jsg::JsRef<jsg::JsObject>) {
        return object.getHandle(js);
      }
    }
    KJ_UNREACHABLE;
  }

  static kj::Maybe<JsBlob> jsgTryUnwrap(auto& typeWrapper,
      jsg::Lock& js,
      v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    KJ_IF_SOME(blob,
        typeWrapper.tryUnwrap(
            js, context, handle, static_cast<jsg::Ref<Blob>*>(nullptr), parentObject)) {
      return JsBlob(kj::mv(blob));
    }
    return tryUnwrapTs(js, jsg::JsValue(handle));
  }

  void visitForGc(jsg::GcVisitor& visitor);

  // Defined in blob-bridge.c++, where Blob is complete.
  JSG_MEMORY_INFO(JsBlob);

 protected:
  explicit JsBlob(Impl impl);

  static kj::Maybe<JsBlob> tryUnwrapTs(jsg::Lock& js, jsg::JsValue value);

  Impl impl;

  friend class JsFile;
};

// A File of either implementation (see JsBlob).
class JsFile: public JsBlob {
 public:
  explicit JsFile(jsg::Ref<File> file);

  // Defined in blob-bridge.c++, where Blob is complete.
  JsFile(JsFile&&);
  JsFile& operator=(JsFile&&);
  ~JsFile() noexcept(false);
  KJ_DISALLOW_COPY(JsFile);

  JsFile addRef(jsg::Lock& js);

  kj::String getName(jsg::Lock& js) const;
  double getLastModified(jsg::Lock& js) const;

  // A File of whichever implementation the global File is, over `data`, which it keeps rather than
  // copies (see JsBlob::create()).
  static JsFile create(jsg::Lock& js,
      jsg::JsBufferSource data,
      kj::String name,
      kj::String type,
      double lastModified);

  // A File named `name`, modified now, with the bytes and type of `blob`, which it shares rather
  // than copies.
  static JsFile fromBlob(jsg::Lock& js, JsBlob& blob, kj::String name);

  // Described to RTTI, and so to the generated types, as the C++ File, and named as it is in error
  // messages.
  using JsgRttiDelegate = jsg::Ref<File>;
  static constexpr const char* jsgTypeName = "File";

  static v8::Local<v8::Value> jsgWrap(auto& typeWrapper,
      jsg::Lock& js,
      v8::Local<v8::Context> context,
      kj::Maybe<v8::Local<v8::Object>> creator,
      JsFile value) {
    return JsBlob::jsgWrap(typeWrapper, js, context, creator, kj::mv(value));
  }

  static kj::Maybe<JsFile> jsgTryUnwrap(auto& typeWrapper,
      jsg::Lock& js,
      v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    KJ_IF_SOME(file,
        typeWrapper.tryUnwrap(
            js, context, handle, static_cast<jsg::Ref<File>*>(nullptr), parentObject)) {
      return JsFile(kj::mv(file));
    }
    return tryUnwrapTsFile(js, jsg::JsValue(handle));
  }

 private:
  explicit JsFile(Impl impl);

  static kj::Maybe<JsFile> tryUnwrapTsFile(jsg::Lock& js, jsg::JsValue value);

  friend class JsBlob;
};

}  // namespace workerd::api
