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
// Keep in sync with the nativeError serialization tag defined in
// worker-interface.capnp
constexpr uint32_t SERIALIZATION_TAG_NATIVE_ERROR = 10;

// The error tag is serialized as a uint32_t immediately following
// the SERIALIZATION_TAG_NATIVE_ERROR tag in order to more efficiently
// determine the type of error when deserializing so that we can
// construct the appropriate v8::Exception type on deserialization
// without having to expensive string comparison on the error name.
enum class ErrorTag : uint32_t {
  // The UNKNOWN tag is used when the error name is not recognized.
  // When this occurs, we will serialize the name of the error, and
  // when it is deserialized, we will create a generic Error and then
  // set the name to the stored name.
  UNKNOWN,
  ERROR,
  TYPE_ERROR,
  RANGE_ERROR,
  REFERENCE_ERROR,
  SYNTAX_ERROR,
  WASM_COMPILE_ERROR,
  WASM_LINK_ERROR,
  WASM_RUNTIME_ERROR,
  WASM_SUSPEND_ERROR,
  EVAL_ERROR,
  URI_ERROR,
  AGGREGATE_ERROR,
  SUPPRESSED_ERROR,
};

constexpr ErrorTag getErrorTagFromName(jsg::Lock& js, kj::StringPtr str) {
  if (str == "Error"_kj) {
    return ErrorTag::ERROR;
  } else if (str == "TypeError"_kj) {
    return ErrorTag::TYPE_ERROR;
  } else if (str == "RangeError"_kj) {
    return ErrorTag::RANGE_ERROR;
  } else if (str == "ReferenceError"_kj) {
    return ErrorTag::REFERENCE_ERROR;
  } else if (str == "SyntaxError"_kj) {
    return ErrorTag::SYNTAX_ERROR;
  } else if (str == "WasmCompileError"_kj) {
    return ErrorTag::WASM_COMPILE_ERROR;
  } else if (str == "WasmLinkError"_kj) {
    return ErrorTag::WASM_LINK_ERROR;
  } else if (str == "WasmRuntimeError"_kj) {
    return ErrorTag::WASM_RUNTIME_ERROR;
  } else if (str == "WasmSuspendError"_kj) {
    return ErrorTag::WASM_SUSPEND_ERROR;
  } else if (str == "EvalError"_kj) {
    return ErrorTag::EVAL_ERROR;
  } else if (str == "URIError"_kj) {
    return ErrorTag::URI_ERROR;
  } else if (str == "AggregateError"_kj) {
    return ErrorTag::AGGREGATE_ERROR;
  } else if (str == "SuppressedError"_kj) {
    return ErrorTag::SUPPRESSED_ERROR;
  }
  return ErrorTag::UNKNOWN;
}

JsObject toJsError(Lock& js, ErrorTag tag, JsString message) {
  switch (tag) {
    case ErrorTag::ERROR: {
      return JsObject(v8::Exception::Error(message).As<v8::Object>());
    }
    case ErrorTag::TYPE_ERROR: {
      return JsObject(v8::Exception::TypeError(message).As<v8::Object>());
    }
    case ErrorTag::RANGE_ERROR: {
      return JsObject(v8::Exception::RangeError(message).As<v8::Object>());
    }
    case ErrorTag::REFERENCE_ERROR: {
      return JsObject(v8::Exception::ReferenceError(message).As<v8::Object>());
    }
    case ErrorTag::SYNTAX_ERROR: {
      return JsObject(v8::Exception::SyntaxError(message).As<v8::Object>());
    }
    case ErrorTag::WASM_COMPILE_ERROR: {
      return JsObject(v8::Exception::WasmCompileError(message).As<v8::Object>());
    }
    case ErrorTag::WASM_LINK_ERROR: {
      return JsObject(v8::Exception::WasmLinkError(message).As<v8::Object>());
    }
    case ErrorTag::WASM_RUNTIME_ERROR: {
      return JsObject(v8::Exception::WasmRuntimeError(message).As<v8::Object>());
    }
    case ErrorTag::WASM_SUSPEND_ERROR: {
      return JsObject(v8::Exception::WasmSuspendError(message).As<v8::Object>());
    }
    case ErrorTag::EVAL_ERROR: {
      return JsObject(v8::Exception::EvalError(message).As<v8::Object>());
    }
    case ErrorTag::URI_ERROR: {
      return JsObject(v8::Exception::URIError(message).As<v8::Object>());
    }
    case ErrorTag::AGGREGATE_ERROR: {
      return JsObject(v8::Exception::AggregateError(message).As<v8::Object>());
    }
    case ErrorTag::SUPPRESSED_ERROR: {
      return JsObject(v8::Exception::SuppressedError(message).As<v8::Object>());
    }
    case ErrorTag::UNKNOWN: {
      return JsObject(v8::Exception::Error(message).As<v8::Object>());
    }
  }
  return JsObject(v8::Exception::Error(message).As<v8::Object>());
}

struct OwnDataProperty {
  v8::Local<v8::Object> descriptor;
  v8::Local<v8::Value> value;

  // Whether the property is neither writable nor configurable, as a frozen object's are, so that
  // its value can never change.
  bool isConstant(Lock& js) const {
    auto context = js.v8Context();
    // The descriptor is a new plain object, whose own properties these are.
    auto isTrue = [&](kj::StringPtr flag) {
      v8::Local<v8::String> flagKey = js.strIntern(flag);
      return check(descriptor->GetRealNamedProperty(context, flagKey))->IsTrue();
    };
    return !isTrue("writable"_kj) && !isTrue("configurable"_kj);
  }
};

// Returns `object`'s own data property `name`, or kj::none if it has none, or has an accessor
// instead, or `object` is a workerd API object. Runs no JavaScript: it calls no Proxy trap or
// JavaScript accessor, and does not read the prototype chain.
kj::Maybe<OwnDataProperty> tryGetOwnDataProperty(
    Lock& js, v8::Local<v8::Object> object, kj::StringPtr name) {
  // GetOwnPropertyDescriptor() below calls an API object's interceptors and native accessors,
  // which may run code. Anything this reads is an ordinary object.
  if (object->InternalFieldCount() > 0) return kj::none;
  auto context = js.v8Context();
  v8::Local<v8::String> key = js.strIntern(name);
  // False for a Proxy, so the descriptor below is not a Proxy trap's.
  if (!check(object->HasRealNamedProperty(context, key))) return kj::none;
  // A native accessor, such as a function's `prototype`, is described as a data property.
  auto maybeDescriptor = check(object->GetOwnPropertyDescriptor(context, key));
  if (!maybeDescriptor->IsObject()) return kj::none;
  auto descriptor = maybeDescriptor.As<v8::Object>();
  v8::Local<v8::String> valueKey = js.strIntern("value"_kj);
  if (!check(descriptor->HasOwnProperty(context, valueKey))) return kj::none;
  return OwnDataProperty{
    .descriptor = descriptor,
    .value = check(descriptor->GetRealNamedProperty(context, valueKey)),
  };
}

// Returns the value of `object`'s own data property `name`, as tryGetOwnDataProperty() does.
kj::Maybe<v8::Local<v8::Value>> tryGetOwnProperty(
    Lock& js, v8::Local<v8::Object> object, kj::StringPtr name) {
  KJ_IF_SOME(property, tryGetOwnDataProperty(js, object, name)) {
    return property.value;
  }
  return kj::none;
}

// The flags of a SERIALIZATION_TAG_JS_SERIALIZABLE instance (see ser.h).
constexpr uint32_t JS_SERIALIZABLE_HAS_STACK = 1;
constexpr uint32_t JS_SERIALIZABLE_KEEP_STACK = 2;
constexpr uint32_t JS_SERIALIZABLE_HAS_MESSAGE = 4;
constexpr uint32_t JS_SERIALIZABLE_KNOWN_FLAGS =
    JS_SERIALIZABLE_HAS_STACK | JS_SERIALIZABLE_KEEP_STACK | JS_SERIALIZABLE_HAS_MESSAGE;

// The message of the DataCloneError that a raw read throws when the data ends too soon. Like V8's
// own error for malformed data, it is not an internal error: corrupt or crafted bytes are not a
// bug in the reader.
constexpr kj::StringPtr MALFORMED_DATA_MESSAGE =
    "Deserialization failed: the data is malformed."_kj;
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
  prototypeOfObject = js.obj().getPrototype(js);
  jsSerializeModeKey = IsolateBase::getJsSerializeModeKey(js.v8Isolate);
  domExceptionBrandKey = IsolateBase::getDomExceptionBrandKey(js.v8Isolate);
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
  js.throwException(js.domException(kj::str("DataCloneError"), kj::mv(message)));
}

void Serializer::ThrowDataCloneError(v8::Local<v8::String> message) {
  auto* isolate = jsg::Lock::current().v8Isolate;
  v8::Local<v8::Value> exception;
  if (newDataCloneError(isolate, message).ToLocal(&exception)) {
    isolate->ThrowException(exception);
  }
}

bool Serializer::HasCustomHostObject(v8::Isolate* isolate) {
  // By default, V8 will call WriteHostObject() only for objects that have internal fields. We
  // treat pure-JS objects differently too: instances of classes implemented in JavaScript, native
  // errors if treatErrorsAsHostObjects is true, and class instances if
  // treatClassInstancesAsPlainObjects is false.
  return true;
}

v8::Maybe<bool> Serializer::IsHostObject(v8::Isolate* isolate, v8::Local<v8::Object> object) {
  // Any object with internal fields is DEFINITELY a host object!
  if (object->InternalFieldCount() > 0) {
    return v8::Just(true);
  }

  // So is an instance of a class implemented in JavaScript, including one that is a native error.
  // Such an instance's prototype is its class's, so a plain object, the most common kind by far,
  // is not checked for the private property. Reading one runs no JavaScript.
  auto prototype = object->GetPrototype();
  if (prototype != prototypeOfObject && !prototype->IsNull()) {
    bool hasMode;
    if (!object->HasPrivate(isolate->GetCurrentContext(), jsSerializeModeKey).To(&hasMode)) {
      return v8::Nothing<bool>();
    }
    if (hasMode) {
      return v8::Just(true);
    }
    bool isDomException;
    if (!object->HasPrivate(isolate->GetCurrentContext(), domExceptionBrandKey)
             .To(&isDomException)) {
      return v8::Nothing<bool>();
    }
    if (isDomException) {
      return v8::Just(true);
    }
  }

  // Native errors are host object if the enhanced_error_serialization compat flag is enalbed.
  if (object->IsNativeError()) {
    return v8::Just(treatErrorsAsHostObjects);
  }

  // If `treatClassInstancesAsPlainObjects` is on (the historical default), then nothing else is
  // a host object. If it has been turned off (e.g. for RPC), then any object that isn't a raw
  // object will be treated as a host object (mainly so that we can error out on these).
  if (treatClassInstancesAsPlainObjects) return v8::Just(false);

  // If the object's prototype is Object.prototype, then it is a plain object, which we'll allow
  // to be serialized normally. Otherwise, it is a class instance, which we should treat as a host
  // object. Inside `WriteHostObject()` we will throw DataCloneError due to the object not having
  // internal fields.
  return v8::Just(prototype != prototypeOfObject);
}

v8::Maybe<bool> Serializer::WriteHostObject(v8::Isolate* isolate, v8::Local<v8::Object> object) {
  try {
    jsg::Lock& js = jsg::Lock::from(isolate);

    // Checked before DOMExceptions and native errors, since such a class may make them.
    KJ_IF_SOME(mark, tryGetJsSerializeMark(js, object)) {
      writeJsSerializable(js, object, mark);
      return v8::Just(true);
    }

    // The bootstrap's DOMException, a native error, writes what the C++ class does.
    KJ_IF_SOME(info, tryGetBootstrapDomExceptionInfo(js, object)) {
      writeRawUint32(DOMException::SERIALIZATION_TAG_V2);
      writeLengthDelimited(info.name.toString(js));
      writeLengthDelimited(info.message.toString(js));
      writeLengthDelimited(JsObject(object).get(js, "stack"_kj).toString(js));
      return v8::Just(true);
    }

    if (object->IsNativeError()) {
      auto nameStr = js.str("name"_kj);
      auto messageStr = js.str("message"_kj);

      // Get the standard name, message, stack, cause, error, errors properties from
      // the error object.
      writeRawUint32(SERIALIZATION_TAG_NATIVE_ERROR);

      // A mix of ad-hoc and regular serialization. We first serialize
      // the error tag, which is an enum that identifies the type of error
      // for faster/easier deserialization. Then we serialize the message which
      // usually come from the prototype. Then we grab the own properties,
      // serializing the number of them followed by each name and value in
      // sequence.

      jsg::JsObject errorObj(object);
      auto name = errorObj.get(js, nameStr);
      auto nameKjStr = name.toString(js);
      auto tag = getErrorTagFromName(js, nameKjStr);
      writeRawUint32(static_cast<uint32_t>(tag));
      // We only write the name if it is not one of the known error types.
      if (tag == ErrorTag::UNKNOWN) {
        write(js, name);
      }

      write(js, errorObj.get(js, messageStr));

      auto names = errorObj.getPropertyNames(js, KeyCollectionFilter::OWN_ONLY,
          PropertyFilter::ALL_PROPERTIES, IndexFilter::SKIP_INDICES);

      auto obj = js.obj();
      for (size_t n = 0; n < names.size(); n++) {
        auto name = names.get(js, n);
        // The name typically comes from the prototype and therefore
        // do not show up in the own properties of the error object, and the
        // message we want to treat specially since we need it early in the
        // deserialization.
        // Since we already have them serialized above, we can filter them
        // out here.
        if (name.strictEquals(nameStr) || name.strictEquals(messageStr)) continue;

        obj.set(js, name, errorObj.get(js, name));
      }
      write(js, obj);

      return v8::Just(true);
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
      }
    }

    // v8::ValueSerializer by default will send us anything that has internal fields, but this
    // object doesn't appear to match the internal fields expected on a JSG object.
    //
    // We also get here if treatClassInstancesAsPlainObjects is false, and the object is an
    // application-defined class. We don't currently support serializing class instances. An
    // instance of a class implemented in JavaScript never gets here: WriteHostObject() hands it to
    // writeJsSerializable(), which offers it to the ExternalHandler.
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

kj::Maybe<v8::Local<v8::Value>> Serializer::tryGetJsSerializeMark(
    Lock& js, v8::Local<v8::Object> object) {
  // Such an instance has no internal fields, unlike a workerd API object.
  if (jsSerializeModeKey.IsEmpty() || object->InternalFieldCount() > 0) return kj::none;
  // No mark is undefined (see serialization.ts), so undefined means that there is none.
  auto mark = check(object->GetPrivate(js.v8Context(), jsSerializeModeKey));
  if (mark->IsUndefined()) return kj::none;
  return mark;
}

void Serializer::writeJsSerializable(
    Lock& js, v8::Local<v8::Object> object, v8::Local<v8::Value> mark) {
  // A class whose instances are externals in this context, such as a stream over RPC, need not be
  // cloneable.
  KJ_IF_SOME(eh, externalHandler) {
    if (eh.trySerializeClassInstance(js, *this, object)) return;
  }

  auto& cls = getJsSerializableClass(js, object, mark);
  v8::Local<v8::Function> clone = cls.clone.getHandle(js);
  // Stays valid when jsSerializableClasses grows, which moves the string but not its text.
  kj::StringPtr deserializeInfo = cls.deserializeInfo;

  auto context = js.v8Context();
  v8::Local<v8::Value> arg = object;
  auto data = check(clone->Call(context, js.undefined(), 1, &arg));

  // An error's message and stack are written as a native error's are, so that the deserializer,
  // rather than the class, decides whether to keep the stack, and a stack it captures begins with
  // the message. Without enhanced error serialization, a native error is written in V8's own
  // format, which keeps its stack whatever the deserializer's options, so the flags say that this
  // one keeps its stack too. As in V8's format, each is written only if it is a string: anything
  // else could refer back to the instance, which a reader cannot resolve while it is still reading
  // the instance's record.
  uint32_t flags = 0;
  kj::Maybe<JsValue> message;
  kj::Maybe<JsValue> stack;
  // An error as Error.isError() reports it: V8 asks isJsApiWrapperNativeErrorCallback() about an
  // API wrapper object, which is one if it is an instance of the bootstrap's DOMException.
  if (object->IsNativeError() || check(object->HasPrivate(context, domExceptionBrandKey))) {
    auto value = check(object->Get(context, js.strIntern("message"_kj)));
    if (value->IsString()) {
      flags |= JS_SERIALIZABLE_HAS_MESSAGE;
      message = JsValue(value);
    }
    v8::Local<v8::String> stackKey = js.strIntern("stack"_kj);
    if (check(object->HasOwnProperty(context, stackKey))) {
      auto value = check(object->Get(context, stackKey));
      if (value->IsString()) {
        flags |= JS_SERIALIZABLE_HAS_STACK;
        if (!treatErrorsAsHostObjects) flags |= JS_SERIALIZABLE_KEEP_STACK;
        stack = JsValue(value);
      }
    }
  }

  writeRawUint32(SERIALIZATION_TAG_JS_SERIALIZABLE);
  writeLengthDelimited(deserializeInfo);
  writeRawUint32(flags);
  KJ_IF_SOME(m, message) {
    write(js, m);
  }
  KJ_IF_SOME(s, stack) {
    write(js, s);
  }
  pendingJsSerializableData.add(js.v8Ref(data));
}

Serializer::JsSerializableClass& Serializer::getJsSerializableClass(
    Lock& js, v8::Local<v8::Object> object, v8::Local<v8::Value> mark) {
  // The mark of a cloneable instance is its class's descriptor.
  if (!mark->IsObject()) throwDataCloneErrorForObject(js, object);
  auto descriptor = mark.As<v8::Object>();
  for (auto& cls: jsSerializableClasses) {
    if (cls.descriptor.getHandle(js) == descriptor) return cls;
  }

  auto clone = KJ_UNWRAP_OR(
      tryGetOwnDataProperty(js, descriptor, "clone"_kj), throwDataCloneErrorForObject(js, object));
  auto deserializeInfo = KJ_UNWRAP_OR(tryGetOwnDataProperty(js, descriptor, "deserializeInfo"_kj),
      throwDataCloneErrorForObject(js, object));
  if (!clone.value->IsFunction() || !deserializeInfo.value->IsString() || !clone.isConstant(js) ||
      !deserializeInfo.isConstant(js)) {
    throwDataCloneErrorForObject(js, object);
  }
  return jsSerializableClasses.add(JsSerializableClass{
    .descriptor = js.v8Ref(descriptor),
    .clone = js.v8Ref(clone.value.As<v8::Function>()),
    .deserializeInfo = JsString(deserializeInfo.value.As<v8::String>()).toString(js),
  });
}

Serializer::Released Serializer::release() {
  KJ_ASSERT(!released, "The data has already been released.");
  KJ_ASSERT(pendingJsSerializableData.empty());
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
  for (n = 0; n < arrayBuffers.size(); n++) {
    // If the ArrayBuffer has already been added, we do not want to try adding it again.
    if (arrayBuffers[n].getHandle(js) == value) {
      return;
    }
  }
  arrayBuffers.add(jsg::JsRef(js, value));

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
  ++writeDepth;
  KJ_DEFER(--writeDepth);
  KJ_ON_SCOPE_FAILURE(if (writeDepth == 1) pendingJsSerializableData.clear());
  KJ_ASSERT(check(ser.WriteValue(js.v8Context(), value)));

  // A top-level value is followed by the data of the instances of classes implemented in
  // JavaScript that it contains, in the order they were written. Writing one's data may add more.
  if (writeDepth == 1) {
    for (size_t i = 0; i < pendingJsSerializableData.size(); i++) {
      auto data = pendingJsSerializableData[i].getHandle(js);
      KJ_ASSERT(check(ser.WriteValue(js.v8Context(), data)));
    }
    pendingJsSerializableData.clear();
  }
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
  ++readDepth;
  KJ_DEFER(--readDepth);
  KJ_DEFER(if (readDepth == 1) jsSerializableClasses.clear());
  KJ_ON_SCOPE_FAILURE(if (readDepth == 1) pendingJsSerializables.clear());
  auto value = [&]() {
    js.setDisallowJavascriptExecution(true);
    KJ_DEFER(js.setDisallowJavascriptExecution(false));
    return JsValue(check(deser.ReadValue(js.v8Context())));
  }();
  if (readDepth == 1 && !pendingJsSerializables.empty()) {
    finishJsSerializables(js);
  }
  return value;
}

uint Deserializer::readRawUint32() {
  uint32_t result;
  JSG_REQUIRE(deser.ReadUint32(&result), DOMDataCloneError, MALFORMED_DATA_MESSAGE);
  return result;
}

uint64_t Deserializer::readRawUint64() {
  uint64_t result;
  JSG_REQUIRE(deser.ReadUint64(&result), DOMDataCloneError, MALFORMED_DATA_MESSAGE);
  return result;
}

kj::ArrayPtr<const kj::byte> Deserializer::readRawBytes(size_t size) {
  const void* data;
  JSG_REQUIRE(deser.ReadRawBytes(size, &data), DOMDataCloneError, MALFORMED_DATA_MESSAGE);
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
  // V8 reports the data as malformed if this returns nothing, as for an id that names no buffer.
  KJ_IF_SOME(backingStores, sharedBackingStores) {
    if (clone_id < backingStores.size()) {
      return v8::SharedArrayBuffer::New(isolate, backingStores[clone_id]);
    }
  }
  return v8::MaybeLocal<v8::SharedArrayBuffer>();
}

v8::MaybeLocal<v8::Object> Deserializer::ReadHostObject(v8::Isolate* isolate) {
  try {
    uint tag = readRawUint32();

    if (tag == SERIALIZATION_TAG_NATIVE_ERROR) {
      auto& js = Lock::from(isolate);
      auto stack = js.str("stack"_kj);

      // The first uint32_t is the error tag, which identifies the type of error.
      auto errorTag = static_cast<ErrorTag>(readRawUint32());
      // If The error tag is UNKNOWN, we will read the name of the error next.
      // If the error is known, we don't both serializing the name.
      kj::Maybe<JsValue> maybeName;
      if (errorTag == ErrorTag::UNKNOWN) {
        maybeName = readValue(js);
      }

      // The next value is the message, which is always present.
      auto messageValue = readValue(js);
      auto messageStr = KJ_UNWRAP_OR(messageValue.tryCast<JsString>(), {
        JSG_FAIL_REQUIRE(
            DOMDataCloneError, "Deserialization failed: error message is not a string");
      });
      // Now let's create the error object based on the tag and message.
      auto obj = toJsError(js, errorTag, messageStr);

      // If we have a name, we set it on the error object. This is not
      // perfect but it gets close enough. Specifically, when the error
      // was serialized, if the user has modified the name or created
      // their own subclass, then we end up having to create just a
      // regular error here and change the name. It is not possible
      // for us here to clone the exact error class that was used,
      // so instanceof checks will not work as expected. But, that's ok.
      KJ_IF_SOME(name, maybeName) {
        // We use defineProperty here since the name is not typically
        // modifiable with set() on error objects.
        obj.defineProperty(js, "name"_kj, name);
      }

      // Now let's read the remaining properties... They were serialized as
      // a plain object with some own properties.
      KJ_IF_SOME(serObj, readValue(js).tryCast<JsObject>()) {
        auto names = serObj.getPropertyNames(js, KeyCollectionFilter::OWN_ONLY,
            PropertyFilter::ALL_PROPERTIES, IndexFilter::SKIP_INDICES);
        for (size_t n = 0; n < names.size(); n++) {
          auto name = names.get(js, n);
          // If the preserveStackInErrors option is false, then we will not
          // restore the serialized stack property if it is included in the
          // serialized output.
          if (!preserveStackInErrors && name.strictEquals(stack)) continue;
          auto value = serObj.get(js, name);
          // Use createDataProperty instead of ordinary set to avoid
          // invoking prototype-chain setters. This code runs inside V8's
          // DisallowJavascriptExecution scope; an ordinary Set() that hits
          // a tenant-installed Error.prototype setter would trigger
          // V8_Fatal -> abort(). CreateDataProperty defines an own data
          // property directly, matching the HTML structured-clone spec.
          obj.createDataProperty(js, name, value);
        }
      }

      v8::Local<v8::Object> ret = obj;
      return ret;
    }

    if (tag == SERIALIZATION_TAG_JS_SERIALIZABLE) {
      return readJsSerializable(Lock::from(isolate));
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
  // A Deserializer reading for another delegate cannot finish such an instance after the
  // top-level value, which that delegate reads.
  JSG_REQUIRE(tag != SERIALIZATION_TAG_JS_SERIALIZABLE, DOMDataCloneError,
      "This deserializer does not support objects implemented in JavaScript.");
  KJ_IF_SOME(result, IsolateBase::from(js.v8Isolate).deserialize(js, tag, *this)) {
    return result;
  } else {
    // Unknown tag is a platform error, so use KJ assert.
    KJ_FAIL_ASSERT("encountered unknown tag in deserialization", tag);
  }
}

kj::Maybe<BootstrapModules&> tryGetBootstrapModules(Lock& js) {
  return getAlignedPointerFromEmbedderData<BootstrapModules>(
      js.v8Context(), ContextPointerSlot::BOOTSTRAP_STATE);
}

size_t Deserializer::findJsSerializableClass(Lock& js, kj::String deserializeInfo) {
  for (auto i: kj::indices(jsSerializableClasses)) {
    if (jsSerializableClasses[i].deserializeInfo == deserializeInfo) return i;
  }
  auto unknownClass = [&]() -> size_t {
    JSG_FAIL_REQUIRE(
        DOMDataCloneError, "Deserialization failed: unknown class implemented in JavaScript.");
  };

  kj::StringPtr info = deserializeInfo;
  auto colon = KJ_UNWRAP_OR(info.findFirst(':'), return unknownClass());
  auto specifier = kj::str(info.first(colon));
  auto exportName = info.slice(colon + 1);

  auto& modules = KJ_UNWRAP_OR(tryGetBootstrapModules(js), return unknownClass());
  auto exports = KJ_UNWRAP_OR(modules.tryGetLoadedExports(js, specifier), return unknownClass());
  auto exportsObject = KJ_UNWRAP_OR(exports.tryCast<JsObject>(), return unknownClass());
  auto ctor = KJ_UNWRAP_OR(tryGetOwnProperty(js, exportsObject, exportName), return unknownClass());
  if (!ctor->IsFunction()) return unknownClass();
  auto prototype = KJ_UNWRAP_OR(
      tryGetOwnProperty(js, ctor.As<v8::Object>(), "prototype"_kj), return unknownClass());
  if (!prototype->IsObject()) return unknownClass();

  // Undefined if the class did not opt in.
  auto descriptor = KJ_UNWRAP_OR(
      JsObject(prototype.As<v8::Object>()).getPrivate(js, JS_CLONEABLE_KEY).tryCast<JsObject>(),
      return unknownClass());
  auto deserialize =
      KJ_UNWRAP_OR(tryGetOwnProperty(js, descriptor, "deserialize"_kj), return unknownClass());
  if (!deserialize->IsFunction()) return unknownClass();

  if (errorPrototype == kj::none) {
    errorPrototype = js.v8Ref(IsolateBase::getErrorPrototype(js.v8Isolate, js.v8Context()));
  }
  v8::Local<v8::Value> errorProto = KJ_ASSERT_NONNULL(errorPrototype).getHandle(js);
  // The bootstrap's DOMException.prototype inherits from Error.prototype, so it is found first.
  auto kind = JsSerializableClass::Kind::OBJECT;
  for (auto current = prototype; current->IsObject();
       current = current.As<v8::Object>()->GetPrototype()) {
    if (JsObject(current.As<v8::Object>()).hasPrivate(js, DOM_EXCEPTION_PROTOTYPE_KEY)) {
      kind = JsSerializableClass::Kind::DOM_EXCEPTION;
      break;
    }
    if (current == errorProto) {
      kind = JsSerializableClass::Kind::NATIVE_ERROR;
      break;
    }
  }

  jsSerializableClasses.add(JsSerializableClass{
    .deserializeInfo = kj::mv(deserializeInfo),
    .prototype = js.v8Ref(prototype.As<v8::Object>()),
    .descriptor = js.v8Ref(v8::Local<v8::Object>(descriptor)),
    .deserialize = js.v8Ref(deserialize.As<v8::Function>()),
    .kind = kind,
  });
  return jsSerializableClasses.size() - 1;
}

v8::Local<v8::Object> Deserializer::readJsSerializable(Lock& js) {
  // Runs no JavaScript: V8 forbids it while it reads a value. The data may come from an untrusted
  // source, so whatever it names is checked to be a class that opted into the protocol.
  auto deserializeInfo = readLengthDelimitedString();
  auto flags = readRawUint32();
  JSG_REQUIRE((flags & ~JS_SERIALIZABLE_KNOWN_FLAGS) == 0, DOMDataCloneError,
      "Deserialization failed: unsupported object implemented in JavaScript.");
  kj::Maybe<v8::Local<v8::String>> message;
  if (flags & JS_SERIALIZABLE_HAS_MESSAGE) {
    auto value = KJ_UNWRAP_OR(readValue(js).tryCast<JsString>(), {
      JSG_FAIL_REQUIRE(DOMDataCloneError,
          "Deserialization failed: message of an object implemented in JavaScript is not a "
          "string.");
    });
    message = v8::Local<v8::String>(value);
  }
  kj::Maybe<v8::Local<v8::String>> stack;
  if (flags & JS_SERIALIZABLE_HAS_STACK) {
    auto value = KJ_UNWRAP_OR(readValue(js).tryCast<JsString>(), {
      JSG_FAIL_REQUIRE(DOMDataCloneError,
          "Deserialization failed: stack of an object implemented in JavaScript is not a "
          "string.");
    });
    stack = v8::Local<v8::String>(value);
  }
  auto classIndex = findJsSerializableClass(js, kj::mv(deserializeInfo));
  auto& cls = jsSerializableClasses[classIndex];
  v8::Local<v8::Object> prototype = cls.prototype.getHandle(js);

  // The instance is what calling the constructor would make, minus whatever the constructor does:
  // `deserialize` initializes it. A class that inherits from Error makes errors, whose stack is the
  // writer's, as for a native error, or else the one captured now. Either way it is not enumerable.
  auto context = js.v8Context();
  kj::Maybe<v8::Local<v8::String>> keptStack;
  if (preserveStackInErrors || (flags & JS_SERIALIZABLE_KEEP_STACK)) keptStack = stack;
  v8::Local<v8::Object> instance;
  switch (cls.kind) {
    case JsSerializableClass::Kind::DOM_EXCEPTION:
      // V8 formats the stack's header lazily, from the name and message that `deserialize` sets.
      instance = newUninitializedBootstrapDomException(js, JsObject(prototype),
          keptStack.map([](v8::Local<v8::String> s) { return JsString(s); }));
      break;
    case JsSerializableClass::Kind::NATIVE_ERROR:
      // The message is what the stack just captured begins with, after the error's name, as for a
      // native error.
      instance =
          v8::Exception::Error(message.orDefault(v8::String::Empty(js.v8Isolate))).As<v8::Object>();
      // Error() always gives the error an own `message`, which a class's instance does not have
      // until it is initialized.
      check(instance->Delete(context, js.strIntern("message"_kj)));
      check(instance->SetPrototype(context, prototype));
      KJ_IF_SOME(s, keptStack) {
        v8::Local<v8::String> stackKey = js.strIntern("stack"_kj);
        check(instance->DefineOwnProperty(context, stackKey, s, v8::DontEnum));
      }
      break;
    case JsSerializableClass::Kind::OBJECT:
      // v8::Object::New() given a prototype makes an object in dictionary mode, slow to use. One
      // given a new prototype, as by Object.setPrototypeOf(), keeps fast properties.
      instance = v8::Object::New(js.v8Isolate);
      check(instance->SetPrototype(context, prototype));
      break;
  }
  // It was cloneable when written, and so is the copy.
  check(instance->SetPrivate(
      context, IsolateBase::getJsSerializeModeKey(js.v8Isolate), cls.descriptor.getHandle(js)));

  pendingJsSerializables.add(PendingJsSerializable{
    .instance = js.v8Ref(instance),
    .classIndex = classIndex,
  });
  return instance;
}

void Deserializer::finishJsSerializables(Lock& js) {
  auto context = js.v8Context();

  // Each instance's data follows the top-level value, in the order the instances were read.
  // Reading one's data may read more instances.
  kj::Vector<V8Ref<v8::Value>> data(pendingJsSerializables.size());
  {
    js.setDisallowJavascriptExecution(true);
    KJ_DEFER(js.setDisallowJavascriptExecution(false));
    for (size_t i = 0; i < pendingJsSerializables.size(); i++) {
      data.add(js.v8Ref(check(deser.ReadValue(context))));
    }
  }

  // Initializing the instances runs JavaScript, which a caller may have disallowed.
  JSG_REQUIRE(!js.isJavascriptExecutionDisallowed(), DOMDataCloneError,
      "Objects implemented in JavaScript cannot be deserialized here.");
  auto pending = kj::mv(pendingJsSerializables);
  for (auto i: kj::indices(pending)) {
    v8::Local<v8::Function> deserialize =
        jsSerializableClasses[pending[i].classIndex].deserialize.getHandle(js);
    v8::Local<v8::Value> args[] = {pending[i].instance.getHandle(js), data[i].getHandle(js)};
    check(deserialize->Call(context, js.undefined(), kj::size(args), args));
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

v8::MaybeLocal<v8::Value> newDataCloneError(v8::Isolate* isolate, v8::Local<v8::String> message) {
  v8::Local<v8::Object> exception;
  if (!newDOMException(isolate, "DataCloneError"_kj, message).ToLocal(&exception)) return {};
  return exception;
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
