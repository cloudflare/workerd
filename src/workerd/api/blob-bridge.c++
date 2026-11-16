// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "blob-bridge.h"

#include <workerd/api/blob.h>
#include <workerd/api/per-isolate-bootstrap.h>
#include <workerd/io/io-util.h>
#include <workerd/io/worker-interface.capnp.h>
#include <workerd/jsg/ser.h>
#include <workerd/util/autogate.h>

namespace workerd::api {

namespace {

// The names of the V8-private properties a TypeScript Blob keeps its state under, shared with
// src/per_isolate/blob/blob.ts.
constexpr kj::StringPtr BYTES_KEY = "workerd:Blob:bytes"_kj;
constexpr kj::StringPtr TYPE_KEY = "workerd:Blob:type"_kj;
constexpr kj::StringPtr NAME_KEY = "workerd:File:name"_kj;
constexpr kj::StringPtr LAST_MODIFIED_KEY = "workerd:File:lastModified"_kj;

// The bootstrap module whose exports describe how to make an instance (see blob/cpp_exports.ts).
// It is loaded only under the TS_BLOB autogate.
constexpr kj::StringPtr CPP_EXPORTS = "blob/cpp_exports"_kj;

jsg::JsObject getCppExports(jsg::Lock& js) {
  auto exports = KJ_REQUIRE_NONNULL(
      tryGetBootstrapExport(js, CPP_EXPORTS), "the TypeScript Blob is not loaded");
  return KJ_REQUIRE_NONNULL(exports.tryCast<jsg::JsObject>());
}

bool tsBlobEnabled() {
  return util::Autogate::isEnabled(util::AutogateKey::TS_BLOB);
}

// The TypeScript Blob or File `value` is, if it is one. Reads only private properties, so runs no
// JavaScript. Without the autogate there are none, so nothing is looked up.
kj::Maybe<jsg::JsObject> tryGetTsBlob(jsg::Lock& js, jsg::JsValue value) {
  if (!tsBlobEnabled()) return kj::none;
  KJ_IF_SOME(object, value.tryCast<jsg::JsObject>()) {
    if (!v8::Local<v8::Value>(object)->IsProxy() &&
        object.getPrivate(js, BYTES_KEY).isUint8Array()) {
      return object;
    }
  }
  return kj::none;
}

// A new instance of the TypeScript Blob or File, made as the TypeScript constructor makes one but
// without running JavaScript: from the class's prototype, with the class's own instance
// properties, if it has any under the dated flags, and its serialization mark.
jsg::JsObject newTsInstance(jsg::Lock& js,
    jsg::JsUint8Array bytes,
    kj::StringPtr type,
    kj::Maybe<kj::Tuple<kj::StringPtr, double>> file) {
  auto exports = getCppExports(js);
  bool isFile = file != kj::none;
  auto prototype = KJ_REQUIRE_NONNULL(
      exports.get(js, isFile ? "FilePrototype"_kj : "BlobPrototype"_kj).tryCast<jsg::JsObject>());

  auto context = js.v8Context();
  // v8::Object::New() given a prototype makes an object in dictionary mode, slow to use. One
  // given a new prototype keeps fast properties.
  v8::Local<v8::Object> instance = v8::Object::New(js.v8Isolate);
  jsg::check(instance->SetPrototype(context, v8::Local<v8::Object>(prototype)));

  jsg::JsObject object(instance);
  object.setPrivate(js, BYTES_KEY, bytes);
  object.setPrivate(js, TYPE_KEY, js.str(type));
  KJ_IF_SOME(f, file) {
    object.setPrivate(js, NAME_KEY, js.str(kj::get<0>(f)));
    object.setPrivate(js, LAST_MODIFIED_KEY, js.num(kj::get<1>(f)));
  }
  // A Blob serializes as a C++ one does; a File, as in C++, does not serialize at all.
  object.setPrivate(js, jsg::JS_SERIALIZE_MODE_KEY,
      js.num(isFile ? 0u : static_cast<uint>(rpc::SerializationTag::BLOB)));

  // Under the legacy layout, the attributes are also own read-only data properties of each
  // instance, in JSG's order: the subclass's first.
  if (exports.get(js, "instanceProperties"_kj).isTrue()) {
    auto define = [&](kj::StringPtr name, v8::Local<v8::Value> value) {
      jsg::check(instance->DefineOwnProperty(
          context, v8::Local<v8::String>(js.strIntern(name)), value, v8::ReadOnly));
    };
    KJ_IF_SOME(f, file) {
      define("lastModified"_kj, js.num(kj::get<1>(f)));
      define("name"_kj, js.str(kj::get<0>(f)));
    }
    define("type"_kj, js.str(type));
    define("size"_kj, js.num(static_cast<double>(bytes.size())));
  }
  return object;
}

jsg::JsUint8Array getTsBytes(jsg::Lock& js, const jsg::JsRef<jsg::JsObject>& object) {
  return KJ_ASSERT_NONNULL(
      object.getHandle(js).getPrivate(js, BYTES_KEY).tryCast<jsg::JsUint8Array>());
}

}  // namespace

JsBlob::JsBlob(jsg::Ref<Blob> blob): impl(kj::mv(blob)) {}

JsBlob JsBlob::addRef(jsg::Lock& js) {
  KJ_SWITCH_ONEOF(impl) {
    KJ_CASE_ONEOF(blob, jsg::Ref<Blob>) {
      return JsBlob(blob.addRef());
    }
    KJ_CASE_ONEOF(object, jsg::JsRef<jsg::JsObject>) {
      return JsBlob(object.addRef(js));
    }
  }
  KJ_UNREACHABLE;
}

kj::ArrayPtr<const kj::byte> JsBlob::getData(jsg::Lock& js) const {
  KJ_SWITCH_ONEOF(impl) {
    KJ_CASE_ONEOF(blob, jsg::Ref<Blob>) {
      return blob->getData(js);
    }
    KJ_CASE_ONEOF(object, jsg::JsRef<jsg::JsObject>) {
      // The Uint8Array is reachable only from the instance, so it lives as long as this JsBlob.
      return getTsBytes(js, object).asArrayPtr();
    }
  }
  KJ_UNREACHABLE;
}

size_t JsBlob::getSize(jsg::Lock& js) const {
  return getData(js).size();
}

kj::String JsBlob::getType(jsg::Lock& js) const {
  KJ_SWITCH_ONEOF(impl) {
    KJ_CASE_ONEOF(blob, jsg::Ref<Blob>) {
      return kj::str(blob->getType());
    }
    KJ_CASE_ONEOF(object, jsg::JsRef<jsg::JsObject>) {
      return KJ_ASSERT_NONNULL(
          object.getHandle(js).getPrivate(js, TYPE_KEY).tryCast<jsg::JsString>())
          .toString(js);
    }
  }
  KJ_UNREACHABLE;
}

kj::Maybe<JsFile> JsBlob::tryGetFile(jsg::Lock& js) {
  KJ_SWITCH_ONEOF(impl) {
    KJ_CASE_ONEOF(blob, jsg::Ref<Blob>) {
      if (dynamic_cast<const File*>(blob.get()) != nullptr) {
        return JsFile(Impl(blob.addRef()));
      }
      return kj::none;
    }
    KJ_CASE_ONEOF(object, jsg::JsRef<jsg::JsObject>) {
      if (object.getHandle(js).getPrivate(js, NAME_KEY).isString()) {
        return JsFile(Impl(object.addRef(js)));
      }
      return kj::none;
    }
  }
  KJ_UNREACHABLE;
}

jsg::JsValue JsBlob::toJsValue(jsg::Lock& js) && {
  KJ_SWITCH_ONEOF(impl) {
    KJ_CASE_ONEOF(blob, jsg::Ref<Blob>) {
      auto& handler = KJ_ASSERT_NONNULL(js.tryGetTypeHandler<jsg::Ref<Blob>>());
      return jsg::JsValue(handler.wrap(js, kj::mv(blob)));
    }
    KJ_CASE_ONEOF(object, jsg::JsRef<jsg::JsObject>) {
      return object.getHandle(js);
    }
  }
  KJ_UNREACHABLE;
}

bool JsBlob::isTsImplementation(jsg::Lock& js) {
  // As global-scope.h decides which Blob is global. Under the autogate the bootstrap has loaded
  // the TypeScript one, which newTsInstance() requires.
  return tsBlobEnabled();
}

JsBlob JsBlob::create(jsg::Lock& js, jsg::JsBufferSource data, kj::String type) {
  if (isTsImplementation(js)) {
    jsg::JsUint8Array bytes = data;
    return JsBlob(Impl(newTsInstance(js, bytes, type, kj::none).addRef(js)));
  }
  return JsBlob(js.alloc<Blob>(js, data, kj::mv(type)));
}

JsBlob JsBlob::createEmpty(jsg::Lock& js, kj::String type) {
  if (isTsImplementation(js)) {
    return JsBlob(
        Impl(newTsInstance(js, jsg::JsUint8Array::create(js, 0), type, kj::none).addRef(js)));
  }
  return JsBlob(js.alloc<Blob>(kj::mv(type)));
}

kj::Maybe<JsBlob> JsBlob::tryUnwrapTs(jsg::Lock& js, jsg::JsValue value) {
  return tryGetTsBlob(js, value).map(
      [&](jsg::JsObject object) { return JsBlob(Impl(object.addRef(js))); });
}

kj::Maybe<JsBlob> JsBlob::tryUnwrap(jsg::Lock& js, jsg::JsValue value) {
  KJ_IF_SOME(handler, js.tryGetTypeHandler<jsg::Ref<Blob>>()) {
    KJ_IF_SOME(blob, handler.tryUnwrap(js, value)) {
      return JsBlob(kj::mv(blob));
    }
  }
  return tryUnwrapTs(js, value);
}

JsBlob::JsBlob(Impl impl): impl(kj::mv(impl)) {}
JsBlob::JsBlob(JsBlob&&) = default;
JsBlob& JsBlob::operator=(JsBlob&&) = default;
JsBlob::~JsBlob() noexcept(false) = default;
JsFile::JsFile(Impl impl): JsBlob(kj::mv(impl)) {}
JsFile::JsFile(JsFile&&) = default;
JsFile& JsFile::operator=(JsFile&&) = default;
JsFile::~JsFile() noexcept(false) = default;

void JsBlob::jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const {
  KJ_SWITCH_ONEOF(impl) {
    KJ_CASE_ONEOF(blob, jsg::Ref<Blob>) {
      tracker.trackField("blob", blob);
    }
    KJ_CASE_ONEOF(object, jsg::JsRef<jsg::JsObject>) {
      tracker.trackField("blob", object);
    }
  }
}

void JsBlob::visitForGc(jsg::GcVisitor& visitor) {
  KJ_SWITCH_ONEOF(impl) {
    KJ_CASE_ONEOF(blob, jsg::Ref<Blob>) {
      visitor.visit(blob);
    }
    KJ_CASE_ONEOF(object, jsg::JsRef<jsg::JsObject>) {
      visitor.visit(object);
    }
  }
}

// =======================================================================================

JsFile::JsFile(jsg::Ref<File> file): JsBlob(Impl(jsg::Ref<Blob>(kj::mv(file)))) {}

JsFile JsFile::addRef(jsg::Lock& js) {
  KJ_SWITCH_ONEOF(impl) {
    KJ_CASE_ONEOF(blob, jsg::Ref<Blob>) {
      return JsFile(Impl(blob.addRef()));
    }
    KJ_CASE_ONEOF(object, jsg::JsRef<jsg::JsObject>) {
      return JsFile(Impl(object.addRef(js)));
    }
  }
  KJ_UNREACHABLE;
}

kj::String JsFile::getName(jsg::Lock& js) const {
  KJ_SWITCH_ONEOF(impl) {
    KJ_CASE_ONEOF(blob, jsg::Ref<Blob>) {
      return kj::str(kj::downcast<const File>(*blob).getName());
    }
    KJ_CASE_ONEOF(object, jsg::JsRef<jsg::JsObject>) {
      return KJ_ASSERT_NONNULL(
          object.getHandle(js).getPrivate(js, NAME_KEY).tryCast<jsg::JsString>())
          .toString(js);
    }
  }
  KJ_UNREACHABLE;
}

double JsFile::getLastModified(jsg::Lock& js) const {
  KJ_SWITCH_ONEOF(impl) {
    KJ_CASE_ONEOF(blob, jsg::Ref<Blob>) {
      return kj::downcast<const File>(*blob).getLastModified();
    }
    KJ_CASE_ONEOF(object, jsg::JsRef<jsg::JsObject>) {
      auto value = KJ_ASSERT_NONNULL(
          object.getHandle(js).getPrivate(js, LAST_MODIFIED_KEY).tryCast<jsg::JsNumber>());
      return KJ_ASSERT_NONNULL(value.value(js));
    }
  }
  KJ_UNREACHABLE;
}

JsFile JsFile::create(jsg::Lock& js,
    jsg::JsBufferSource data,
    kj::String name,
    kj::String type,
    double lastModified) {
  if (isTsImplementation(js)) {
    jsg::JsUint8Array bytes = data;
    return JsFile(
        Impl(newTsInstance(js, bytes, type, kj::tuple(name.asPtr(), lastModified)).addRef(js)));
  }
  return JsFile(js.alloc<File>(js, data, kj::mv(name), kj::mv(type), lastModified));
}

JsFile JsFile::fromBlob(jsg::Lock& js, JsBlob& blob, kj::String name) {
  KJ_SWITCH_ONEOF(blob.impl) {
    KJ_CASE_ONEOF(b, jsg::Ref<Blob>) {
      // The file is created as a view of the same data as the blob, to avoid copying it.
      return JsFile(js.alloc<File>(
          js, b.addRef(), b->getData(js), kj::mv(name), kj::str(b->getType()), dateNow()));
    }
    KJ_CASE_ONEOF(object, jsg::JsRef<jsg::JsObject>) {
      // The bytes are never written to, so the File can share them.
      auto type = blob.getType(js);
      return JsFile(
          Impl(newTsInstance(js, getTsBytes(js, object), type, kj::tuple(name.asPtr(), dateNow()))
                   .addRef(js)));
    }
  }
  KJ_UNREACHABLE;
}

kj::Maybe<JsFile> JsFile::tryUnwrapTsFile(jsg::Lock& js, jsg::JsValue value) {
  KJ_IF_SOME(object, tryGetTsBlob(js, value)) {
    if (object.getPrivate(js, NAME_KEY).isString()) {
      return JsFile(Impl(object.addRef(js)));
    }
  }
  return kj::none;
}

}  // namespace workerd::api
