// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Structured serialization for Rust JSG, in the format of C++ JSG's jsg::Serializer and
// jsg::Deserializer (workerd/jsg/ser.c++), which can read what these shims write and the other
// way round. They drive V8's serializer with delegates of their own. Native errors are written and
// read by writeNativeError() and readNativeError(), which jsg::Serializer and jsg::Deserializer
// use too, and workerd API objects, such as Blob and Headers, are handed to C++ JSG through
// writeApiObject() and readApiObject().

#include "ffi.h"

#include <workerd/jsg/ser.h>
#include <workerd/rust/jsg/ffi-inl.h>
#include <workerd/rust/jsg/v8.rs.h>

#include <v8-value-serializer-version.h>
#include <v8-value-serializer.h>

#include <kj/debug.h>

#include <cstdlib>

namespace workerd::rust::jsg {
namespace {

// Throws the DOMException named DataCloneError that jsg::Serializer throws, unless execution is
// terminating.
void throwDataCloneError(v8::Isolate* isolate, v8::Local<v8::String> message) {
  if (isolate->IsExecutionTerminating()) return;
  v8::Local<v8::Value> exception;
  if (::workerd::jsg::newDataCloneError(isolate, message).ToLocal(&exception)) {
    isolate->ThrowException(exception);
  }
}

class Serializer final: public v8::ValueSerializer::Delegate {
 public:
  Serializer(v8::Isolate* isolate, bool errorsAreHostObjects)
      : isolate(isolate),
        errorsAreHostObjects(errorsAreHostObjects),
        sharedArrayBuffers(isolate),
        ser(isolate, this) {
    ser.WriteHeader();
  }

  // Returns false if serialization threw, leaving the exception pending.
  bool write(v8::Local<v8::Value> value) {
    return ser.WriteValue(isolate->GetCurrentContext(), value).IsJust();
  }

  ::rust::Vec<uint8_t> release() {
    auto [data, size] = ser.Release();
    KJ_DEFER(free(data));
    ::rust::Vec<uint8_t> result;
    result.reserve(size);
    for (auto byte: kj::arrayPtr(data, size)) {
      result.push_back(byte);
    }
    return result;
  }

  void ThrowDataCloneError(v8::Local<v8::String> message) override {
    throwDataCloneError(isolate, message);
  }

  bool HasCustomHostObject(v8::Isolate*) override {
    // V8 already treats objects with internal fields, such as API objects, as host objects.
    return errorsAreHostObjects;
  }

  v8::Maybe<bool> IsHostObject(v8::Isolate*, v8::Local<v8::Object> object) override {
    return v8::Just(object->InternalFieldCount() > 0 || object->IsNativeError());
  }

  v8::Maybe<bool> WriteHostObject(v8::Isolate*, v8::Local<v8::Object> object) override {
    if (errorsAreHostObjects && object->IsNativeError()) {
      return ::workerd::jsg::writeNativeError(isolate, ser, object);
    }
    return ::workerd::jsg::writeApiObject(isolate, ser, object);
  }

  // Numbers each SharedArrayBuffer, as jsg::Serializer does. The buffers are not passed on: the
  // data refers to them by number, and no deserializer is given them.
  v8::Maybe<uint32_t> GetSharedArrayBufferId(
      v8::Isolate*, v8::Local<v8::SharedArrayBuffer> buffer) override {
    for (auto i: kj::zeroTo(sharedArrayBuffers.size())) {
      if (sharedArrayBuffers[i] == buffer) return v8::Just(static_cast<uint32_t>(i));
    }
    sharedArrayBuffers.push_back(buffer);
    return v8::Just(static_cast<uint32_t>(sharedArrayBuffers.size() - 1));
  }

 private:
  v8::Isolate* isolate;
  bool errorsAreHostObjects;
  v8::LocalVector<v8::SharedArrayBuffer> sharedArrayBuffers;
  v8::ValueSerializer ser;
};

class Deserializer final: public v8::ValueDeserializer::Delegate {
 public:
  Deserializer(v8::Isolate* isolate, ::rust::Slice<const uint8_t> data)
      : isolate(isolate),
        deser(isolate, data.data(), data.size(), this) {}

  // Reads the header as jsg::Deserializer does. If it is missing or of an unsupported version,
  // throws a KJ exception, which reaches JavaScript as an internal error; if execution is
  // terminating, returns false.
  bool readHeader(::rust::Slice<const uint8_t> data) {
    v8::TryCatch tryCatch(isolate);
    if (deser.ReadHeader(isolate->GetCurrentContext()).IsJust()) return true;
    MaybeLocal exception;
    if (!catch_exception(tryCatch, exception)) return false;
    // Only header metadata, never bytes of the value.
    auto header = data.empty() ? "empty" : data[0] == 0xff ? "present" : "missing";
    KJ_FAIL_REQUIRE("Unable to deserialize cloned data due to invalid or unsupported version.",
        data.size(), header, deser.GetWireFormatVersion(),
        v8::CurrentValueSerializerFormatVersion());
  }

  // Returns none if deserialization threw, leaving the exception pending.
  v8::MaybeLocal<v8::Value> read() {
    return deser.ReadValue(isolate->GetCurrentContext());
  }

  v8::MaybeLocal<v8::Object> ReadHostObject(v8::Isolate*) override {
    uint32_t tag;
    if (!deser.ReadUint32(&tag)) return throwCorrupt();
    if (tag == ::workerd::jsg::SERIALIZATION_TAG_NATIVE_ERROR) {
      return ::workerd::jsg::readNativeError(isolate, deser, ::workerd::jsg::PreserveStack::YES);
    }
    return ::workerd::jsg::readApiObject(isolate, deser, tag);
  }

  // V8's GetSharedArrayBufferFromId() throws for a SharedArrayBuffer, which the data only refers
  // to by number.

 private:
  v8::MaybeLocal<v8::Object> throwCorrupt() {
    isolate_throw_internal_error(isolate, "deserialization failure, possible corruption");
    return {};
  }

  v8::Isolate* isolate;
  v8::ValueDeserializer deser;
};

}  // namespace

// Serialization
bool local_serialize(Isolate* isolate,
    const Local& value,
    bool errors_are_host_objects,
    ::rust::Vec<uint8_t>& result) {
  Serializer serializer(isolate, errors_are_host_objects);
  if (!serializer.write(local_as_ref_from_ffi<v8::Value>(value))) return false;
  result = serializer.release();
  return true;
}

MaybeLocal value_deserialize(Isolate* isolate, ::rust::Slice<const uint8_t> data) {
  Deserializer deserializer(isolate, data);
  if (!deserializer.readHeader(data)) return {};
  return maybe_local_to_ffi(deserializer.read());
}

}  // namespace workerd::rust::jsg
