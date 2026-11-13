// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Structured serialization for Rust JSG, in the format of C++ JSG's jsg::Serializer and
// jsg::Deserializer (workerd/jsg/ser.c++), which can read what these shims write and the other
// way round. They drive V8's serializer with delegates of their own, which write native errors and
// instances of the runtime's classes implemented in JavaScript themselves, and hand workerd API
// objects, such as Blob and Headers, and DOMExceptions, however they are implemented, to C++ JSG
// through writeApiObject() and readApiObject().

#include "ffi.h"

#include <workerd/jsg/dom-exception.h>
#include <workerd/jsg/ser.h>
#include <workerd/jsg/setup.h>
#include <workerd/rust/jsg/ffi-inl.h>
#include <workerd/rust/jsg/v8.rs.h>

#include <v8-value-serializer-version.h>
#include <v8-value-serializer.h>

#include <kj/debug.h>
#include <kj/vector.h>

#include <cstdlib>

namespace workerd::rust::jsg {
namespace {

using ::workerd::jsg::JS_SERIALIZABLE_HAS_MESSAGE;
using ::workerd::jsg::JS_SERIALIZABLE_HAS_STACK;
using ::workerd::jsg::JS_SERIALIZABLE_KEEP_STACK;
using ::workerd::jsg::JS_SERIALIZABLE_KNOWN_FLAGS;
using ::workerd::jsg::SERIALIZATION_TAG_JS_SERIALIZABLE;

// jsg::Serializer's tag for a native error written as a host object. A uint32 error tag follows:
// 0 for a name that is written out, or the 1-based index of the name in ERROR_TYPES.
constexpr uint32_t SERIALIZATION_TAG_NATIVE_ERROR = 10;
constexpr uint32_t ERROR_TAG_UNKNOWN = 0;
// The oldest version of V8's format that jsg::Serializer writes.
constexpr uint32_t MIN_SERIALIZATION_VERSION = 13;
// The message of the DataCloneError that jsg::Deserializer throws when a host object's data ends
// too soon.
constexpr kj::StringPtr MALFORMED_DATA_MESSAGE =
    "Deserialization failed: the data is malformed."_kj;

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

void throwDataCloneError(v8::Isolate* isolate, kj::StringPtr message) {
  throwDataCloneError(isolate, newString(isolate, message));
}

// Throws the DataCloneError that jsg::Serializer throws for an object that it cannot serialize.
void throwDataCloneErrorForObject(v8::Isolate* isolate, v8::Local<v8::Object> object) {
  auto message = v8::String::Concat(isolate,
      newString(isolate, "Could not serialize object of type \""_kj), object->GetConstructorName());
  message = v8::String::Concat(
      isolate, message, newString(isolate, "\". This type does not support serialization."_kj));
  throwDataCloneError(isolate, message);
}

// `string` as UTF-8, converted as jsg::JsString::toString() converts it.
kj::String toKjString(v8::Isolate* isolate, v8::Local<v8::String> string) {
  auto buffer = kj::heapArray<char>(string->Utf8Length(isolate) + 1);
  // The count that WriteUtf8 returns includes the terminator.
  auto written = string->WriteUtf8(
      isolate, buffer.begin(), buffer.size(), v8::String::WriteFlags::kNullTerminate);
  KJ_ASSERT(written == buffer.size(), "WriteUtf8 must completely overwrite the backing buffer");
  return kj::String(kj::mv(buffer));
}

// An own data property of an object, as getOwnDataProperty() reads it.
struct OwnDataProperty {
  // The property's descriptor, a new plain object.
  v8::Local<v8::Object> descriptor;
  v8::Local<v8::Value> value;
};

// Reads `object`'s own data property `name` as jsg::Serializer and jsg::Deserializer read those of
// the exports, constructors and descriptors of classes implemented in JavaScript: kj::none if it
// has none, or has an accessor instead, or `object` is a workerd API object, whose interceptors and
// native accessors may run code. Runs no JavaScript: it calls no Proxy trap or JavaScript accessor,
// and does not read the prototype chain. Returns nothing if V8 throws, leaving the exception
// pending.
v8::Maybe<kj::Maybe<OwnDataProperty>> getOwnDataProperty(
    v8::Isolate* isolate, v8::Local<v8::Object> object, kj::StringPtr name) {
  using Result = kj::Maybe<OwnDataProperty>;
  if (object->InternalFieldCount() > 0) return v8::Just<Result>(kj::none);
  auto context = isolate->GetCurrentContext();
  auto key = newString(isolate, name);
  // False for a Proxy, so the descriptor below is not a Proxy trap's.
  bool found;
  if (!object->HasRealNamedProperty(context, key).To(&found)) return v8::Nothing<Result>();
  if (!found) return v8::Just<Result>(kj::none);
  // A native accessor, such as a function's `prototype`, is described as a data property.
  v8::Local<v8::Value> descriptor;
  if (!object->GetOwnPropertyDescriptor(context, key).ToLocal(&descriptor)) {
    return v8::Nothing<Result>();
  }
  if (!descriptor->IsObject()) return v8::Just<Result>(kj::none);
  auto descriptorObject = descriptor.As<v8::Object>();
  auto valueKey = newString(isolate, "value"_kj);
  bool isData;
  if (!descriptorObject->HasOwnProperty(context, valueKey).To(&isData)) {
    return v8::Nothing<Result>();
  }
  if (!isData) return v8::Just<Result>(kj::none);
  v8::Local<v8::Value> value;
  if (!descriptorObject->GetRealNamedProperty(context, valueKey).ToLocal(&value)) {
    return v8::Nothing<Result>();
  }
  return v8::Just<Result>(OwnDataProperty{.descriptor = descriptorObject, .value = value});
}

// Sets `value` to `object`'s own data property `name`, read as getOwnDataProperty() reads it, or
// to an empty handle if there is none. Returns false if V8 throws, leaving the exception pending.
bool getOwnProperty(v8::Isolate* isolate,
    v8::Local<v8::Object> object,
    kj::StringPtr name,
    v8::Local<v8::Value>& value) {
  value = {};
  kj::Maybe<OwnDataProperty> property;
  if (!getOwnDataProperty(isolate, object, name).To(&property)) return false;
  KJ_IF_SOME(p, property) {
    value = p.value;
  }
  return true;
}

// As getOwnProperty(), but leaves `value` empty too if the property is writable or configurable,
// unlike a frozen object's, so that its value could change.
bool getOwnConstant(v8::Isolate* isolate,
    v8::Local<v8::Object> object,
    kj::StringPtr name,
    v8::Local<v8::Value>& value) {
  value = {};
  kj::Maybe<OwnDataProperty> maybeProperty;
  if (!getOwnDataProperty(isolate, object, name).To(&maybeProperty)) return false;
  auto& property = KJ_UNWRAP_OR(maybeProperty, return true);
  auto context = isolate->GetCurrentContext();
  // The descriptor is a new plain object, whose own properties these are.
  for (auto flag: {"writable"_kj, "configurable"_kj}) {
    v8::Local<v8::Value> isSet;
    if (!property.descriptor->GetRealNamedProperty(context, newString(isolate, flag))
             .ToLocal(&isSet)) {
      return false;
    }
    if (isSet->IsTrue()) return true;
  }
  value = property.value;
  return true;
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
        ser(isolate, this) {}
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
    auto context = isolate->GetCurrentContext();
    if (ser.WriteValue(context, value).IsNothing()) return false;
    // As jsg::Serializer writes it, the value is followed by the data of the instances of classes
    // implemented in JavaScript that it contains, in the order they were written. Writing one's
    // data may add more.
    for (size_t i = 0; i < pendingJsSerializableData.size(); i++) {
      v8::HandleScope scope(isolate);
      if (ser.WriteValue(context, pendingJsSerializableData[i].Get(isolate)).IsNothing()) {
        return false;
      }
    }
    return true;
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
    if (!mark->IsUndefined()) return writeJsSerializable(object, mark);
    if (errorsAreHostObjects && object->IsNativeError()) return writeError(object);
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

  // Writes `object`, an instance of a class implemented in JavaScript whose mark is `mark`, as
  // jsg::Serializer does (SERIALIZATION_TAG_JS_SERIALIZABLE in workerd/jsg/ser.h), and queues the
  // data that its class's `clone` returns for it to follow the top-level value. Calling `clone`
  // runs JavaScript, as can reading an error's `message` and `stack`. Returns nothing if one
  // throws, or if the mark describes no class (a DataCloneError), leaving the exception pending.
  v8::Maybe<bool> writeJsSerializable(v8::Local<v8::Object> object, v8::Local<v8::Value> mark) {
    auto context = isolate->GetCurrentContext();
    JsSerializableClass* cls;
    if (!getJsSerializableClass(object, mark).To(&cls)) return v8::Nothing<bool>();
    auto clone = cls->clone.Get(isolate);
    // Stays valid when jsSerializableClasses grows, which moves the string but not its text.
    kj::StringPtr deserializeInfo = cls->deserializeInfo;

    v8::Local<v8::Value> arg = object;
    v8::Local<v8::Value> data;
    if (!clone->Call(context, v8::Undefined(isolate), 1, &arg).ToLocal(&data)) {
      return v8::Nothing<bool>();
    }
    ErrorFields error;
    if (!getErrorFields(object).To(&error)) return v8::Nothing<bool>();

    ser.WriteUint32(SERIALIZATION_TAG_JS_SERIALIZABLE);
    ser.WriteUint32(static_cast<uint32_t>(deserializeInfo.size()));
    ser.WriteRawBytes(deserializeInfo.begin(), deserializeInfo.size());
    ser.WriteUint32(error.flags);
    if (!error.message.IsEmpty() && ser.WriteValue(context, error.message).IsNothing()) {
      return v8::Nothing<bool>();
    }
    if (!error.stack.IsEmpty() && ser.WriteValue(context, error.stack).IsNothing()) {
      return v8::Nothing<bool>();
    }
    pendingJsSerializableData.add(isolate, data);
    return v8::Just(true);
  }

  // A class implemented in JavaScript, as its descriptor describes it.
  struct JsSerializableClass {
    v8::Global<v8::Object> descriptor;
    v8::Global<v8::Function> clone;
    kj::String deserializeInfo;
  };

  // Returns the class that `mark`, the mark of `object`, describes, as jsg::Serializer finds it, or
  // throws DataCloneError for `object` if it describes none. A descriptor's properties can't
  // change, so each is read once. Runs no JavaScript. Returns nothing if it throws, leaving the
  // exception pending.
  v8::Maybe<JsSerializableClass*> getJsSerializableClass(
      v8::Local<v8::Object> object, v8::Local<v8::Value> mark) {
    using Result = JsSerializableClass*;
    // The mark of a cloneable instance is its class's descriptor.
    if (!mark->IsObject()) {
      throwDataCloneErrorForObject(isolate, object);
      return v8::Nothing<Result>();
    }
    auto descriptor = mark.As<v8::Object>();
    for (auto& cls: jsSerializableClasses) {
      if (cls.descriptor == descriptor) return v8::Just(&cls);
    }

    v8::Local<v8::Value> clone;
    v8::Local<v8::Value> deserializeInfo;
    if (!getOwnConstant(isolate, descriptor, "clone"_kj, clone) ||
        !getOwnConstant(isolate, descriptor, "deserializeInfo"_kj, deserializeInfo)) {
      return v8::Nothing<Result>();
    }
    if (clone.IsEmpty() || !clone->IsFunction() || deserializeInfo.IsEmpty() ||
        !deserializeInfo->IsString()) {
      throwDataCloneErrorForObject(isolate, object);
      return v8::Nothing<Result>();
    }
    return v8::Just(&jsSerializableClasses.add(JsSerializableClass{
      .descriptor = v8::Global<v8::Object>(isolate, descriptor),
      .clone = v8::Global<v8::Function>(isolate, clone.As<v8::Function>()),
      .deserializeInfo = toKjString(isolate, deserializeInfo.As<v8::String>()),
    }));
  }

  // What writeJsSerializable() writes of an instance's error fields: its flags, and the `message`
  // and `stack` that they say follow.
  struct ErrorFields {
    uint32_t flags = 0;
    v8::Local<v8::Value> message;
    v8::Local<v8::Value> stack;
  };

  // Reads the error fields of `object` as jsg::Serializer does. They are those of an error as
  // Error.isError() reports it, a native error or an instance of the bootstrap's DOMException: its
  // `message` and its own `stack`, each if it is a string. Anything else could refer back to the
  // instance, which a reader cannot resolve while it is still reading the instance's record. Reading
  // them can run JavaScript. Returns nothing if it throws, leaving the exception pending.
  v8::Maybe<ErrorFields> getErrorFields(v8::Local<v8::Object> object) {
    auto context = isolate->GetCurrentContext();
    ErrorFields result;
    if (!object->IsNativeError()) {
      bool isDomException;
      if (!object->HasPrivate(context, domExceptionBrandKey).To(&isDomException)) {
        return v8::Nothing<ErrorFields>();
      }
      if (!isDomException) return v8::Just(result);
    }

    v8::Local<v8::Value> message;
    if (!object->Get(context, newString(isolate, "message"_kj)).ToLocal(&message)) {
      return v8::Nothing<ErrorFields>();
    }
    if (message->IsString()) {
      result.flags |= JS_SERIALIZABLE_HAS_MESSAGE;
      result.message = message;
    }

    auto stackKey = newString(isolate, "stack"_kj);
    bool hasStack;
    if (!object->HasOwnProperty(context, stackKey).To(&hasStack)) {
      return v8::Nothing<ErrorFields>();
    }
    if (!hasStack) return v8::Just(result);
    v8::Local<v8::Value> stack;
    if (!object->Get(context, stackKey).ToLocal(&stack)) return v8::Nothing<ErrorFields>();
    if (stack->IsString()) {
      result.flags |= JS_SERIALIZABLE_HAS_STACK;
      // Without enhanced error serialization, a native error is written in V8's own format, which
      // keeps its stack whatever the reader's options, so this one keeps its stack too.
      if (!errorsAreHostObjects) result.flags |= JS_SERIALIZABLE_KEEP_STACK;
      result.stack = stack;
    }
    return v8::Just(result);
  }

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
  // The data of the instances of classes implemented in JavaScript that write() writes after the
  // value.
  kj::Vector<v8::Global<v8::Value>> pendingJsSerializableData;
  // The classes that getJsSerializableClass() has found.
  kj::Vector<JsSerializableClass> jsSerializableClasses;
  v8::Local<v8::Value> prototypeOfObject;
  v8::Local<v8::Private> jsSerializeModeKey;
  v8::Local<v8::Private> domExceptionBrandKey;
  v8::ValueSerializer ser;
};

class Deserializer final: public v8::ValueDeserializer::Delegate {
 public:
  Deserializer(v8::Isolate* isolate, ::rust::Slice<const uint8_t> data, bool preserveStackInErrors)
      : isolate(isolate),
        preserveStackInErrors(preserveStackInErrors),
        deser(isolate, data.data(), data.size(), this) {}
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
    if (!pendingJsSerializables.empty() && !finishJsSerializables()) return {};
    return value;
  }

  v8::MaybeLocal<v8::Object> ReadHostObject(v8::Isolate*) override {
    uint32_t tag;
    if (!deser.ReadUint32(&tag)) return throwMalformed();
    if (tag == SERIALIZATION_TAG_NATIVE_ERROR) return readError();
    if (tag == SERIALIZATION_TAG_JS_SERIALIZABLE) return readJsSerializable();
    return ::workerd::jsg::readApiObject(isolate, deser, tag);
  }

  // V8's GetSharedArrayBufferFromId() throws for a SharedArrayBuffer, which the data only refers
  // to by number.

 private:
  // Reads an error that Serializer::writeError() wrote, as jsg::Deserializer does, restoring its
  // `stack` only if preserveStackInErrors is set. Runs no JavaScript.
  v8::MaybeLocal<v8::Object> readError() {
    auto context = isolate->GetCurrentContext();

    uint32_t tag;
    if (!deser.ReadUint32(&tag)) return throwMalformed();
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
        if (!preserveStackInErrors && key->StrictEquals(newString(isolate, "stack"_kj))) continue;
        // A data property, rather than Set(), so that no setter on the error's prototype runs.
        if (!object->Get(context, key).ToLocal(&value) ||
            error->CreateDataProperty(context, key.As<v8::Name>(), value).IsNothing()) {
          return {};
        }
      }
    }
    return error;
  }

  // Reads an instance of a class implemented in JavaScript, as jsg::Deserializer does: makes it
  // from its class's prototype at once, without calling the constructor, for
  // finishJsSerializables() to initialize. Runs no JavaScript. The data may come from an untrusted
  // source, so whatever it names is checked to be a class that opted into the protocol.
  v8::MaybeLocal<v8::Object> readJsSerializable() {
    uint64_t size;
    const void* bytes;
    uint32_t flags;
    if (!deser.ReadUint64(&size) || !deser.ReadRawBytes(size, &bytes) ||
        !deser.ReadUint32(&flags)) {
      return throwMalformed();
    }
    auto deserializeInfo = kj::str(kj::arrayPtr(static_cast<const char*>(bytes), size));
    if ((flags & ~JS_SERIALIZABLE_KNOWN_FLAGS) != 0) {
      throwDataCloneError(
          isolate, "Deserialization failed: unsupported object implemented in JavaScript."_kj);
      return {};
    }
    v8::Local<v8::String> message;
    v8::Local<v8::String> stack;
    if ((flags & JS_SERIALIZABLE_HAS_MESSAGE) && !readErrorField("message"_kj, message)) return {};
    if ((flags & JS_SERIALIZABLE_HAS_STACK) && !readErrorField("stack"_kj, stack)) return {};

    size_t classIndex;
    if (!findJsSerializableClass(kj::mv(deserializeInfo)).To(&classIndex)) return {};
    // As for a native error, the instance keeps the writer's stack only if the reader keeps
    // errors' stacks or the writer was not using enhanced error serialization.
    if (!preserveStackInErrors && !(flags & JS_SERIALIZABLE_KEEP_STACK)) stack = {};
    auto& cls = jsSerializableClasses[classIndex];
    v8::Local<v8::Object> instance;
    if (!newInstance(cls, message, stack).ToLocal(&instance)) return {};
    // It was cloneable when written, and so is the copy.
    auto modeKey =
        v8::Private::ForApi(isolate, newString(isolate, ::workerd::jsg::JS_SERIALIZE_MODE_KEY));
    if (instance->SetPrivate(isolate->GetCurrentContext(), modeKey, cls.descriptor.Get(isolate))
            .IsNothing()) {
      return {};
    }
    pendingJsSerializables.add(PendingJsSerializable{
      .instance = v8::Global<v8::Object>(isolate, instance),
      .classIndex = classIndex,
    });
    return instance;
  }

  // Reads the `message` or `stack`, named by `field`, of an instance of a class implemented in
  // JavaScript into `string`. Returns false if reading it throws, or it is not a string (a
  // DataCloneError), leaving the exception pending.
  bool readErrorField(kj::StringPtr field, v8::Local<v8::String>& string) {
    v8::Local<v8::Value> value;
    if (!deser.ReadValue(isolate->GetCurrentContext()).ToLocal(&value)) return false;
    if (!value->IsString()) {
      throwDataCloneError(isolate,
          kj::str("Deserialization failed: ", field,
              " of an object implemented in JavaScript is not a string."));
      return false;
    }
    string = value.As<v8::String>();
    return true;
  }

  // A class implemented in JavaScript that findJsSerializableClass() found.
  struct JsSerializableClass {
    kj::String deserializeInfo;
    v8::Global<v8::Object> prototype;
    v8::Global<v8::Object> descriptor;
    v8::Global<v8::Function> deserialize;
    // What readJsSerializable() makes the class's instances as, which the prototype decides.
    enum class Kind : uint8_t {
      OBJECT,
      // The prototype inherits from Error.prototype.
      NATIVE_ERROR,
      // The prototype inherits from the bootstrap's DOMException's.
      DOM_EXCEPTION,
    };
    Kind kind;
  };

  // Returns the index in jsSerializableClasses of the class that `deserializeInfo` names, or throws
  // DataCloneError if it names none. Runs no JavaScript. Returns nothing if it throws, leaving the
  // exception pending.
  v8::Maybe<size_t> findJsSerializableClass(kj::String deserializeInfo) {
    for (auto i: kj::indices(jsSerializableClasses)) {
      if (jsSerializableClasses[i].deserializeInfo == deserializeInfo) return v8::Just(i);
    }
    kj::Maybe<JsSerializableClass> found;
    if (!lookUpJsSerializableClass(deserializeInfo).MoveTo(&found)) return v8::Nothing<size_t>();
    auto& cls = KJ_UNWRAP_OR(found, {
      throwDataCloneError(
          isolate, "Deserialization failed: unknown class implemented in JavaScript."_kj);
      return v8::Nothing<size_t>();
    });
    cls.deserializeInfo = kj::mv(deserializeInfo);
    jsSerializableClasses.add(kj::mv(cls));
    return v8::Just(jsSerializableClasses.size() - 1);
  }

  // Looks up the class that `deserializeInfo` names, "module:Export", as jsg::Deserializer does:
  // the export of a loaded bootstrap module, whose `prototype` holds a descriptor with a
  // `deserialize` function. Only own data properties of ordinary objects count. Gives kj::none for
  // anything else. Runs no JavaScript. Returns nothing if V8 throws, leaving the exception pending.
  v8::Maybe<kj::Maybe<JsSerializableClass>> lookUpJsSerializableClass(
      kj::StringPtr deserializeInfo) {
    using Result = kj::Maybe<JsSerializableClass>;
    auto colon = KJ_UNWRAP_OR(deserializeInfo.findFirst(':'), return v8::Just<Result>(kj::none));
    auto exports = KJ_UNWRAP_OR(
        ::workerd::jsg::tryGetBootstrapExports(isolate, kj::str(deserializeInfo.first(colon))),
        return v8::Just<Result>(kj::none));
    if (!exports->IsObject()) return v8::Just<Result>(kj::none);
    v8::Local<v8::Value> constructor;
    if (!getOwnProperty(
            isolate, exports.As<v8::Object>(), deserializeInfo.slice(colon + 1), constructor)) {
      return v8::Nothing<Result>();
    }
    if (constructor.IsEmpty() || !constructor->IsFunction()) return v8::Just<Result>(kj::none);
    v8::Local<v8::Value> prototype;
    if (!getOwnProperty(isolate, constructor.As<v8::Object>(), "prototype"_kj, prototype)) {
      return v8::Nothing<Result>();
    }
    if (prototype.IsEmpty() || !prototype->IsObject()) return v8::Just<Result>(kj::none);

    // Undefined if the class did not opt in.
    auto context = isolate->GetCurrentContext();
    auto cloneableKey =
        v8::Private::ForApi(isolate, newString(isolate, ::workerd::jsg::JS_CLONEABLE_KEY));
    v8::Local<v8::Value> descriptor;
    if (!prototype.As<v8::Object>()->GetPrivate(context, cloneableKey).ToLocal(&descriptor)) {
      return v8::Nothing<Result>();
    }
    if (!descriptor->IsObject()) return v8::Just<Result>(kj::none);
    v8::Local<v8::Value> deserialize;
    if (!getOwnProperty(isolate, descriptor.As<v8::Object>(), "deserialize"_kj, deserialize)) {
      return v8::Nothing<Result>();
    }
    if (deserialize.IsEmpty() || !deserialize->IsFunction()) return v8::Just<Result>(kj::none);

    JsSerializableClass::Kind kind;
    if (!kindOf(prototype.As<v8::Object>()).To(&kind)) return v8::Nothing<Result>();
    return v8::Just<Result>(JsSerializableClass{
      .prototype = v8::Global<v8::Object>(isolate, prototype.As<v8::Object>()),
      .descriptor = v8::Global<v8::Object>(isolate, descriptor.As<v8::Object>()),
      .deserialize = v8::Global<v8::Function>(isolate, deserialize.As<v8::Function>()),
      .kind = kind,
    });
  }

  // What readJsSerializable() makes the instances of a class whose prototype is `prototype` as,
  // as jsg::Deserializer decides. Runs no JavaScript: V8 calls no Proxy trap to read a private
  // property, and a Proxy ends the prototype chain. Returns nothing if V8 throws, leaving the
  // exception pending.
  v8::Maybe<JsSerializableClass::Kind> kindOf(v8::Local<v8::Object> prototype) {
    using Kind = JsSerializableClass::Kind;
    auto context = isolate->GetCurrentContext();
    v8::Local<v8::Value> errorPrototype;
    if (!::workerd::jsg::IsolateBase::getErrorPrototype(isolate, context)
             .ToLocal(&errorPrototype)) {
      return v8::Nothing<Kind>();
    }
    auto domExceptionPrototypeKey = v8::Private::ForApi(
        isolate, newString(isolate, ::workerd::jsg::DOM_EXCEPTION_PROTOTYPE_KEY));
    // The bootstrap's DOMException.prototype inherits from Error.prototype, so it is found first.
    for (v8::Local<v8::Value> current = prototype; current->IsObject();
         current = current.As<v8::Object>()->GetPrototype()) {
      bool isDomExceptionPrototype;
      if (!current.As<v8::Object>()
               ->HasPrivate(context, domExceptionPrototypeKey)
               .To(&isDomExceptionPrototype)) {
        return v8::Nothing<Kind>();
      }
      if (isDomExceptionPrototype) return v8::Just(Kind::DOM_EXCEPTION);
      if (current == errorPrototype) return v8::Just(Kind::NATIVE_ERROR);
    }
    return v8::Just(Kind::OBJECT);
  }

  // What calling the constructor of `cls` would make, minus whatever the constructor does, which
  // the class's `deserialize` does: an instance of the bootstrap's DOMException's base or a native
  // error, as the kind of the class says, or else an object, whose prototype is the class's. An
  // error's `stack` is `stack` if it is given, or else the one V8 captures now, and either way not
  // enumerable. Runs no JavaScript. Returns nothing if V8 throws, leaving the exception pending.
  v8::MaybeLocal<v8::Object> newInstance(
      const JsSerializableClass& cls, v8::Local<v8::String> message, v8::Local<v8::String> stack) {
    using Kind = JsSerializableClass::Kind;
    auto context = isolate->GetCurrentContext();
    auto prototype = cls.prototype.Get(isolate);
    v8::Local<v8::Object> instance;
    switch (cls.kind) {
      case Kind::DOM_EXCEPTION: {
        // As jsg::newUninitializedBootstrapDomException() makes one. V8 formats its stack's header
        // lazily, from the name and message that `deserialize` sets.
        auto tmpl = ::workerd::jsg::IsolateBase::getDomExceptionTemplate(isolate);
        if (!tmpl->InstanceTemplate()->NewInstance(context).ToLocal(&instance)) return {};
        break;
      }
      case Kind::NATIVE_ERROR:
        // The message is what the stack captured now begins with, after the error's name, as for a
        // native error.
        instance = v8::Exception::Error(message.IsEmpty() ? v8::String::Empty(isolate) : message)
                       .As<v8::Object>();
        // Error() always gives the error an own `message`, which a class's instance does not have
        // until it is initialized.
        if (instance->Delete(context, newString(isolate, "message"_kj)).IsNothing()) return {};
        break;
      case Kind::OBJECT:
        // v8::Object::New() given a prototype makes an object in dictionary mode, slow to use. One
        // given a new prototype, as by Object.setPrototypeOf(), keeps fast properties.
        instance = v8::Object::New(isolate);
        break;
    }
    if (instance->SetPrototype(context, prototype).IsNothing()) return {};
    if (cls.kind == Kind::OBJECT) return instance;
    if (!stack.IsEmpty()) {
      if (instance->DefineOwnProperty(context, newString(isolate, "stack"_kj), stack, v8::DontEnum)
              .IsNothing()) {
        return {};
      }
    } else if (cls.kind == Kind::DOM_EXCEPTION &&
        v8::Exception::CaptureStackTrace(context, instance).IsNothing()) {
      return {};
    }
    return instance;
  }

  // Reads the data of the instances that readJsSerializable() made, which follows the top-level
  // value in the order they were read, then initializes them with their classes' `deserialize`
  // functions, in the same order, as jsg::Deserializer does. Reading one's data may read more
  // instances. Returns false if either throws, leaving the exception pending.
  bool finishJsSerializables() {
    auto context = isolate->GetCurrentContext();
    kj::Vector<v8::Global<v8::Value>> data(pendingJsSerializables.size());
    for (size_t i = 0; i < pendingJsSerializables.size(); i++) {
      v8::HandleScope scope(isolate);
      v8::Local<v8::Value> value;
      if (!deser.ReadValue(context).ToLocal(&value)) return false;
      data.add(isolate, value);
    }

    // Initializing the instances runs JavaScript, which a caller may have disallowed.
    if (::workerd::jsg::IsolateBase::from(isolate).getDisallowJavascriptExecution()) {
      throwDataCloneError(
          isolate, "Objects implemented in JavaScript cannot be deserialized here."_kj);
      return false;
    }
    auto pending = kj::mv(pendingJsSerializables);
    for (auto i: kj::indices(pending)) {
      v8::HandleScope scope(isolate);
      auto deserialize = jsSerializableClasses[pending[i].classIndex].deserialize.Get(isolate);
      v8::Local<v8::Value> args[] = {pending[i].instance.Get(isolate), data[i].Get(isolate)};
      if (deserialize->Call(context, v8::Undefined(isolate), kj::size(args), args).IsEmpty()) {
        return false;
      }
    }
    return true;
  }

  // Throws a DataCloneError, as jsg::Deserializer does, rather than an internal error: like V8's
  // own error for malformed data, since corrupt or crafted bytes are not a bug in the reader.
  v8::MaybeLocal<v8::Object> throwMalformed() {
    throwDataCloneError(isolate, newString(isolate, MALFORMED_DATA_MESSAGE));
    return {};
  }

  struct PendingJsSerializable {
    v8::Global<v8::Object> instance;
    // The instance's class in jsSerializableClasses.
    size_t classIndex;
  };

  v8::Isolate* isolate;
  bool preserveStackInErrors;
  // The classes that findJsSerializableClass() has found. No JavaScript runs while V8 reads the
  // value, so none of them can change before it ends.
  kj::Vector<JsSerializableClass> jsSerializableClasses;
  // The instances of classes implemented in JavaScript that the value contains, in the order they
  // were read, which read() initializes once it has read the value. Globals, since V8 hands the
  // delegate values in handle scopes of its own, which end before deserialization does.
  kj::Vector<PendingJsSerializable> pendingJsSerializables;
  v8::ValueDeserializer deser;
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
