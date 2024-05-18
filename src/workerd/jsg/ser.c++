// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "ser.h"
#include "setup.h"

namespace workerd::jsg {

void Serializer::ExternalHandler::serializeFunction(
    jsg::Lock& js, jsg::Serializer& serializer, v8::Local<v8::Function> func) {
  JSG_FAIL_REQUIRE(DOMDataCloneError, func, " could not be cloned.");
}

Serializer::Serializer(Lock& js, Options options)
    : externalHandler(options.externalHandler),
      treatClassInstancesAsPlainObjects(options.treatClassInstancesAsPlainObjects),
      ser(js.v8Isolate, this) {
#ifdef KJ_DEBUG
  kj::requireOnStack(this, "jsg::Serializer must be allocated on the stack");
#endif
  if (!treatClassInstancesAsPlainObjects) {
    prototypeOfObject = js.obj().getPrototype();
  }
  if (externalHandler != kj::none) {
    // If we have an ExternalHandler, we'll ask it to serialize host objects.
    ser.SetTreatFunctionsAsHostObjects(true);
  }
  KJ_IF_SOME(version, options.version) {
    KJ_ASSERT(version >= 13, "The minimum serialization version is 13.");
    KJ_ASSERT(jsg::check(ser.SetWriteVersion(version)));
  }
  if (!options.omitHeader) {
    ser.WriteHeader();
  }
}

v8::Maybe<uint32_t> Serializer::GetSharedArrayBufferId(
    v8::Isolate *isolate,
    v8::Local<v8::SharedArrayBuffer> sab) {
  uint32_t n;
  auto value = JsValue(sab);
  for (n = 0; n < sharedArrayBuffers.size(); n++) {
    // If the SharedArrayBuffer has already been added, return the existing ID for it.
    if (sharedArrayBuffers[n] == value) {
      return v8::Just(n);
    }
  }
  sharedArrayBuffers.add(value);
  sharedBackingStores.add(sab->GetBackingStore());
  return v8::Just(n);
}

void Serializer::throwDataCloneErrorForObject(jsg::Lock& js, v8::Local<v8::Object> obj) {
  // The default error that V8 would generate is "#<TypeName> could not be cloned." -- for some
  // reason, it surrounds the type name in "#<>", which seems bizarre? Let's generate a better
  // error.
  auto message = kj::str(
      "Could not serialize object of type \"", obj->GetConstructorName(), "\". This type does "
      "not support serialization.");
  auto exception = js.domException(kj::str("DataCloneError"), kj::mv(message));
  js.throwException(jsg::JsValue(KJ_ASSERT_NONNULL(exception.tryGetHandle(js))));
}

void Serializer::ThrowDataCloneError(v8::Local<v8::String> message) {
  auto isolate = v8::Isolate::GetCurrent();
  try {
    Lock& js = Lock::from(isolate);
    auto exception = js.domException(kj::str("DataCloneError"), kj::str(message));
    isolate->ThrowException(KJ_ASSERT_NONNULL(exception.tryGetHandle(js)));
  } catch (JsExceptionThrown&) {
    // Apparently an exception was thrown during the construction of the DOMException. Most likely
    // we were terminated. In any case, we'll let that exception stay scheduled and propagate back
    // to V8.
  } catch (...) {
    // A KJ exception was thrown, we'll have to convert it to JavaScript and propagate that
    // exception instead.
    throwInternalError(isolate, kj::getCaughtExceptionAsKj());
  }
}

bool Serializer::HasCustomHostObject(v8::Isolate* isolate) {
  // V8 will always call WriteHostObject() for objects that have internal fields. We only need
  // to override IsHostObject() if we want to treat pure-JS objects differently, which we do if
  // treatClassInstancesAsPlainObjects is false.
  return !treatClassInstancesAsPlainObjects;
}

v8::Maybe<bool> Serializer::IsHostObject(v8::Isolate* isolate, v8::Local<v8::Object> object) {
  // This is only called if HasCustomHostObject() returned true.
  KJ_ASSERT(!treatClassInstancesAsPlainObjects);
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

    if (object->InternalFieldCount() != Wrappable::INTERNAL_FIELD_COUNT ||
        object->GetAlignedPointerFromInternalField(Wrappable::WRAPPABLE_TAG_FIELD_INDEX) !=
            &Wrappable::WRAPPABLE_TAG) {
      KJ_IF_SOME(eh, externalHandler) {
        if (object->IsFunction()) {
          eh.serializeFunction(js, *this, object.As<v8::Function>());
          return v8::Just(true);
        }
      }

      // v8::ValueSerializer by default will send us anything that has internal fields, but this
      // object doesn't appear to match the internal fields expected on a JSG object.
      //
      // We also get here if treatClassInstancesAsPlainObjects is false, and the object is an
      // application-defined class. We don't currently support serializing class instances.
      throwDataCloneErrorForObject(js, object);
    }

    Wrappable* wrappable = reinterpret_cast<Wrappable*>(
        object->GetAlignedPointerFromInternalField(Wrappable::WRAPPED_OBJECT_FIELD_INDEX));

    // HACK: Although we don't technically know yet that `wrappable` is an `Object`, we know that
    //   only subclasses of `Object` register serializers. So *if* a serializer is found, then this
    //   cast is valid, and the pointer won't be accessed otherwise. We can't do a dynamic_cast
    //   here since `Wrappable` is privately inherited by `Object` and anyway we don't want the
    //   overhead of dynamic_cast.
    // TODO(cleanup): Probably `Wrappable` should contain a bool indicating if it is an `Object`
    //   or not?
    Object* obj = reinterpret_cast<jsg::Object*>(wrappable);

    if (!IsolateBase::from(isolate).serialize(
          Lock::from(isolate), typeid(*wrappable), *obj, *this)) {
      // This type is not serializable.
      throwDataCloneErrorForObject(js, object);
    }

    return v8::Just(true);
  } catch (JsExceptionThrown&) {
    return v8::Nothing<bool>();
  } catch (...) {
    throwInternalError(isolate, kj::getCaughtExceptionAsKj());
    return v8::Nothing<bool>();
  }
}

Serializer::Released Serializer::release() {
  KJ_ASSERT(!released, "The data has already been released.");
  released = true;
  sharedArrayBuffers.clear();
  arrayBuffers.clear();
  auto pair = ser.Release();
  return Released {
    .data = kj::Array(pair.first, pair.second, jsg::SERIALIZED_BUFFER_DISPOSER),
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

  uint32_t n;
  for (n = 0; n < arrayBuffers.size(); n++) {
    // If the ArrayBuffer has already been added, we do not want to try adding it again.
    if (arrayBuffers[n] == value) {
      return;
    }
  }
  arrayBuffers.add(value);

  backingStores.add(arrayBuffer->GetBackingStore());
  check(arrayBuffer->Detach(v8::Local<v8::Value>()));
  ser.TransferArrayBuffer(n, arrayBuffer);
}

void Serializer::write(Lock& js, const JsValue& value) {
  KJ_ASSERT(!released, "The data has already been released.");
  KJ_ASSERT(check(ser.WriteValue(js.v8Context(), value)));
}

Deserializer::ExternalHandler::~ExternalHandler() noexcept(false) {}

Deserializer::Deserializer(
    Lock& js,
    kj::ArrayPtr<const kj::byte> data,
    kj::Maybe<kj::ArrayPtr<std::shared_ptr<v8::BackingStore>>> transferredArrayBuffers,
    kj::Maybe<kj::ArrayPtr<std::shared_ptr<v8::BackingStore>>> sharedArrayBuffers,
    kj::Maybe<Options> maybeOptions)
    : totalInputSize(data.size()),
      deser(js.v8Isolate, data.begin(), data.size(), this),
      sharedBackingStores(kj::mv(sharedArrayBuffers)) {
#ifdef KJ_DEBUG
  kj::requireOnStack(this, "jsg::Deserializer must be allocated on the stack");
#endif
  init(js, kj::mv(transferredArrayBuffers), kj::mv(maybeOptions));
}

Deserializer::Deserializer(
    Lock& js,
    Serializer::Released& released,
    kj::Maybe<Options> maybeOptions)
    : Deserializer(
        js,
        released.data.asPtr(),
        released.transferredArrayBuffers.asPtr(),
        released.sharedArrayBuffers.asPtr(),
        kj::mv(maybeOptions)) {}

void Deserializer::init(
    Lock& js,
    kj::Maybe<kj::ArrayPtr<std::shared_ptr<v8::BackingStore>>> transferredArrayBuffers,
    kj::Maybe<Options> maybeOptions) {
  auto options = kj::mv(maybeOptions).orDefault({});
  externalHandler = options.externalHandler;
  if (options.readHeader) {
    check(deser.ReadHeader(js.v8Context()));
  }
  KJ_IF_SOME(version, options.version) {
    KJ_ASSERT(version >= 13, "The minimum serialization version is 13.");
    deser.SetWireFormatVersion(version);
  }
  KJ_IF_SOME(arrayBuffers, transferredArrayBuffers) {
    for (auto n : kj::indices(arrayBuffers)) {
      deser.TransferArrayBuffer(n,
          v8::ArrayBuffer::New(js.v8Isolate, kj::mv((arrayBuffers)[n])));
    }
  }
}

JsValue Deserializer::readValue(Lock& js) {
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
    v8::Isolate* isolate,
    uint32_t clone_id) {
  KJ_IF_SOME(backingStores, sharedBackingStores) {
    KJ_ASSERT(clone_id < backingStores.size());
    return v8::SharedArrayBuffer::New(isolate, backingStores[clone_id]);
  }
  return v8::MaybeLocal<v8::SharedArrayBuffer>();
}

v8::MaybeLocal<v8::Object> Deserializer::ReadHostObject(v8::Isolate* isolate) {
  try {
    uint tag = readRawUint32();
    KJ_IF_SOME(result, IsolateBase::from(isolate).deserialize(Lock::from(isolate), tag, *this)) {
      return result;
    } else {
      // Unknown tag is a platform error, so use KJ assert.
      KJ_FAIL_ASSERT("encountered unknown tag in deserialization", tag);
    }
  } catch (JsExceptionThrown&) {
    return {};
  } catch (...) {
    throwInternalError(isolate, kj::getCaughtExceptionAsKj());
    return {};
  }
}

void SerializedBufferDisposer::disposeImpl(
    void* firstElement,
    size_t elementSize,
    size_t elementCount,
    size_t capacity,
    void (*destroyElement)(void*)) const {
  free(firstElement);
}

JsValue structuredClone(
    Lock& js,
    const JsValue& value,
    kj::Maybe<kj::Array<JsValue>> maybeTransfer) {
  Serializer ser(js);
  KJ_IF_SOME(transfers, maybeTransfer) {
    for (auto& item : transfers) {
      ser.transfer(js, item);
    }
  }
  ser.write(js, value);
  auto released = ser.release();
  Deserializer des(js, released, kj::none);
  return des.readValue(js);
}

}  // namespace workerd::jsg
