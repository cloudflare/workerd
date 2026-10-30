// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Structured serialization for Rust JSG, in the format of C++ JSG's jsg::Serializer and
// jsg::Deserializer (workerd/jsg/ser.c++), which can read what these shims write and the other
// way round. They drive V8's serializer with delegates of their own and hand workerd API objects,
// such as Blob and Headers, to C++ JSG through writeApiObject() and readApiObject().

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

// jsg::Serializer's tag for a native error written as a host object. A uint32 error tag follows:
// 0 for a name that is written out, or the 1-based index of the name in ERROR_TYPES.
constexpr uint32_t SERIALIZATION_TAG_NATIVE_ERROR = 10;
constexpr uint32_t ERROR_TAG_UNKNOWN = 0;

struct ErrorType {
  kj::StringPtr name;
  v8::Local<v8::Value> (*create)(v8::Local<v8::String> message, v8::Local<v8::Value> options);
};

// In the order of jsg::Serializer's ErrorTag, which is part of the format.
const ErrorType ERROR_TYPES[] = {
  {"Error"_kj, v8::Exception::Error},
  {"TypeError"_kj, v8::Exception::TypeError},
  {"RangeError"_kj, v8::Exception::RangeError},
  {"ReferenceError"_kj, v8::Exception::ReferenceError},
  {"SyntaxError"_kj, v8::Exception::SyntaxError},
  {"WasmCompileError"_kj, v8::Exception::WasmCompileError},
  {"WasmLinkError"_kj, v8::Exception::WasmLinkError},
  {"WasmRuntimeError"_kj, v8::Exception::WasmRuntimeError},
  {"WasmSuspendError"_kj, v8::Exception::WasmSuspendError},
  {"EvalError"_kj, v8::Exception::EvalError},
  {"URIError"_kj, v8::Exception::URIError},
  {"AggregateError"_kj, v8::Exception::AggregateError},
  {"SuppressedError"_kj, v8::Exception::SuppressedError},
};

// The tag of the error type named `name`, compared as jsg::Serializer compares it: as UTF-8, up to
// the first NUL.
uint32_t errorTagOf(v8::Isolate* isolate, v8::Local<v8::String> name) {
  v8::String::Utf8Value utf8(isolate, name);
  if (*utf8 == nullptr) return ERROR_TAG_UNKNOWN;
  kj::StringPtr str(*utf8);
  for (auto i: kj::indices(ERROR_TYPES)) {
    if (ERROR_TYPES[i].name == str) return i + 1;
  }
  return ERROR_TAG_UNKNOWN;
}

// A plain `Error` for a tag that names no error type, as jsg::Deserializer creates.
v8::Local<v8::Object> newError(uint32_t tag, v8::Local<v8::String> message) {
  auto create =
      tag >= 1 && tag <= kj::size(ERROR_TYPES) ? ERROR_TYPES[tag - 1].create : v8::Exception::Error;
  return create(message, {}).As<v8::Object>();
}

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
      return writeError(object);
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
  // Writes `error` as jsg::Serializer does: its type, its name if the type is not a known one, its
  // message, and its other own properties as a plain object. Reading the properties can run
  // JavaScript; returns nothing if it throws, leaving the exception pending.
  v8::Maybe<bool> writeError(v8::Local<v8::Object> error) {
    auto context = isolate->GetCurrentContext();
    auto nameKey = newString(isolate, "name"_kj);
    auto messageKey = newString(isolate, "message"_kj);

    ser.WriteUint32(SERIALIZATION_TAG_NATIVE_ERROR);

    v8::Local<v8::Value> name;
    v8::Local<v8::String> nameString;
    if (!error->Get(context, nameKey).ToLocal(&name) ||
        !name->ToDetailString(context).ToLocal(&nameString)) {
      return v8::Nothing<bool>();
    }
    auto tag = errorTagOf(isolate, nameString);
    ser.WriteUint32(tag);
    if (tag == ERROR_TAG_UNKNOWN && ser.WriteValue(context, name).IsNothing()) {
      return v8::Nothing<bool>();
    }

    v8::Local<v8::Value> message;
    if (!error->Get(context, messageKey).ToLocal(&message) ||
        ser.WriteValue(context, message).IsNothing()) {
      return v8::Nothing<bool>();
    }

    v8::Local<v8::Array> keys;
    if (!error
             ->GetPropertyNames(context, v8::KeyCollectionMode::kOwnOnly,
                 v8::PropertyFilter::ALL_PROPERTIES, v8::IndexFilter::kSkipIndices)
             .ToLocal(&keys)) {
      return v8::Nothing<bool>();
    }
    auto properties = v8::Object::New(isolate);
    for (uint32_t i = 0; i < keys->Length(); i++) {
      v8::Local<v8::Value> key;
      if (!keys->Get(context, i).ToLocal(&key)) return v8::Nothing<bool>();
      // The name and message are written above.
      if (key->StrictEquals(nameKey) || key->StrictEquals(messageKey)) continue;
      v8::Local<v8::Value> value;
      if (!error->Get(context, key).ToLocal(&value) ||
          properties->Set(context, key, value).IsNothing()) {
        return v8::Nothing<bool>();
      }
    }
    return ser.WriteValue(context, properties);
  }

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
    if (tag == SERIALIZATION_TAG_NATIVE_ERROR) return readError();
    return ::workerd::jsg::readApiObject(isolate, deser, tag);
  }

  // V8's GetSharedArrayBufferFromId() throws for a SharedArrayBuffer, which the data only refers
  // to by number.

 private:
  // Reads an error that Serializer::writeError() wrote, as jsg::Deserializer does by default
  // (with preserveStackInErrors). Runs no JavaScript.
  v8::MaybeLocal<v8::Object> readError() {
    auto context = isolate->GetCurrentContext();

    uint32_t tag;
    if (!deser.ReadUint32(&tag)) return throwCorrupt();
    v8::Local<v8::Value> name;
    if (tag == ERROR_TAG_UNKNOWN && !deser.ReadValue(context).ToLocal(&name)) return {};

    v8::Local<v8::Value> message;
    if (!deser.ReadValue(context).ToLocal(&message)) return {};
    if (!message->IsString()) {
      throwDataCloneError(
          isolate, newString(isolate, "Deserialization failed: error message is not a string"_kj));
      return {};
    }
    auto error = newError(tag, message.As<v8::String>());
    // The name is set as an own property, since an error's `name` is usually inherited from its
    // prototype.
    if (!name.IsEmpty() &&
        error->DefineOwnProperty(context, newString(isolate, "name"_kj), name).IsNothing()) {
      return {};
    }

    v8::Local<v8::Value> properties;
    if (!deser.ReadValue(context).ToLocal(&properties)) return {};
    if (properties->IsObject()) {
      auto object = properties.As<v8::Object>();
      v8::Local<v8::Array> keys;
      if (!object
               ->GetPropertyNames(context, v8::KeyCollectionMode::kOwnOnly,
                   v8::PropertyFilter::ALL_PROPERTIES, v8::IndexFilter::kSkipIndices)
               .ToLocal(&keys)) {
        return {};
      }
      for (uint32_t i = 0; i < keys->Length(); i++) {
        v8::Local<v8::Value> key;
        v8::Local<v8::Value> value;
        if (!keys->Get(context, i).ToLocal(&key)) return {};
        // A data property, rather than Set(), so that no setter on the error's prototype runs.
        if (!object->Get(context, key).ToLocal(&value) ||
            error->CreateDataProperty(context, key.As<v8::Name>(), value).IsNothing()) {
          return {};
        }
      }
    }
    return error;
  }

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
