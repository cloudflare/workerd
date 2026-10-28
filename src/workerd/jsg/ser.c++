// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "ser.h"

#include "dom-exception.h"
#include "setup.h"

#include <workerd/util/sentry.h>

#include <v8-proxy.h>
#include <v8-value-serializer-version.h>

namespace workerd::jsg {
namespace {

// The error tag is serialized as a uint32_t immediately following SERIALIZATION_TAG_NATIVE_ERROR,
// so that the deserializer can construct the appropriate v8::Exception type without comparing
// the error name. It is 0 (ERROR_TAG_UNKNOWN) for an error whose name is not one of
// NATIVE_ERROR_TYPES; that name is then written out, and the error is deserialized as a generic
// Error with the name set on it. Otherwise it is the 1-based index of the name in
// NATIVE_ERROR_TYPES.
constexpr uint32_t ERROR_TAG_UNKNOWN = 0;

struct NativeErrorType {
  kj::StringPtr name;
  v8::Local<v8::Value> (*create)(v8::Local<v8::String> message, v8::Local<v8::Value> options);
};

// The order is part of the format: never reorder, insert or remove entries; only append.
const NativeErrorType NATIVE_ERROR_TYPES[] = {
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

// The tag of the error type named `name`, compared as UTF-8 up to the first NUL.
uint32_t errorTagOf(v8::Isolate* isolate, v8::Local<v8::String> name) {
  v8::String::Utf8Value utf8(isolate, name);
  if (*utf8 == nullptr) return ERROR_TAG_UNKNOWN;
  kj::StringPtr str(*utf8);
  for (auto i: kj::indices(NATIVE_ERROR_TYPES)) {
    if (NATIVE_ERROR_TYPES[i].name == str) return i + 1;
  }
  return ERROR_TAG_UNKNOWN;
}

// A new error of the type `tag` names, or a generic Error for a tag that names no type.
v8::Local<v8::Object> newNativeError(uint32_t tag, v8::Local<v8::String> message) {
  auto create = tag >= 1 && tag <= kj::size(NATIVE_ERROR_TYPES) ? NATIVE_ERROR_TYPES[tag - 1].create
                                                                : v8::Exception::Error;
  return create(message, {}).As<v8::Object>();
}

v8::Local<v8::String> newInternalizedString(v8::Isolate* isolate, kj::StringPtr str) {
  return v8::String::NewFromUtf8(
      isolate, str.begin(), v8::NewStringType::kInternalized, static_cast<int>(str.size()))
      .ToLocalChecked();
}

// Throws what Serializer's ThrowDataCloneError() throws, unless execution is terminating.
void throwDataCloneError(v8::Isolate* isolate, kj::StringPtr message) {
  if (isolate->IsExecutionTerminating()) return;
  auto messageStr = v8::String::NewFromUtf8(
      isolate, message.begin(), v8::NewStringType::kNormal, static_cast<int>(message.size()))
                        .ToLocalChecked();
  v8::Local<v8::Value> exception;
  if (newDataCloneError(isolate, messageStr).ToLocal(&exception)) {
    isolate->ThrowException(exception);
  }
}

// Throws the internal error that a failed read of the format throws, unless execution is
// terminating.
void throwCorrupt(v8::Isolate* isolate) {
  if (isolate->IsExecutionTerminating()) return;
  throwInternalError(isolate, "deserialization failure, possible corruption"_kj);
}

}  // namespace

void Serializer::ExternalHandler::serializeFunction(
    jsg::Lock& js, jsg::Serializer& serializer, v8::Local<v8::Function> func) {
  JSG_FAIL_REQUIRE(DOMDataCloneError, func, " could not be cloned.");
}

void Serializer::ExternalHandler::serializeProxy(
    jsg::Lock& js, jsg::Serializer& serializer, v8::Local<v8::Proxy> proxy) {
  JSG_FAIL_REQUIRE(DOMDataCloneError, proxy, " could not be cloned.");
}

Serializer::Serializer(Lock& js, Options options)
    : externalHandler(options.externalHandler),
      treatClassInstancesAsPlainObjects(options.treatClassInstancesAsPlainObjects),
      treatErrorsAsHostObjects(js.isUsingEnhancedErrorSerialization()),
      ser(ownedSer.emplace(js.v8Isolate, static_cast<v8::ValueSerializer::Delegate*>(this))) {
#ifdef KJ_DEBUG
  kj::requireOnStack(this, "jsg::Serializer must be allocated on the stack");
#endif
  if (!treatClassInstancesAsPlainObjects) {
    prototypeOfObject = js.obj().getPrototype(js);
  }
  if (externalHandler != kj::none) {
    // If we have an ExternalHandler, we'll ask it to serialize host objects.
    ser.SetTreatFunctionsAsHostObjects(true);
    ser.SetTreatProxiesAsHostObjects(true);
  }
  KJ_IF_SOME(version, options.version) {
    KJ_ASSERT(version >= 13, "The minimum serialization version is 13.");
    KJ_ASSERT(jsg::check(ser.SetWriteVersion(version)));
  }
  if (!options.omitHeader) {
    ser.WriteHeader();
  }
}

Serializer::Serializer(Lock& js, v8::ValueSerializer& ser, Borrowed)
    : treatClassInstancesAsPlainObjects(true),
      ser(ser) {}

v8::Maybe<uint32_t> Serializer::GetSharedArrayBufferId(
    v8::Isolate* isolate, v8::Local<v8::SharedArrayBuffer> sab) {
  auto& js = jsg::Lock::from(isolate);
  uint32_t n;
  auto value = JsValue(sab);
  for (n = 0; n < sharedArrayBuffers.size(); n++) {
    // If the SharedArrayBuffer has already been added, return the existing ID for it.
    if (sharedArrayBuffers[n].getHandle(js) == value) {
      return v8::Just(n);
    }
  }
  sharedArrayBuffers.add(jsg::JsRef(js, value));
  sharedBackingStores.add(sab->GetBackingStore());
  return v8::Just(n);
}

void* Serializer::ReallocateBufferMemory(void* oldBuffer, size_t size, size_t* actualSize) {
  auto result = v8::ValueSerializer::Delegate::ReallocateBufferMemory(oldBuffer, size, actualSize);
  if (result != nullptr) {
    dataCapacity = *actualSize;
  }
  return result;
}

void Serializer::throwDataCloneErrorForObject(jsg::Lock& js, v8::Local<v8::Object> obj) {
  // The default error that V8 would generate is "#<TypeName> could not be cloned." -- for some
  // reason, it surrounds the type name in "#<>", which seems bizarre? Let's generate a better
  // error.
  auto message = kj::str("Could not serialize object of type \"", obj->GetConstructorName(),
      "\". This type does "
      "not support serialization.");
  auto exception = js.domException(kj::str("DataCloneError"), kj::mv(message));
  js.throwException(jsg::JsValue(KJ_ASSERT_NONNULL(exception.tryGetHandle(js))));
}

void Serializer::ThrowDataCloneError(v8::Local<v8::String> message) {
  auto* isolate = jsg::Lock::current().v8Isolate;
  v8::Local<v8::Value> exception;
  if (newDataCloneError(isolate, message).ToLocal(&exception)) {
    isolate->ThrowException(exception);
  }
}

bool Serializer::HasCustomHostObject(v8::Isolate* isolate) {
  // By default, V8 will call WriteHostObject() for objects that have internal fields. We only need
  // to override IsHostObject() if we want to treat pure-JS objects differently, which we do if
  // treatClassInstancesAsPlainObjects is false, or if treatErrorsAsHostObjects is true.
  return !treatClassInstancesAsPlainObjects || treatErrorsAsHostObjects;
}

v8::Maybe<bool> Serializer::IsHostObject(v8::Isolate* isolate, v8::Local<v8::Object> object) {
  // This is only called if HasCustomHostObject() returned true.
  KJ_ASSERT(!treatClassInstancesAsPlainObjects || treatErrorsAsHostObjects);

  // Any object with internal fields is DEFINITELY a host object!
  if (object->InternalFieldCount() > 0) {
    return v8::Just(true);
  }

  // Native errors are host object if the enhanced_error_serialization compat flag is enalbed.
  if (object->IsNativeError()) {
    return v8::Just(treatErrorsAsHostObjects);
  }

  // If `treatClassInstancesAsPlainObjects` is on (the historical default), then nothing else is
  // a host object. If it has been turned off (e.g. for RPC), then any object that isn't a raw
  // object will be treated as a host object (mainly so that we can error out on these).
  if (treatClassInstancesAsPlainObjects) return v8::Just(false);
  KJ_ASSERT(!prototypeOfObject.IsEmpty());

  // If the object's prototype is Object.prototype, then it is a plain object, which we'll allow
  // to be serialized normally. Otherwise, it is a class instance, which we should treat as a host
  // object. Inside `WriteHostObject()` we will throw DataCloneError due to the object not having
  // internal fields.
  return v8::Just(object->GetPrototype() != prototypeOfObject);
}

v8::Maybe<bool> Serializer::WriteHostObject(v8::Isolate* isolate, v8::Local<v8::Object> object) {
  try {
    jsg::Lock& js = jsg::Lock::from(isolate);

    if (object->IsNativeError()) {
      return writeNativeError(isolate, ser, object);
    }

    writeApiObject(js, object);
    return v8::Just(true);
  } catch (JsExceptionThrown&) {
    return v8::Nothing<bool>();
  } catch (...) {
    throwInternalError(isolate, kj::getCaughtExceptionAsKj());
    return v8::Nothing<bool>();
  }
}

void Serializer::writeApiObject(Lock& js, v8::Local<v8::Object> object) {
  if (object->InternalFieldCount() != Wrappable::INTERNAL_FIELD_COUNT ||
      !Wrappable::isWorkerdApiObject(object)) {
    KJ_IF_SOME(eh, externalHandler) {
      if (object->IsProxy()) {
        eh.serializeProxy(js, *this, object.As<v8::Proxy>());
        return;
      } else if (object->IsFunction()) {
        eh.serializeFunction(js, *this, object.As<v8::Function>());
        return;
      } else if (eh.trySerializeClassInstance(js, *this, object)) {
        // The handler recognized this class instance (e.g. by brand check) and serialized it.
        return;
      }
    }

    // v8::ValueSerializer by default will send us anything that has internal fields, but this
    // object doesn't appear to match the internal fields expected on a JSG object.
    //
    // We also get here if treatClassInstancesAsPlainObjects is false, and the object is an
    // application-defined class. We don't currently support serializing class instances.
    throwDataCloneErrorForObject(js, object);
  }

  // The InternalFieldCount + isWorkerdApiObject() checks above established that this is one of
  // our wrapped objects; the serializer is then selected by the object's dynamic type, so we
  // only need the pointer here.
  Wrappable* wrappable = Wrappable::unwrapFromShimAnyType(js.v8Isolate, object);
  if (wrappable == nullptr) {
    // The object carries our marker (isWorkerdApiObject() passed) yet its CppHeap handle resolves
    // to no live wrappable. That should be unreachable.
    KJ_LOG(FATAL, "wrapper type mismatch: marked object's CppHeap handle resolves to no wrappable");
    abort();
  }

  // Only subclasses of `Object` register serializers, so anything else is not serializable.
  Object* obj = wrappable->jsgTryGetObject();
  if (obj == nullptr) {
    throwDataCloneErrorForObject(js, object);
  }

  if (!IsolateBase::from(js.v8Isolate).serialize(js, typeid(*wrappable), *obj, *this)) {
    // This type is not serializable.
    throwDataCloneErrorForObject(js, object);
  }
}

Serializer::Released Serializer::release() {
  KJ_ASSERT(!released, "The data has already been released.");
  released = true;

  // Now that serialization is complete, detach the ArrayBuffers that were transferred. We
  // intentionally defer this until after write() -- see transfer() for details.
  if (!arrayBuffersToDetach.empty()) {
    auto& js = jsg::Lock::current();
    for (auto& arrayBuffer: arrayBuffersToDetach) {
      auto handle = arrayBuffer.getHandle(js);
      // JS run during write() (e.g. an accessor getter performing a nested structuredClone that
      // transfers the same buffer, or calling ArrayBuffer.prototype.transfer()) may have already
      // detached this buffer. Skip those: v8::ArrayBuffer::Detach() fatally checks IsDetachable(),
      // and while a plain double-detach happens to be a no-op today, we don't want to depend on
      // that V8 internal.
      if (handle->IsDetachable() && !handle->WasDetached()) {
        check(handle->Detach(v8::Local<v8::Value>()));
      }
    }
  }

  sharedArrayBuffers.clear();
  arrayBuffers.clear();
  arrayBuffersToDetach.clear();
  auto pair = ser.Release();
  return Released{
    .data = kj::Array(pair.first, pair.second, jsg::SERIALIZED_BUFFER_DISPOSER),
    .dataCapacity = dataCapacity,
    .sharedArrayBuffers = sharedBackingStores.releaseAsArray(),
    .transferredArrayBuffers = backingStores.releaseAsArray(),
  };
}

void Serializer::transfer(Lock& js, const JsValue& value) {
  KJ_ASSERT(!released, "The data has already been released.");
  // Currently we only allow transfer of ArrayBuffers
  v8::Local<v8::ArrayBuffer> arrayBuffer;
  if (value.isArrayBufferView()) {
    auto view = v8::Local<v8::Value>(value).As<v8::ArrayBufferView>();
    arrayBuffer = view->Buffer();
  } else if (value.isArrayBuffer()) {
    arrayBuffer = v8::Local<v8::Value>(value).As<v8::ArrayBuffer>();
  } else {
    JSG_FAIL_REQUIRE(TypeError, "Object is not transferable");
  }

  // SharedArrayBuffers are not transferable. Per the HTML spec, attempting to
  // transfer a SharedArrayBuffer (or a view backed by one) must throw a
  // DataCloneError. We must check before calling Detach(), which would trigger
  // a fatal V8 CHECK failure on non-detachable buffers.
  JSG_REQUIRE(arrayBuffer->IsDetachable(), DOMDataCloneError, "Object is not transferable.");

  uint32_t n;
  // Each buffer is transferred once, however many times it or views of it are listed: V8 must
  // be told about a buffer only once.
  JsValue buffer(arrayBuffer);
  for (n = 0; n < arrayBuffers.size(); n++) {
    if (arrayBuffers[n].getHandle(js) == buffer) {
      return;
    }
  }
  arrayBuffers.add(jsg::JsRef(js, buffer));

  backingStores.add(arrayBuffer->GetBackingStore());
  ser.TransferArrayBuffer(n, arrayBuffer);

  // Defer detaching the ArrayBuffer until release(), after write() has run. We only register the
  // transfer with V8 here; TransferArrayBuffer() records the buffer in the serializer's transfer
  // map, and WriteValue() emits a transfer marker for it from that map regardless of whether the
  // buffer is currently detached. So detaching is not required for serialization to be correct,
  // and deferring it has two benefits:
  //
  // 1. It avoids a crash under V8's array buffer view tracking (--track-array-buffer-views, on by
  //    default in V8 >= 15.0): detaching a buffer updates any TypedArray/DataView over it in place,
  //    redirecting the view to a different (empty) backing buffer. If we detached here, a view
  //    serialized later by WriteValue() would no longer reference the buffer we registered for
  //    transfer, so V8 would try to clone a detached buffer and fail with "An ArrayBuffer is
  //    detached and could not be cloned."
  //
  // 2. It matches the HTML structured-clone-with-transfer algorithm, which serializes the value
  //    first and only detaches the transfer list afterwards. This matters because serialization is
  //    NOT free of JS execution -- WriteValue() invokes user accessor getters (see
  //    ValueSerializer::WriteJSObject -> Object::GetProperty), so a getter can observe a buffer
  //    that is in the transfer list while serialization is still running. Detaching up front (as we
  //    used to) would show such a getter a prematurely-detached buffer; deferring matches the spec
  //    (the buffer is still detached by the time structuredClone() returns, in release()).
  //
  // A vector is required because the transfer list may contain multiple ArrayBuffers.
  arrayBuffersToDetach.add(js.v8Ref(arrayBuffer));
}

void Serializer::write(Lock& js, const JsValue& value) {
  KJ_ASSERT(!released, "The data has already been released.");
  KJ_ASSERT(check(ser.WriteValue(js.v8Context(), value)));
}

Deserializer::ExternalHandler::~ExternalHandler() noexcept(false) {}

Deserializer::Deserializer(Lock& js,
    kj::ArrayPtr<const kj::byte> data,
    kj::Maybe<kj::ArrayPtr<std::shared_ptr<v8::BackingStore>>> transferredArrayBuffers,
    kj::Maybe<kj::ArrayPtr<std::shared_ptr<v8::BackingStore>>> sharedArrayBuffers,
    kj::Maybe<Options> maybeOptions)
    : totalInputSize(data.size()),
      deser(ownedDeser.emplace(js.v8Isolate,
          data.begin(),
          data.size(),
          static_cast<v8::ValueDeserializer::Delegate*>(this))),
      sharedBackingStores(kj::mv(sharedArrayBuffers)) {
#ifdef KJ_DEBUG
  kj::requireOnStack(this, "jsg::Deserializer must be allocated on the stack");
#endif
  init(js, data, kj::mv(transferredArrayBuffers), kj::mv(maybeOptions));
}

Deserializer::Deserializer(
    Lock& js, Serializer::Released& released, kj::Maybe<Options> maybeOptions)
    : Deserializer(js,
          released.data.asPtr(),
          released.transferredArrayBuffers.asPtr(),
          released.sharedArrayBuffers.asPtr(),
          kj::mv(maybeOptions)) {}

Deserializer::Deserializer(Lock& js, v8::ValueDeserializer& deser, Borrowed)
    : totalInputSize(0),
      deser(deser) {}

void Deserializer::init(Lock& js,
    kj::ArrayPtr<const kj::byte> data,
    kj::Maybe<kj::ArrayPtr<std::shared_ptr<v8::BackingStore>>> transferredArrayBuffers,
    kj::Maybe<Options> maybeOptions) {
  auto options = kj::mv(maybeOptions).orDefault({});
  externalHandler = options.externalHandler;
  if (options.readHeader) {
    JSG_TRY(js) {
      check(deser.ReadHeader(js.v8Context()));
    }
    JSG_CATCH(exception) {
      (void)exception;
      // Only report header metadata, never bytes from the serialized value. V8 leaves the wire
      // version at zero if the header is absent or its version could not be decoded.
      auto header = data.size() == 0 ? "empty" : data[0] == 0xff ? "present" : "missing";
      auto message = kj::str(
          "Unable to deserialize cloned data due to invalid or unsupported version. [context=",
          options.diagnosticContext.orDefault("unknown"), "; bytes=", data.size(),
          "; header=", header, "; wireVersion=", deser.GetWireFormatVersion(),
          "; maxWireVersion=", v8::CurrentValueSerializerFormatVersion(),
          "; v8=", v8::V8::GetVersion(), "]");
      LOG_WARNING_PERIODICALLY(message, kj::getStackTrace());
      // Header failures are internal faults. Suppress duplicate reporting when this exception
      // crosses a JavaScript or RPC boundary; the periodic warning above carries the diagnostic.
      kj::throwException(KJ_EXCEPTION(FAILED, "worker_do_not_log", message));
    }
  }
  preserveStackInErrors = options.preserveStackInErrors;
  KJ_IF_SOME(version, options.version) {
    KJ_ASSERT(version >= 13, "The minimum serialization version is 13.");
    deser.SetWireFormatVersion(version);
  }
  KJ_IF_SOME(arrayBuffers, transferredArrayBuffers) {
    for (auto n: kj::indices(arrayBuffers)) {
      deser.TransferArrayBuffer(n, v8::ArrayBuffer::New(js.v8Isolate, kj::mv((arrayBuffers)[n])));
    }
  }
}

JsValue Deserializer::readValue(Lock& js) {
  js.setDisallowJavascriptExecution(true);
  KJ_DEFER(js.setDisallowJavascriptExecution(false));
  return JsValue(check(deser.ReadValue(js.v8Context())));
}

uint Deserializer::readRawUint32() {
  uint32_t result;
  KJ_ASSERT(deser.ReadUint32(&result), "deserialization failure, possible corruption");
  return result;
}

uint64_t Deserializer::readRawUint64() {
  uint64_t result;
  KJ_ASSERT(deser.ReadUint64(&result), "deserialization failure, possible corruption");
  return result;
}

kj::ArrayPtr<const kj::byte> Deserializer::readRawBytes(size_t size) {
  const void* data;
  KJ_ASSERT(deser.ReadRawBytes(size, &data), "deserialization failure, possible corruption");
  return kj::arrayPtr(reinterpret_cast<const kj::byte*>(data), size);
}

kj::ArrayPtr<const kj::byte> Deserializer::readLengthDelimitedBytes() {
  return readRawBytes(readRawUint64());
}

kj::String Deserializer::readRawString(size_t size) {
  return kj::str(readRawBytes(size).asChars());
}

kj::String Deserializer::readLengthDelimitedString() {
  return kj::str(readLengthDelimitedBytes().asChars());
}

v8::MaybeLocal<v8::SharedArrayBuffer> Deserializer::GetSharedArrayBufferFromId(
    v8::Isolate* isolate, uint32_t clone_id) {
  KJ_IF_SOME(backingStores, sharedBackingStores) {
    // V8 calls this, so it must throw a JavaScript exception, not a KJ one.
    if (clone_id >= backingStores.size()) {
      throwCorrupt(isolate);
      return {};
    }
    return v8::SharedArrayBuffer::New(isolate, backingStores[clone_id]);
  }
  return v8::MaybeLocal<v8::SharedArrayBuffer>();
}

v8::MaybeLocal<v8::Object> Deserializer::ReadHostObject(v8::Isolate* isolate) {
  try {
    uint tag = readRawUint32();

    if (tag == SERIALIZATION_TAG_NATIVE_ERROR) {
      return readNativeError(isolate, deser, PreserveStack(preserveStackInErrors));
    }

    return readApiObject(Lock::from(isolate), tag);
  } catch (JsExceptionThrown&) {
    return {};
  } catch (...) {
    throwInternalError(isolate, kj::getCaughtExceptionAsKj());
    return {};
  }
}

v8::Local<v8::Object> Deserializer::readApiObject(Lock& js, uint32_t tag) {
  KJ_IF_SOME(result, IsolateBase::from(js.v8Isolate).deserialize(js, tag, *this)) {
    return result;
  } else {
    // Unknown tag is a platform error, so use KJ assert.
    KJ_FAIL_ASSERT("encountered unknown tag in deserialization", tag);
  }
}

v8::Maybe<bool> writeApiObject(
    v8::Isolate* isolate, v8::ValueSerializer& ser, v8::Local<v8::Object> object) {
  try {
    auto& js = Lock::from(isolate);
    Serializer serializer(js, ser, Serializer::Borrowed{});
    serializer.writeApiObject(js, object);
    return v8::Just(true);
  } catch (JsExceptionThrown&) {
    return v8::Nothing<bool>();
  } catch (...) {
    throwInternalError(isolate, kj::getCaughtExceptionAsKj());
    return v8::Nothing<bool>();
  }
}

v8::MaybeLocal<v8::Object> readApiObject(
    v8::Isolate* isolate, v8::ValueDeserializer& deser, uint32_t tag) {
  try {
    auto& js = Lock::from(isolate);
    js.setDisallowJavascriptExecution(true);
    KJ_DEFER(js.setDisallowJavascriptExecution(false));
    Deserializer deserializer(js, deser, Deserializer::Borrowed{});
    return deserializer.readApiObject(js, tag);
  } catch (JsExceptionThrown&) {
    return {};
  } catch (...) {
    throwInternalError(isolate, kj::getCaughtExceptionAsKj());
    return {};
  }
}

v8::Maybe<bool> writeNativeError(
    v8::Isolate* isolate, v8::ValueSerializer& ser, v8::Local<v8::Object> error) {
  auto context = isolate->GetCurrentContext();
  auto nameKey = newInternalizedString(isolate, "name"_kj);
  auto messageKey = newInternalizedString(isolate, "message"_kj);

  ser.WriteUint32(SERIALIZATION_TAG_NATIVE_ERROR);

  v8::Local<v8::Value> name;
  v8::Local<v8::String> nameString;
  if (!error->Get(context, nameKey).ToLocal(&name) ||
      !name->ToDetailString(context).ToLocal(&nameString)) {
    return v8::Nothing<bool>();
  }
  auto tag = errorTagOf(isolate, nameString);
  ser.WriteUint32(tag);
  // The name of a known type is implied by the tag.
  if (tag == ERROR_TAG_UNKNOWN && ser.WriteValue(context, name).IsNothing()) {
    return v8::Nothing<bool>();
  }

  // The message comes before the other properties, since the deserializer needs it to create
  // the error.
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
    // A data property rather than Set(), as when reading, so that each property is copied as it
    // is: Set() would run a setter that Object.prototype has for the key instead, and an own
    // `__proto__` would replace the object's prototype.
    v8::Local<v8::Value> value;
    if (!error->Get(context, key).ToLocal(&value) ||
        properties->CreateDataProperty(context, key.As<v8::Name>(), value).IsNothing()) {
      return v8::Nothing<bool>();
    }
  }
  return ser.WriteValue(context, properties);
}

v8::MaybeLocal<v8::Object> readNativeError(
    v8::Isolate* isolate, v8::ValueDeserializer& deser, PreserveStack preserveStack) {
  auto context = isolate->GetCurrentContext();

  uint32_t tag;
  if (!deser.ReadUint32(&tag)) {
    throwCorrupt(isolate);
    return {};
  }
  v8::Local<v8::Value> name;
  if (tag == ERROR_TAG_UNKNOWN && !deser.ReadValue(context).ToLocal(&name)) return {};

  v8::Local<v8::Value> message;
  if (!deser.ReadValue(context).ToLocal(&message)) return {};
  if (!message->IsString()) {
    throwDataCloneError(isolate, "Deserialization failed: error message is not a string"_kj);
    return {};
  }
  auto error = newNativeError(tag, message.As<v8::String>());

  // An error of an unknown type is created as a generic Error with the name it was written
  // with, so it is not an instance of the class it was created from. The name is defined as an
  // own property, since an error's `name` usually comes from its prototype.
  if (!name.IsEmpty() &&
      error->DefineOwnProperty(context, newInternalizedString(isolate, "name"_kj), name)
          .IsNothing()) {
    return {};
  }

  v8::Local<v8::Value> properties;
  if (!deser.ReadValue(context).ToLocal(&properties)) return {};
  if (properties->IsObject()) {
    auto object = properties.As<v8::Object>();
    auto stackKey = newInternalizedString(isolate, "stack"_kj);
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
      if (!preserveStack && key->StrictEquals(stackKey)) continue;
      // A data property rather than Set(), so that no setter on the error's prototype chain runs:
      // deserialization runs with JavaScript execution disallowed, where calling one is fatal,
      // and the structured clone spec defines own data properties.
      if (!object->Get(context, key).ToLocal(&value) ||
          error->CreateDataProperty(context, key.As<v8::Name>(), value).IsNothing()) {
        return {};
      }
    }
  }
  return error;
}

v8::MaybeLocal<v8::Value> newDataCloneError(v8::Isolate* isolate, v8::Local<v8::String> message) {
  auto& js = Lock::from(isolate);
  try {
    auto exception = js.domException(kj::str("DataCloneError"), kj::str(message));
    return KJ_ASSERT_NONNULL(exception.tryGetHandle(js));
  } catch (JsExceptionThrown&) {
    // Creating the DOMException threw, most likely because execution is terminating; that
    // exception stays pending.
    return {};
  } catch (...) {
    throwInternalError(isolate, kj::getCaughtExceptionAsKj());
    return {};
  }
}

void SerializedBufferDisposer::disposeImpl(void* firstElement,
    size_t elementSize,
    size_t elementCount,
    size_t capacity,
    void (*destroyElement)(void*)) const {
  free(firstElement);
}

JsValue structuredClone(
    Lock& js, const JsValue& value, kj::Maybe<kj::Array<JsValue>> maybeTransfer) {
  Serializer ser(js);
  KJ_IF_SOME(transfers, maybeTransfer) {
    for (auto& item: transfers) {
      ser.transfer(js, item);
    }
  }
  ser.write(js, value);
  auto released = ser.release();
  Deserializer des(js, released);
  return des.readValue(js);
}

}  // namespace workerd::jsg
