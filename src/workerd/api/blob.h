// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/io/compatibility-date.capnp.h>
#include <workerd/jsg/jsg.h>

namespace workerd::api {

class ReadableStream;

// An implementation of the Web Platform Standard Blob API
class Blob: public jsg::Object {
 public:
  Blob(jsg::Lock& js, jsg::BufferSource data, kj::String type);
  Blob(jsg::Lock& js, kj::Array<byte> data, kj::String type);
  Blob(jsg::Ref<Blob> parent, kj::ArrayPtr<const byte> data, kj::String type);

  kj::ArrayPtr<const byte> getData() const KJ_LIFETIMEBOUND;

  // ---------------------------------------------------------------------------
  // JS API

  struct Options {
    jsg::Optional<kj::String> type;
    jsg::Unimplemented endings;

    JSG_STRUCT(type, endings);
  };

  typedef kj::Array<kj::OneOf<kj::Array<const byte>, kj::String, jsg::Ref<Blob>>> Bits;

  static jsg::Ref<Blob> constructor(
      jsg::Lock& js, jsg::Optional<Bits> bits, jsg::Optional<Options> options);

  int getSize() const {
    return data.size();
  }
  kj::StringPtr getType() const {
    return type;
  }

  jsg::Ref<Blob> slice(
      jsg::Optional<int> start, jsg::Optional<int> end, jsg::Optional<kj::String> type);

  jsg::Promise<jsg::BufferSource> arrayBuffer(jsg::Lock& js);
  jsg::Promise<jsg::BufferSource> bytes(jsg::Lock& js);
  jsg::Promise<kj::String> text(jsg::Lock& js);
  jsg::Ref<ReadableStream> stream();

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
    JSG_METHOD(bytes);
    JSG_METHOD(text);
    JSG_METHOD(stream);

    JSG_TS_OVERRIDE({
      bytes(): Promise<Uint8Array>;
    });
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    KJ_SWITCH_ONEOF(ownData) {
      KJ_CASE_ONEOF(data, jsg::BufferSource) {
        tracker.trackField("ownData", data);
      }
      KJ_CASE_ONEOF(data, jsg::Ref<Blob>) {
        tracker.trackField("ownData", data);
      }
      KJ_CASE_ONEOF(data, kj::Array<kj::byte>) {
        tracker.trackField("ownData", data);
      }
    }
    tracker.trackField("type", type);
  }

 private:
  Blob(kj::Array<byte> data, kj::String type);

  // Using a jsg::BufferSource to store the ownData allows the associated isolate
  // to track the external data allocation correctly.
  // The Variation that uses kj::Array<kj::byte> only is used only in very
  // specific cases (i.e. the internal fiddle service) where we parse FormData
  // outside of the isolate lock.
  kj::OneOf<jsg::BufferSource, kj::Array<kj::byte>, jsg::Ref<Blob>> ownData;
  kj::ArrayPtr<const byte> data;
  kj::String type;

  void visitForGc(jsg::GcVisitor& visitor) {
    KJ_SWITCH_ONEOF(ownData) {
      KJ_CASE_ONEOF(b, jsg::BufferSource) {
        visitor.visit(b);
      }
      KJ_CASE_ONEOF(b, jsg::Ref<Blob>) {
        visitor.visit(b);
      }
      KJ_CASE_ONEOF(b, kj::Array<kj::byte>) {}
    }
  }

  class BlobInputStream;
  friend class File;
};

// An implementation of the Web Platform Standard File API
class File: public Blob {
 public:
  // This constructor variation is used when a File is created outside of the isolate
  // lock. This is currently only the case when parsing FormData outside of running
  // JavaScript (such as in the internal fiddle service).
  File(kj::Array<byte> data, kj::String name, kj::String type, double lastModified);
  File(jsg::Lock& js, kj::Array<byte> data, kj::String name, kj::String type, double lastModified);
  File(jsg::Ref<Blob> parent,
      kj::ArrayPtr<const byte> data,
      kj::String name,
      kj::String type,
      double lastModified);

  struct Options {
    jsg::Optional<kj::String> type;
    jsg::Optional<double> lastModified;
    jsg::Unimplemented endings;

    JSG_STRUCT(type, lastModified, endings);
  };

  static jsg::Ref<File> constructor(
      jsg::Lock& js, jsg::Optional<Bits> bits, kj::String name, jsg::Optional<Options> options);

  kj::StringPtr getName() {
    return name;
  }
  double getLastModified() {
    return lastModified;
  }

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

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackField("name", name);
  }

 private:
  kj::String name;
  double lastModified;
};

#define EW_BLOB_ISOLATE_TYPES api::Blob, api::Blob::Options, api::File, api::File::Options

}  // namespace workerd::api
