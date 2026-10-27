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
#include <kj/vector.h>

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

// Throws a TypeError with `message`, unless execution is terminating.
void throwTypeError(v8::Isolate* isolate, kj::StringPtr message) {
  if (isolate->IsExecutionTerminating()) return;
  auto messageStr = v8::String::NewFromUtf8(
      isolate, message.begin(), v8::NewStringType::kNormal, static_cast<int>(message.size()))
                        .ToLocalChecked();
  isolate->ThrowException(v8::Exception::TypeError(messageStr));
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

class Serializer final: public v8::ValueSerializer::Delegate {
 public:
  Serializer(v8::Isolate* isolate, const SerializeOptions& options)
      : isolate(isolate),
        options(options),
        sharedArrayBuffers(isolate),
        transferredValues(isolate),
        buffersToDetach(isolate),
        ser(isolate, this) {}
  // noexcept(true), as the Delegate's destructor is, though kj::Vector's is not.
  ~Serializer() noexcept(true) {}

  // Applies the options as jsg::Serializer's constructor does. Returns false if V8 rejected the
  // version, leaving a DataCloneError pending. Throws a KJ exception for a version older than 13.
  bool init() {
    if (options.has_version) {
      KJ_REQUIRE(
          options.version >= 13, "The minimum serialization version is 13.", options.version);
      if (ser.SetWriteVersion(options.version).IsNothing()) return false;
    }
    if (!options.omit_header) {
      ser.WriteHeader();
    }
    if (!options.class_instances_as_plain_objects) {
      prototypeOfObject = v8::Object::New(isolate)->GetPrototype();
    }
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
      throwDataCloneError(
          isolate, v8::String::NewFromUtf8Literal(isolate, "Object is not transferable."));
      return false;
    }
    // A value given twice is transferred once. Values are compared, not their buffers, as
    // jsg::Serializer compares them, so that the numbers are the same.
    for (auto transferred: transferredValues) {
      if (transferred == value) return true;
    }
    auto id = static_cast<uint32_t>(transferredValues.size());
    transferredValues.push_back(value);
    transferredBackingStores.add(buffer->GetBackingStore());
    ser.TransferArrayBuffer(id, buffer);
    // Detached once the value is written, as the structured clone algorithm does, so that a getter
    // that serialization runs sees the buffer intact, and so that V8 still finds the buffer of a
    // view written after it.
    buffersToDetach.push_back(buffer);
    return true;
  }

  // Returns false if serialization threw, leaving the exception pending.
  bool write(v8::Local<v8::Value> value) {
    return ser.WriteValue(isolate->GetCurrentContext(), value).IsJust();
  }

  // Detaches the transferred buffers and moves the data and the backing stores into `result`, as
  // jsg::Serializer::release() does. Returns false if detaching threw, leaving the exception
  // pending.
  bool release(Serialized& result) {
    for (auto buffer: buffersToDetach) {
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
    // V8 already treats objects with internal fields, such as API objects, as host objects.
    return !options.class_instances_as_plain_objects || options.errors_are_host_objects;
  }

  // Decides as jsg::Serializer::IsHostObject() does.
  v8::Maybe<bool> IsHostObject(v8::Isolate*, v8::Local<v8::Object> object) override {
    if (object->InternalFieldCount() > 0) return v8::Just(true);
    if (object->IsNativeError()) return v8::Just(options.errors_are_host_objects);
    if (options.class_instances_as_plain_objects) return v8::Just(false);
    // A class instance, whose prototype is not Object.prototype, is a host object, which
    // writeApiObject() rejects with a DataCloneError.
    return v8::Just(object->GetPrototype() != prototypeOfObject);
  }

  v8::Maybe<bool> WriteHostObject(v8::Isolate*, v8::Local<v8::Object> object) override {
    if (options.errors_are_host_objects && object->IsNativeError()) {
      return ::workerd::jsg::writeNativeError(isolate, ser, object);
    }
    return ::workerd::jsg::writeApiObject(isolate, ser, object);
  }

  // Numbers each SharedArrayBuffer and keeps its backing store, as jsg::Serializer does. The data
  // refers to the buffer by number; a deserializer given the backing stores shares it.
  v8::Maybe<uint32_t> GetSharedArrayBufferId(
      v8::Isolate*, v8::Local<v8::SharedArrayBuffer> buffer) override {
    for (auto i: kj::zeroTo(sharedArrayBuffers.size())) {
      if (sharedArrayBuffers[i] == buffer) return v8::Just(static_cast<uint32_t>(i));
    }
    sharedArrayBuffers.push_back(buffer);
    sharedBackingStores.add(buffer->GetBackingStore());
    return v8::Just(static_cast<uint32_t>(sharedArrayBuffers.size() - 1));
  }

 private:
  v8::Isolate* isolate;
  SerializeOptions options;
  // Object.prototype, if `options.class_instances_as_plain_objects` is false.
  v8::Local<v8::Value> prototypeOfObject;
  v8::LocalVector<v8::SharedArrayBuffer> sharedArrayBuffers;
  kj::Vector<std::shared_ptr<v8::BackingStore>> sharedBackingStores;
  // The values given to transfer(), the backing stores of their buffers, and the buffers.
  v8::LocalVector<v8::Value> transferredValues;
  kj::Vector<std::shared_ptr<v8::BackingStore>> transferredBackingStores;
  v8::LocalVector<v8::ArrayBuffer> buffersToDetach;
  v8::ValueSerializer ser;
};

class Deserializer final: public v8::ValueDeserializer::Delegate {
 public:
  Deserializer(v8::Isolate* isolate,
      ::rust::Slice<const uint8_t> data,
      const DeserializeOptions& options,
      ::rust::Slice<const size_t> sharedArrayBuffers)
      : isolate(isolate),
        options(options),
        sharedArrayBuffers(sharedArrayBuffers),
        deser(isolate, data.data(), data.size(), this) {}

  // Applies the options and hands over the transferred buffers as jsg::Deserializer's constructor
  // does, reading the header unless `options.read_header` is false. Throws a KJ exception, which
  // reaches JavaScript as an internal error, if the header is missing or of an unsupported
  // version, or the version is older than 13; if execution is terminating, returns false.
  bool init(::rust::Slice<const uint8_t> data,
      ::rust::Str diagnosticContext,
      ::rust::Slice<const size_t> transferredArrayBuffers) {
    if (options.read_header && !readHeader(data, diagnosticContext)) return false;
    if (options.has_version) {
      KJ_REQUIRE(
          options.version >= 13, "The minimum serialization version is 13.", options.version);
      deser.SetWireFormatVersion(options.version);
    }
    for (auto i: kj::zeroTo(transferredArrayBuffers.size())) {
      deser.TransferArrayBuffer(static_cast<uint32_t>(i),
          v8::ArrayBuffer::New(isolate, backingStoreAt(transferredArrayBuffers[i])));
    }
    return true;
  }

  // Returns none if deserialization threw, leaving the exception pending.
  v8::MaybeLocal<v8::Value> read() {
    return deser.ReadValue(isolate->GetCurrentContext());
  }

  v8::MaybeLocal<v8::Object> ReadHostObject(v8::Isolate*) override {
    uint32_t tag;
    if (!deser.ReadUint32(&tag)) return throwCorrupt();
    if (tag == ::workerd::jsg::SERIALIZATION_TAG_NATIVE_ERROR) {
      return ::workerd::jsg::readNativeError(
          isolate, deser, ::workerd::jsg::PreserveStack(options.preserve_stack_in_errors));
    }
    return ::workerd::jsg::readApiObject(isolate, deser, tag);
  }

  // A new SharedArrayBuffer over the backing store the data numbers `id`, as jsg::Deserializer
  // creates.
  v8::MaybeLocal<v8::SharedArrayBuffer> GetSharedArrayBufferFromId(
      v8::Isolate*, uint32_t id) override {
    if (!options.has_shared_array_buffers) {
      // V8 throws its "Unable to deserialize cloned data" error, as it does for
      // jsg::Deserializer.
      return v8::ValueDeserializer::Delegate::GetSharedArrayBufferFromId(isolate, id);
    }
    // The buffers are the data's own, so only corrupt data names one beyond them.
    if (id >= sharedArrayBuffers.size()) {
      throwCorrupt();
      return {};
    }
    return v8::SharedArrayBuffer::New(isolate, backingStoreAt(sharedArrayBuffers[id]));
  }

 private:
  bool readHeader(::rust::Slice<const uint8_t> data, ::rust::Str diagnosticContext) {
    v8::TryCatch tryCatch(isolate);
    if (deser.ReadHeader(isolate->GetCurrentContext()).IsJust()) return true;
    MaybeLocal exception;
    if (!catch_exception(tryCatch, exception)) return false;
    // Only header metadata, never bytes of the value.
    auto context = kj::str(diagnosticContext);
    auto header = data.empty() ? "empty" : data[0] == 0xff ? "present" : "missing";
    KJ_FAIL_REQUIRE("Unable to deserialize cloned data due to invalid or unsupported version.",
        context, data.size(), header, deser.GetWireFormatVersion(),
        v8::CurrentValueSerializerFormatVersion());
  }

  v8::MaybeLocal<v8::Object> throwCorrupt() {
    isolate_throw_internal_error(isolate, "deserialization failure, possible corruption");
    return {};
  }

  v8::Isolate* isolate;
  DeserializeOptions options;
  ::rust::Slice<const size_t> sharedArrayBuffers;
  v8::ValueDeserializer deser;
};

}  // namespace

// Serialization
bool local_serialize(Isolate* isolate,
    const Local& value,
    const SerializeOptions& options,
    ::rust::Slice<const Local> transfer,
    Serialized& result) {
  Serializer serializer(isolate, options);
  if (!serializer.init()) return false;
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
  Deserializer deserializer(isolate, data, options, shared_array_buffers);
  if (!deserializer.init(data, diagnostic_context, transferred_array_buffers)) return {};
  return maybe_local_to_ffi(deserializer.read());
}

}  // namespace workerd::rust::jsg
