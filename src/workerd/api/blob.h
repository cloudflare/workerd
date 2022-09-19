// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/jsg/jsg.h>
#include "streams.h"
#include "util.h"

namespace workerd::api {

class Blob: public jsg::Object {
public:
  Blob(kj::Array<byte> data, kj::String type)
      : ownData(kj::mv(data)), data(ownData.get<kj::Array<byte>>()), type(kj::mv(type)) {}
  Blob(jsg::Ref<Blob> parent, kj::ArrayPtr<const byte> data, kj::String type)
      : ownData(kj::mv(parent)), data(data), type(kj::mv(type)) {}

  inline kj::ArrayPtr<const byte> getData() const KJ_LIFETIMEBOUND { return data; }

  // ---------------------------------------------------------------------------
  // JS API

  struct Options {
    jsg::Optional<kj::String> type;
    jsg::Unimplemented endings;

    JSG_STRUCT(type, endings);
  };

  typedef kj::Array<kj::OneOf<kj::Array<const byte>, kj::String, jsg::Ref<Blob>>> Bits;

  static jsg::Ref<Blob> constructor(jsg::Optional<Bits> bits, jsg::Optional<Options> options);

  int getSize() { return data.size(); }
  kj::StringPtr getType() { return type; }

  jsg::Ref<Blob> slice(jsg::Optional<int> start, jsg::Optional<int> end,
                        jsg::Optional<kj::String> type);

  jsg::Promise<kj::Array<kj::byte>> arrayBuffer(v8::Isolate* isolate);
  jsg::Promise<kj::String> text(v8::Isolate* isolate);
  jsg::Ref<ReadableStream> stream(v8::Isolate* isolate);

  JSG_RESOURCE_TYPE(Blob, CompatibilityFlags::Reader flags) {
    if (flags.getJsgPropertyOnPrototypeTemplate()) {
      JSG_READONLY_PROTOTYPE_PROPERTY(size, getSize);
      JSG_READONLY_PROTOTYPE_PROPERTY(type, getType);
    } else {
      JSG_READONLY_INSTANCE_PROPERTY(size, getSize);
      JSG_READONLY_INSTANCE_PROPERTY(type, getType);
    }

    JSG_METHOD(slice);
    JSG_METHOD(arrayBuffer);
    JSG_METHOD(text);
    JSG_METHOD(stream);
  }

private:
  kj::OneOf<kj::Array<byte>, jsg::Ref<Blob>> ownData;
  kj::ArrayPtr<const byte> data;
  kj::String type;

  void visitForGc(jsg::GcVisitor& visitor) {
    KJ_IF_MAYBE(b, ownData.tryGet<jsg::Ref<Blob>>()) {
      visitor.visit(*b);
    }
  }

  class BlobInputStream;
};

class File: public Blob {
public:
  File(kj::Array<byte> data, kj::String name, kj::String type, double lastModified)
      : Blob(kj::mv(data), kj::mv(type)),
        name(kj::mv(name)), lastModified(lastModified) {}

  struct Options {
    jsg::Optional<kj::String> type;
    jsg::Optional<double> lastModified;
    jsg::Unimplemented endings;

    JSG_STRUCT(type, lastModified, endings);
  };

  static jsg::Ref<File> constructor(jsg::Optional<Bits> bits,
      kj::String name, jsg::Optional<Options> options);

  kj::StringPtr getName() { return name; }
  double getLastModified() { return lastModified; }

  JSG_RESOURCE_TYPE(File, CompatibilityFlags::Reader flags) {
    JSG_INHERIT(Blob);
    if (flags.getJsgPropertyOnPrototypeTemplate()) {
      JSG_READONLY_PROTOTYPE_PROPERTY(name, getName);
      JSG_READONLY_PROTOTYPE_PROPERTY(lastModified, getLastModified);
    } else {
      JSG_READONLY_INSTANCE_PROPERTY(name, getName);
      JSG_READONLY_INSTANCE_PROPERTY(lastModified, getLastModified);
    }
  }

private:
  kj::String name;
  double lastModified;
};

#define EW_BLOB_ISOLATE_TYPES \
  api::Blob,                  \
  api::Blob::Options,         \
  api::File,                  \
  api::File::Options

}  // namespace workerd::api
