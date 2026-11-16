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
// and readApiObject(). They read the header through readHeader().

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

void throwDataCloneError(v8::Isolate* isolate, kj::StringPtr message) {
  throwDataCloneError(isolate, newString(isolate, message));
}

// Throws a TypeError with `message`, as JSG_FAIL_REQUIRE(TypeError, ...) does, unless execution is
// terminating.
void throwTypeError(v8::Isolate* isolate, kj::StringPtr message) {
  if (isolate->IsExecutionTerminating()) return;
  isolate->ThrowException(v8::Exception::TypeError(newString(isolate, message)));
}

// The backing store at `address`, a heap-allocated std::shared_ptr<v8::BackingStore> that Rust's
// jsg::v8::BackingStore owns.
const std::shared_ptr<v8::BackingStore>& backingStoreAt(size_t address) {
  return *reinterpret_cast<const std::shared_ptr<v8::BackingStore>*>(address);
}

// Moves `stores` into `addresses` as heap-allocated std::shared_ptrs, which Rust takes ownership
// of.
void releaseBackingStores(
    kj::Vector<std::shared_ptr<v8::BackingStore>>& stores, ::rust::Vec<size_t>& addresses) {
  addresses.reserve(stores.size());
  for (auto& store: stores) {
    addresses.push_back(
        reinterpret_cast<size_t>(new std::shared_ptr<v8::BackingStore>(kj::mv(store))));
  }
  stores.clear();
}

// Whether V8 can write and read `version` of its format, from the oldest that jsg::Serializer
// writes to V8's latest. Throws a DataCloneError, as `what` ("Serialization" or
// "Deserialization") of that version, if not.
bool checkVersion(v8::Isolate* isolate, uint32_t version, kj::StringPtr what) {
  if (version >= MIN_SERIALIZATION_VERSION &&
      version <= v8::CurrentValueSerializerFormatVersion()) {
    return true;
  }
  throwDataCloneError(isolate, kj::str(what, " version ", version, " is not supported."));
  return false;
}

class Serializer final: public v8::ValueSerializer::Delegate {
 public:
  Serializer(v8::Isolate* isolate, const SerializeOptions& options)
      : isolate(isolate),
        errorsAreHostObjects(options.errors_are_host_objects),
        classInstancesAsPlainObjects(options.class_instances_as_plain_objects),
        prototypeOfObject(v8::Object::New(isolate)->GetPrototype()),
        jsSerializeModeKey(v8::Private::ForApi(
            isolate, newString(isolate, ::workerd::jsg::JS_SERIALIZE_MODE_KEY))),
        domExceptionBrandKey(v8::Private::ForApi(
            isolate, newString(isolate, ::workerd::jsg::DOM_EXCEPTION_BRAND_KEY))),
        ser(isolate, this),
        jsSerializables(isolate, ser) {}
  // noexcept(true), as the Delegate's destructor is, though kj::Vector's is not.
  ~Serializer() noexcept(true) {}

  // Applies the version and header options as jsg::Serializer's constructor does. Returns false,
  // leaving a DataCloneError pending, if V8 can't write the version.
  bool init(const SerializeOptions& options) {
    if (options.has_version) {
      // For a version above V8's latest, SetWriteVersion() throws a DataCloneError whose message
      // V8 can't format, which aborts the process, and jsg::Serializer KJ_ASSERTs on one below
      // the minimum. checkVersion() throws a DataCloneError naming the version instead.
      if (!checkVersion(isolate, options.version, "Serialization"_kj)) return false;
      if (ser.SetWriteVersion(options.version).IsNothing()) return false;
    }
    if (!options.omit_header) ser.WriteHeader();
    return true;
  }

  // Transfers `value`, an ArrayBuffer or a view of one, as jsg::Serializer::transfer() does: the
  // data refers to the buffer by number, and the buffer is detached once the value is written.
  // Returns false if `value` can't be transferred, leaving a TypeError or DataCloneError pending.
  bool transfer(v8::Local<v8::Value> value) {
    v8::Local<v8::ArrayBuffer> buffer;
    if (value->IsArrayBufferView()) {
      buffer = value.As<v8::ArrayBufferView>()->Buffer();
    } else if (value->IsArrayBuffer()) {
      buffer = value.As<v8::ArrayBuffer>();
    } else {
      throwTypeError(isolate, "Object is not transferable"_kj);
      return false;
    }
    // A view of a SharedArrayBuffer, which can't be detached.
    if (!buffer->IsDetachable()) {
      throwDataCloneError(isolate, "Object is not transferable."_kj);
      return false;
    }
    // A value given twice is transferred once. Values are compared, not their buffers, as
    // jsg::Serializer compares them, so that the numbers are the same.
    for (auto& transferred: transferredValues) {
      if (transferred == value) return true;
    }
    auto id = static_cast<uint32_t>(transferredValues.size());
    transferredValues.add(isolate, value);
    transferredBackingStores.add(buffer->GetBackingStore());
    ser.TransferArrayBuffer(id, buffer);
    // Detached once the value is written, as jsg::Serializer::release() detaches it, so that a
    // getter that serialization runs sees the buffer intact, and V8 still finds the buffer of a
    // view written after it.
    buffersToDetach.add(isolate, buffer);
    return true;
  }

  // Returns false if serialization threw, leaving the exception pending.
  bool write(v8::Local<v8::Value> value) {
    if (ser.WriteValue(isolate->GetCurrentContext(), value).IsNothing()) return false;
    return jsSerializables.writePendingData().IsJust();
  }

  // Detaches the transferred buffers and moves the data and the backing stores into `result`, as
  // jsg::Serializer::release() does. Returns false if detaching threw, leaving the exception
  // pending.
  bool release(Serialized& result) {
    for (auto& global: buffersToDetach) {
      auto buffer = global.Get(isolate);
      // JavaScript that serialization ran may have detached it already, as a nested
      // structuredClone() that transfers it does.
      if (buffer->IsDetachable() && !buffer->WasDetached() &&
          buffer->Detach(v8::Local<v8::Value>()).IsNothing()) {
        return false;
      }
    }
    auto [data, size] = ser.Release();
    KJ_DEFER(free(data));
    result.data.reserve(size);
    for (auto byte: kj::arrayPtr(data, size)) {
      result.data.push_back(byte);
    }
    releaseBackingStores(sharedBackingStores, result.shared_array_buffers);
    releaseBackingStores(transferredBackingStores, result.transferred_array_buffers);
    return true;
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

  // Decides as jsg::Serializer::IsHostObject() does.
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
    if (isJsSerializable || isDomException) return v8::Just(true);
    if (object->IsNativeError()) return v8::Just(errorsAreHostObjects);
    if (classInstancesAsPlainObjects) return v8::Just(false);
    // Any other object whose prototype is not Object.prototype, a class instance, is a host
    // object, which writeApiObject() rejects with a DataCloneError.
    return v8::Just(object->GetPrototype() != prototypeOfObject);
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

  // Numbers each SharedArrayBuffer and keeps its backing store, as jsg::Serializer does. The data
  // refers to the buffer by number; a deserializer given the backing stores shares it.
  v8::Maybe<uint32_t> GetSharedArrayBufferId(
      v8::Isolate*, v8::Local<v8::SharedArrayBuffer> buffer) override {
    for (auto i: kj::indices(sharedArrayBuffers)) {
      if (sharedArrayBuffers[i] == buffer) return v8::Just(static_cast<uint32_t>(i));
    }
    sharedArrayBuffers.add(isolate, buffer);
    sharedBackingStores.add(buffer->GetBackingStore());
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
  bool classInstancesAsPlainObjects;
  // Globals, since V8 hands the delegate values in handle scopes of its own, which end before
  // serialization does.
  kj::Vector<v8::Global<v8::SharedArrayBuffer>> sharedArrayBuffers;
  kj::Vector<std::shared_ptr<v8::BackingStore>> sharedBackingStores;
  // The values given to transfer(), the backing stores of their buffers, and the buffers.
  kj::Vector<v8::Global<v8::Value>> transferredValues;
  kj::Vector<std::shared_ptr<v8::BackingStore>> transferredBackingStores;
  kj::Vector<v8::Global<v8::ArrayBuffer>> buffersToDetach;
  v8::Local<v8::Value> prototypeOfObject;
  v8::Local<v8::Private> jsSerializeModeKey;
  v8::Local<v8::Private> domExceptionBrandKey;
  v8::ValueSerializer ser;
  // Writes the instances of classes implemented in JavaScript, after `ser`, which it writes to.
  ::workerd::jsg::JsSerializableWriter jsSerializables;
};

class Deserializer final: public v8::ValueDeserializer::Delegate {
 public:
  Deserializer(v8::Isolate* isolate,
      ::rust::Slice<const uint8_t> data,
      const DeserializeOptions& options,
      ::rust::Slice<const size_t> sharedArrayBuffers)
      : isolate(isolate),
        preserveStackInErrors(options.preserve_stack_in_errors),
        sharedArrayBuffers(sharedArrayBuffers),
        deser(isolate, data.data(), data.size(), this),
        jsSerializables(isolate, deser, ::workerd::jsg::PreserveStack(preserveStackInErrors)) {}
  // noexcept(true), as the Delegate's destructor is, though kj::Vector's is not.
  ~Deserializer() noexcept(true) {}

  // Applies the header and version options and hands over the transferred buffers as
  // jsg::Deserializer's constructor does: reads the header unless `options.read_header` is false,
  // then reads in `options.version` if it has one.
  // If the header is missing or of an unsupported version, throws the KJ exception that
  // jsg::Deserializer throws, labeled with `diagnosticContext`, which reaches JavaScript as an
  // internal error. Returns false, leaving a DataCloneError pending, if V8 can't read the
  // version, or if execution is terminating.
  bool init(::rust::Slice<const uint8_t> data,
      const DeserializeOptions& options,
      ::rust::Str diagnosticContext,
      ::rust::Slice<const size_t> transferredArrayBuffers) {
    if (options.read_header) {
      auto context = kj::heapString(diagnosticContext.data(), diagnosticContext.size());
      if (::workerd::jsg::readHeader(
              isolate, deser, kj::arrayPtr(data.data(), data.size()), context.asPtr())
              .IsNothing()) {
        return false;
      }
    }
    if (options.has_version) {
      if (!checkVersion(isolate, options.version, "Deserialization"_kj)) return false;
      deser.SetWireFormatVersion(options.version);
    }
    for (auto i: kj::indices(transferredArrayBuffers)) {
      deser.TransferArrayBuffer(static_cast<uint32_t>(i),
          v8::ArrayBuffer::New(isolate, backingStoreAt(transferredArrayBuffers[i])));
    }
    return true;
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

  // A new SharedArrayBuffer over the backing store the data numbers `id`, as jsg::Deserializer
  // creates. V8 reports the data as malformed if this returns nothing, as for an id that names no
  // buffer, as jsg::Deserializer's does.
  v8::MaybeLocal<v8::SharedArrayBuffer> GetSharedArrayBufferFromId(
      v8::Isolate*, uint32_t id) override {
    if (id >= sharedArrayBuffers.size()) return {};
    return v8::SharedArrayBuffer::New(isolate, backingStoreAt(sharedArrayBuffers[id]));
  }

 private:
  // Throws a DataCloneError, as jsg::Deserializer does, rather than an internal error: like V8's
  // own error for malformed data, since corrupt or crafted bytes are not a bug in the reader.
  v8::MaybeLocal<v8::Object> throwMalformed() {
    throwDataCloneError(isolate, newString(isolate, MALFORMED_DATA_MESSAGE));
    return {};
  }

  v8::Isolate* isolate;
  bool preserveStackInErrors;
  // Borrowed jsg::v8::BackingStore addresses.
  ::rust::Slice<const size_t> sharedArrayBuffers;
  v8::ValueDeserializer deser;
  // Reads the instances of classes implemented in JavaScript, after `deser`, which it reads from.
  ::workerd::jsg::JsSerializableReader jsSerializables;
};

}  // namespace

// Serialization
bool local_serialize(Isolate* isolate,
    const Local& value,
    const SerializeOptions& options,
    ::rust::Slice<const Local> transfer,
    Serialized& result) {
  if (exceptionPending(isolate)) return false;
  Serializer serializer(isolate, options);
  if (!serializer.init(options)) return false;
  for (auto& item: transfer) {
    if (!serializer.transfer(local_as_ref_from_ffi<v8::Value>(item))) return false;
  }
  return serializer.write(local_as_ref_from_ffi<v8::Value>(value)) && serializer.release(result);
}

MaybeLocal value_deserialize(Isolate* isolate,
    ::rust::Slice<const uint8_t> data,
    const DeserializeOptions& options,
    ::rust::Str diagnostic_context,
    ::rust::Slice<const size_t> transferred_array_buffers,
    ::rust::Slice<const size_t> shared_array_buffers) {
  if (exceptionPending(isolate)) return {};
  Deserializer deserializer(isolate, data, options, shared_array_buffers);
  if (!deserializer.init(data, options, diagnostic_context, transferred_array_buffers)) return {};
  return maybe_local_to_ffi(deserializer.read());
}

}  // namespace workerd::rust::jsg
