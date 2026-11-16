// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Structured serialization for Rust JSG, in the format of C++ JSG's jsg::Serializer and
// jsg::Deserializer (workerd/jsg/ser.c++), which can read what these shims write and the other
// way round. They drive V8's serializer with delegates of their own, which decide what is a host
// object and hand each to C++ JSG through the hooks in workerd/jsg/ser.h: native errors through
// writeNativeError() and readNativeError(), instances of the runtime's classes implemented in
// JavaScript through JsSerializableWriter and JsSerializableReader, and workerd API objects, such
// as Blob and Headers, and DOMExceptions, however they are implemented, through writeApiObject()
// and readApiObject().

#include "ffi.h"

#include <workerd/jsg/dom-exception.h>
#include <workerd/jsg/ser.h>
#include <workerd/rust/jsg/ffi-inl.h>
#include <workerd/rust/jsg/v8.rs.h>

#include <v8-value-serializer-version.h>
#include <v8-value-serializer.h>

#include <kj/debug.h>
#include <kj/vector.h>

#include <cstdlib>

namespace workerd::rust::jsg {
namespace {

using ::workerd::jsg::SERIALIZATION_TAG_JS_SERIALIZABLE;
using ::workerd::jsg::SERIALIZATION_TAG_NATIVE_ERROR;

// The oldest version of V8's format that jsg::Serializer writes.
constexpr uint32_t MIN_SERIALIZATION_VERSION = 13;
// The message of the DataCloneError that jsg::Deserializer throws when a host object's data ends
// too soon.
constexpr kj::StringPtr MALFORMED_DATA_MESSAGE =
    "Deserialization failed: the data is malformed."_kj;

v8::Local<v8::String> newString(v8::Isolate* isolate, kj::StringPtr str) {
  return v8::String::NewFromUtf8(
      isolate, str.begin(), v8::NewStringType::kNormal, static_cast<int>(str.size()))
      .ToLocalChecked();
}

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
        prototypeOfObject(v8::Object::New(isolate)->GetPrototype()),
        jsSerializeModeKey(v8::Private::ForApi(
            isolate, newString(isolate, ::workerd::jsg::JS_SERIALIZE_MODE_KEY))),
        domExceptionBrandKey(v8::Private::ForApi(
            isolate, newString(isolate, ::workerd::jsg::DOM_EXCEPTION_BRAND_KEY))),
        ser(isolate, this),
        jsSerializables(isolate, ser) {}
  // noexcept(true), as the Delegate's destructor is, though kj::Vector's is not.
  ~Serializer() noexcept(true) {}

  // Writes the header, of `version` if there is one, as jsg::Serializer does for its `version`
  // option. Returns false, leaving a DataCloneError pending, if V8 can't write that version.
  bool writeHeader(kj::Maybe<uint32_t> version) {
    KJ_IF_SOME(v, version) {
      // SetWriteVersion() fails a V8 check, rather than throwing, for a version it can't write.
      if (v < MIN_SERIALIZATION_VERSION || v > v8::CurrentValueSerializerFormatVersion()) {
        throwDataCloneError(isolate,
            newString(isolate, kj::str("Serialization version ", v, " is not supported.")));
        return false;
      }
      if (ser.SetWriteVersion(v).IsNothing()) return false;
    }
    ser.WriteHeader();
    return true;
  }

  // Returns false if serialization threw, leaving the exception pending.
  bool write(v8::Local<v8::Value> value) {
    if (ser.WriteValue(isolate->GetCurrentContext(), value).IsNothing()) return false;
    return jsSerializables.writePendingData().IsJust();
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
    // V8 already treats objects with internal fields, such as API objects, as host objects, but
    // not instances of classes implemented in JavaScript (jsg::Serializer), the bootstrap's
    // DOMException, an API wrapper object without them, or native errors.
    return true;
  }

  v8::Maybe<bool> IsHostObject(v8::Isolate*, v8::Local<v8::Object> object) override {
    if (object->InternalFieldCount() > 0) return v8::Just(true);
    bool isJsSerializable;
    if (!hasPrivateUnlessPlain(object, jsSerializeModeKey).To(&isJsSerializable)) {
      return v8::Nothing<bool>();
    }
    bool isDomException;
    if (!hasPrivateUnlessPlain(object, domExceptionBrandKey).To(&isDomException)) {
      return v8::Nothing<bool>();
    }
    return v8::Just(
        isJsSerializable || isDomException || (errorsAreHostObjects && object->IsNativeError()));
  }

  v8::Maybe<bool> WriteHostObject(v8::Isolate*, v8::Local<v8::Object> object) override {
    // An instance of a class implemented in JavaScript is checked for first, as jsg::Serializer
    // checks for it, since such a class may make native errors and DOMExceptions. It has no
    // internal fields, unlike a workerd API object, and no mark is undefined.
    v8::Local<v8::Value> mark = v8::Undefined(isolate);
    if (object->InternalFieldCount() == 0 &&
        !object->GetPrivate(isolate->GetCurrentContext(), jsSerializeModeKey).ToLocal(&mark)) {
      return v8::Nothing<bool>();
    }
    if (!mark->IsUndefined()) return jsSerializables.write(object, mark);
    if (errorsAreHostObjects && object->IsNativeError()) {
      return ::workerd::jsg::writeNativeError(isolate, ser, object);
    }
    // writeApiObject() writes a DOMException as jsg::Serializer does.
    return ::workerd::jsg::writeApiObject(isolate, ser, object);
  }

  // Numbers each SharedArrayBuffer, as jsg::Serializer does. The buffers are not passed on: the
  // data refers to them by number, and no deserializer is given them.
  v8::Maybe<uint32_t> GetSharedArrayBufferId(
      v8::Isolate*, v8::Local<v8::SharedArrayBuffer> buffer) override {
    for (auto i: kj::indices(sharedArrayBuffers)) {
      if (sharedArrayBuffers[i] == buffer) return v8::Just(static_cast<uint32_t>(i));
    }
    sharedArrayBuffers.add(isolate, buffer);
    return v8::Just(static_cast<uint32_t>(sharedArrayBuffers.size() - 1));
  }

 private:
  // Whether `object` carries the private property `key`: jsg::JS_SERIALIZE_MODE_KEY on an instance
  // of a class implemented in JavaScript, or jsg::DOM_EXCEPTION_BRAND_KEY on one of the
  // bootstrap's DOMException. As jsg::Serializer does, an object whose prototype is
  // Object.prototype or null is not checked for either. Reading a private property runs no
  // JavaScript.
  v8::Maybe<bool> hasPrivateUnlessPlain(v8::Local<v8::Object> object, v8::Local<v8::Private> key) {
    auto prototype = object->GetPrototype();
    if (prototype == prototypeOfObject || prototype->IsNull()) return v8::Just(false);
    return object->HasPrivate(isolate->GetCurrentContext(), key);
  }

  v8::Isolate* isolate;
  bool errorsAreHostObjects;
  // Globals, since V8 hands the delegate values in handle scopes of its own, which end before
  // serialization does.
  kj::Vector<v8::Global<v8::SharedArrayBuffer>> sharedArrayBuffers;
  v8::Local<v8::Value> prototypeOfObject;
  v8::Local<v8::Private> jsSerializeModeKey;
  v8::Local<v8::Private> domExceptionBrandKey;
  v8::ValueSerializer ser;
  // Writes the instances of classes implemented in JavaScript, after `ser`, which it writes to.
  ::workerd::jsg::JsSerializableWriter jsSerializables;
};

class Deserializer final: public v8::ValueDeserializer::Delegate {
 public:
  Deserializer(v8::Isolate* isolate, ::rust::Slice<const uint8_t> data, bool preserveStackInErrors)
      : isolate(isolate),
        preserveStackInErrors(preserveStackInErrors),
        deser(isolate, data.data(), data.size(), this),
        jsSerializables(isolate, deser, ::workerd::jsg::PreserveStack(preserveStackInErrors)) {}
  // noexcept(true), as the Delegate's destructor is, though kj::Vector's is not.
  ~Deserializer() noexcept(true) {}

  // Reads the header as jsg::Deserializer does. If it is missing or of an unsupported version,
  // throws a KJ exception, which reaches JavaScript as an internal error; if execution is
  // terminating, returns false.
  bool readHeader(::rust::Slice<const uint8_t> data) {
    // NOLINTNEXTLINE(custom-raw-v8-trycatch): only turns what it caught into a KJ exception.
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
    v8::Local<v8::Value> value;
    if (!deser.ReadValue(isolate->GetCurrentContext()).ToLocal(&value)) return {};
    if (jsSerializables.finish().IsNothing()) return {};
    return value;
  }

  v8::MaybeLocal<v8::Object> ReadHostObject(v8::Isolate*) override {
    uint32_t tag;
    if (!deser.ReadUint32(&tag)) return throwMalformed();
    if (tag == SERIALIZATION_TAG_NATIVE_ERROR) {
      return ::workerd::jsg::readNativeError(
          isolate, deser, ::workerd::jsg::PreserveStack(preserveStackInErrors));
    }
    if (tag == SERIALIZATION_TAG_JS_SERIALIZABLE) return jsSerializables.read();
    return ::workerd::jsg::readApiObject(isolate, deser, tag);
  }

  // V8's GetSharedArrayBufferFromId() throws for a SharedArrayBuffer, which the data only refers
  // to by number.

 private:
  // Throws a DataCloneError, as jsg::Deserializer does, rather than an internal error: like V8's
  // own error for malformed data, since corrupt or crafted bytes are not a bug in the reader.
  v8::MaybeLocal<v8::Object> throwMalformed() {
    throwDataCloneError(isolate, newString(isolate, MALFORMED_DATA_MESSAGE));
    return {};
  }

  v8::Isolate* isolate;
  bool preserveStackInErrors;
  v8::ValueDeserializer deser;
  // Reads the instances of classes implemented in JavaScript, after `deser`, which it reads from.
  ::workerd::jsg::JsSerializableReader jsSerializables;
};

}  // namespace

// Serialization
bool local_serialize(Isolate* isolate,
    const Local& value,
    bool errors_are_host_objects,
    kj::Maybe<uint32_t> version,
    ::rust::Vec<uint8_t>& result) {
  if (exceptionPending(isolate)) return false;
  Serializer serializer(isolate, errors_are_host_objects);
  if (!serializer.writeHeader(version)) return false;
  if (!serializer.write(local_as_ref_from_ffi<v8::Value>(value))) return false;
  result = serializer.release();
  return true;
}

MaybeLocal value_deserialize(
    Isolate* isolate, ::rust::Slice<const uint8_t> data, bool preserve_stack_in_errors) {
  if (exceptionPending(isolate)) return {};
  Deserializer deserializer(isolate, data, preserve_stack_in_errors);
  if (!deserializer.readHeader(data)) return {};
  return maybe_local_to_ffi(deserializer.read());
}

}  // namespace workerd::rust::jsg
