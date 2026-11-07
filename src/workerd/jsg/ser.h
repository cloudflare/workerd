// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/jsg/jsg.h>

#include <v8-value-serializer.h>

#include <kj/vector.h>

namespace workerd::jsg {

// The serialization tag of an instance of a class implemented in JavaScript (see Serializer).
// Keep in sync with jsSerializable in worker-interface.capnp.
constexpr uint32_t SERIALIZATION_TAG_JS_SERIALIZABLE = 23;

// The names of the V8 private properties of the protocol by which classes implemented in
// JavaScript are serialized (see Serializer). src/per_isolate/serialization.ts uses the same names.
constexpr kj::StringPtr JS_SERIALIZE_MODE_KEY = "workerd:serializeMode"_kj;
constexpr kj::StringPtr JS_CLONEABLE_KEY = "workerd:cloneable"_kj;

// The runtime's internal JavaScript modules loaded in a context (the per-isolate bootstrap's, see
// api/per-isolate-bootstrap.h), which the deserializer looks up classes implemented in JavaScript
// among. Stored in the context's ContextPointerSlot::BOOTSTRAP_STATE slot.
class BootstrapModules {
 public:
  // Returns the exports of the module named `specifier` if it has been loaded, or kj::none. Must
  // not run JavaScript.
  virtual kj::Maybe<JsValue> tryGetLoadedExports(Lock& js, kj::StringPtr specifier) = 0;

 protected:
  ~BootstrapModules() noexcept(false) = default;
};

// Returns the BootstrapModules of the current context, if any.
kj::Maybe<BootstrapModules&> tryGetBootstrapModules(Lock& js);

// Wraps the v8::ValueSerializer and v8::ValueSerializer::Delegate implementation.
// Must be allocated on the stack, and requires that a v8::HandleScope exist in
// the stack.
//
// To declare a JSG_RESOURCE_TYPE as serializeable, you must declare two special methods
// `serialize()` and `deserialize()`, and also use the JSG_SERIALIZABLE macro, which must appear
// after the `JSG_RESOURCE_TYPE` block (NOT inside it). Example:
//
//     class Foo: public jsg::Object {
//     public:
//       // ...
//
//       JSG_RESOURCE_TYPE(Foo) {
//         // ...
//       }
//
//       void serialize(jsg::Lock& js, jsg::Serializer& serializer);
//       static jsg::Ref<Foo> deserialize(Lock& js, MyTag tag, Deserializer& deserializer);
//       JSG_SERIALIZABLE(MyTag::FOO_V2, MyTag::FOO_V1);
//
//       // ...
//     };
//
// * `MyTag` is some enum type declared in the application which enumerates all known serializeable
//   types. This can be any enum, but it is suggested that all types in the application use the
//   same enum type, and the numeric values of the tags must never change. A Cap'n Proto enum is
//   suggested.
// * `JSG_SERIALIZABLE`'s parameters are a list of tags that encode this type. There may be more
//   than one tag listed to support versioning. The first tag listed is the current version, which
//   is the format that `serialize()` will write. The other tags are non-current versions that
//   `deserialize()` accepts in addition to the current version. These are usually old versions,
//   but could also include a new version that hasn't fully rolled out yet -- it will be necessary
//   to fully roll out support for parsing a new version before anyone can start generating it.
// * The serialization system automatically handles writing and reading the tag values before
//   calling the methods.
// * serialize() makes a series of calls to serializer.write*() methods to write the content of the
//   object.
// * deserialize() makes a corresponding series of calls to deserializer.read*() methods to read
//   the object. These must be the exact corresponding calls in the same order as serialize()
//   would have made them. The sequence can never change once data has been written for a given
//   tag version; the only way to change is to define a new version.
// * Both `serialize()` and `deserialize()` can take additional arguments of the form
//   `const jsg::TypeHandler<SomeType>&`, which will automatically be filled in with the
//   corresponding type handler. This is useful if the serializer wants to, say, assemble a
//   JSG_STRUCT, convert it into an actual JS object, and serialize that.
//
// Classes implemented in JavaScript by the runtime itself (the per-isolate bootstrap's, see
// src/per_isolate/serialization.ts) take part through a protocol modelled on Node.js's
// js_transferable. Each part is a V8 private property, which only runtime code can name:
//
// * A cloneable class has a descriptor: an object with the own data properties `deserializeInfo`,
//   `clone` and `deserialize`. `deserializeInfo` is a string "module:Export" naming the class's
//   constructor among the exports of a loaded bootstrap module, which is how the deserializer finds
//   the class: there is no registry. Only an own data property counts, as an export or in a
//   descriptor: the deserializer can't call a getter, so it rejects an accessor. The deserializer
//   cannot load a module, so one that defines such a class must be loaded while the bootstrap
//   runs, whatever the compatibility flags.
//   `clone(instance)` returns the instance's data, any serializable value, and
//   `deserialize(instance, data)` initializes an instance from it. The class's constructor's
//   `prototype` holds the descriptor under JS_CLONEABLE_KEY.
// * An instance carries a mark under JS_SERIALIZE_MODE_KEY. If the instance is cloneable, the mark
//   is its class's descriptor, so the class that marked an instance decides how it is written,
//   whatever its prototype is now. Any other mark makes serializing it throw DataCloneError,
//   unless the ExternalHandler serializes it (trySerializeClassInstance()). An object whose
//   prototype is Object.prototype or null is not checked for the property, which keeps plain
//   objects cheap, so it is never an instance.
//
// An instance is written as SERIALIZATION_TAG_JS_SERIALIZABLE, its `deserializeInfo`, and a uint32
// of flags, followed by the instance's own `stack` if it is a native error that has one (flag 1).
// A reader rejects flags it doesn't know, so that later versions can add fields. The instance's
// `data` follows the top-level value (as in Node.js), so that it can refer back to the instance.
// V8 runs no JavaScript while it reads a value, so the deserializer creates each instance from its
// constructor's `prototype` without calling the constructor (as a native error if the prototype
// inherits from Error.prototype), marks it with the descriptor, and calls `deserialize`, in the
// order the instances were read, once the top-level value and every instance's `data` have been
// read. The data may come from an untrusted source, so `deserialize` validates it.
//
// The deserializer, not the class, decides whether a native error made this way keeps the writer's
// stack, as it does for a native error: with the Deserializer's `preserveStackInErrors` option, or
// whenever enhanced error serialization is off. Otherwise it keeps the stack captured when it was
// made. A class does not serialize `stack` itself.
//
// A `deserializeInfo` is part of the format, which persisted data depends on.
class Serializer final: v8::ValueSerializer::Delegate {
 public:
  // "Externals" are values which can be serialized, but refer to some external resource, rather
  // than being self-contained. The way externals are supported depends on the serialization
  // context: passing externals over RPC, for example, is completely different from storing them
  // to disk.
  //
  // A `Serializer` instance may have an `ExternalHandler` which can be used when serializing
  // externals. This type has no methods, but is meant to be subclassed. A host object which
  // represents an external and is trying to serialize itself should use dynamic_cast to try to
  // downcast `ExternalHandler` to any particular handler interface it supports. If the handler
  // doesn't implement any supported subclass, then serialization is not possible, and an
  // appropriate exception should be thrown.
  class ExternalHandler {
   public:
    // Tries to serialize a function as an external. The default implementation throws
    // DataCloneError.
    virtual void serializeFunction(
        jsg::Lock& js, jsg::Serializer& serializer, v8::Local<v8::Function> func);

    // Tries to serialize a proxy as an external. The default implementation throws
    // DataCloneError.
    //
    // TODO(cleanup): This is a bit of a hack to support an RpcTarget that is wrapped in a Proxy.
    //   For RpcTarget specifically, this works because inheriting RpcTarget is just a marker that
    //   opts into serializing by creating a stub pointing at the object -- we can create a stub
    //   pointing at the proxy instead. For any other type, serializing a Proxy probably isn't
    //   possible, since the serialization wouldn't actually capture the Proxy logic? But I'm
    //   not 100% certain of that. If we find other use cases in the future it may turn out that
    //   they call for a different design.
    virtual void serializeProxy(
        jsg::Lock& js, jsg::Serializer& serializer, v8::Local<v8::Proxy> proxy);

    // Offers the handler a class instance (an object whose prototype is not Object.prototype and
    // which is not a JSG-wrapped host object, or an instance of a class implemented in JavaScript,
    // whatever its mark) to serialize as an external. Returns true if the handler
    // recognized the object and serialized it; false to fall through to the default behavior
    // (DataCloneError, or the object's own serialization if it is cloneable). This is how objects
    // implemented in JavaScript (e.g. the TypeScript-implemented stream classes, which have no JSG
    // wrapper for the serializer to find) participate in external serialization: the handler
    // recognizes them by brand check.
    virtual bool trySerializeClassInstance(
        jsg::Lock& js, jsg::Serializer& serializer, v8::Local<v8::Object> object) {
      return false;
    }
  };

  struct Options {
    // When set, overrides the default wire format version with the one provided.
    kj::Maybe<uint32_t> version;
    // When set to true, the serialization header is not written to the output buffer.
    bool omitHeader = false;

    // The structured clone spec states that instances of classes are serialized as if they were
    // plain objects: their "own" properties are serialized, but the prototype is completely
    // ignored. Upon deserialization, the value is no longer class instance, it's just a plain
    // object. This is probably not useful behavior in any real use case, but that's what the spec
    // says.
    //
    // If this flag is true, we'll follow the spec. If it's false, then instances of classes
    // (i.e. objects which have a prototype which is not Object.prototype) will not be serializable
    // (they throw DataCloneError).
    //
    // TODO(someday): Perhaps we could create a framework for application-defined classes to define
    //   their own serializers. However, we would need to be extremely careful about this when
    //   deserializing data from a possibly-malicious source. Such serialization frameworks have
    //   a history of creating security bugs as people declare various classes serializable without
    //   fully thinking through what an attacker could do by sending them an instance of that class
    //   when it isn't expected. Probably, we just shouldn't support this over RPC at all. For DO
    //   storage, it could be OK since the application would only be deserializing objects it wrote
    //   itself, but it may not be worth it to support for only that use case.
    bool treatClassInstancesAsPlainObjects = true;

    // ExternalHandler, if any. Typically this would be allocated on the stack just before the
    // Serializer.
    kj::Maybe<ExternalHandler&> externalHandler;
  };

  struct Released {
    // The serialized data.
    kj::Array<kj::byte> data;

    // The allocated size of the buffer containing `data`.
    size_t dataCapacity;

    // All instances of SharedArrayBuffer seen during serialization. Pass these along to the
    // deserializer to achieve actual sharing of buffers.
    kj::Array<std::shared_ptr<v8::BackingStore>> sharedArrayBuffers;

    // All ArrayBuffers that were passed to `transfer()`.
    kj::Array<std::shared_ptr<v8::BackingStore>> transferredArrayBuffers;
  };

  explicit Serializer(Lock& js): Serializer(js, {}) {}
  explicit Serializer(Lock& js, Options options);
  inline ~Serializer() noexcept(true) {}  // noexcept(true) because Delegate's is noexcept

  KJ_DISALLOW_COPY_AND_MOVE(Serializer);

  kj::Maybe<ExternalHandler&> getExternalHandler() {
    return externalHandler;
  }

  // Write a value.
  //
  // You can call this multiple times to write multiple values, then call `readValue()` the same
  // number of times on the deserialization side.
  void write(Lock& js, const JsValue& value);

  // Implements the `transfer` option of `structuredClone()`. Pass each item in the transfer array
  // to this method before calling `write()`. This gives the Serializer permission to serialize
  // these values by detaching them (destroying the caller's handle) rather than make a copy. The
  // detached content will show up as part of `Released`, where it should then be delivered to the
  // Deserializer later.
  void transfer(Lock& js, const JsValue& value);

  Released release();

  void writeRawUint32(uint32_t i) {
    ser.WriteUint32(i);
  }
  void writeRawUint64(uint64_t i) {
    ser.WriteUint64(i);
  }

  void writeRawBytes(kj::ArrayPtr<const kj::byte> bytes) {
    ser.WriteRawBytes(bytes.begin(), bytes.size());
  }

  // Write a size followed by bytes.
  void writeLengthDelimited(kj::ArrayPtr<const kj::byte> bytes) {
    writeRawUint32(bytes.size());
    writeRawBytes(bytes);
  }
  void writeLengthDelimited(kj::StringPtr text) {
    writeLengthDelimited(text.asBytes());
  }

 private:
  // A Serializer that writes to `ser`, which another delegate drives; see writeApiObject().
  struct Borrowed {};
  Serializer(Lock& js, v8::ValueSerializer& ser, Borrowed);
  friend v8::Maybe<bool> writeApiObject(
      v8::Isolate* isolate, v8::ValueSerializer& ser, v8::Local<v8::Object> object);

  // Writes `object`, which is not a native error, through its type's serializer if it is a
  // serializable workerd API object, or else throws a DataCloneError.
  void writeApiObject(Lock& js, v8::Local<v8::Object> object);

  // Returns the JS_SERIALIZE_MODE_KEY mark of `object`, if it is an instance of a class
  // implemented in JavaScript.
  kj::Maybe<v8::Local<v8::Value>> tryGetJsSerializeMark(Lock& js, v8::Local<v8::Object> object);

  // Writes `object`, an instance of a class implemented in JavaScript with the given mark, and
  // queues its data to follow the top-level value.
  void writeJsSerializable(Lock& js, v8::Local<v8::Object> object, v8::Local<v8::Value> mark);

  // Throw a DataCloneError, complaining that the given object cannot be serialized. (This is
  // similar to ThrowDataCloneError() except that it formats the error message itself, and it
  // is expected to be called from KJ-ish code so it throws JsExceptionThrown rather than
  // returning.)
  [[noreturn]] void throwDataCloneErrorForObject(jsg::Lock& js, v8::Local<v8::Object> obj);

  // v8::ValueSerializer::Delegate implementation
  void ThrowDataCloneError(v8::Local<v8::String> message) override;
  bool HasCustomHostObject(v8::Isolate* isolate) override;
  v8::Maybe<bool> IsHostObject(v8::Isolate* isolate, v8::Local<v8::Object> object) override;
  v8::Maybe<bool> WriteHostObject(v8::Isolate* isolate, v8::Local<v8::Object> object) override;

  v8::Maybe<uint32_t> GetSharedArrayBufferId(
      v8::Isolate* isolate, v8::Local<v8::SharedArrayBuffer> sab) override;
  void* ReallocateBufferMemory(void* oldBuffer, size_t size, size_t* actualSize) override;

  kj::Maybe<ExternalHandler&> externalHandler;

  kj::Vector<jsg::JsRef<JsValue>> sharedArrayBuffers;
  kj::Vector<jsg::JsRef<JsValue>> arrayBuffers;
  // ArrayBuffers passed to transfer() that still need to be detached. We defer detaching until
  // release() (i.e. after write()) -- see transfer() for why.
  kj::Vector<jsg::V8Ref<v8::ArrayBuffer>> arrayBuffersToDetach;
  kj::Vector<std::shared_ptr<v8::BackingStore>> sharedBackingStores;
  kj::Vector<std::shared_ptr<v8::BackingStore>> backingStores;
  // The data of the instances of classes implemented in JavaScript written by the current
  // top-level write(), which it writes after the value.
  kj::Vector<V8Ref<v8::Value>> pendingJsSerializableData;
  // How many calls to write() are in progress: a type's serializer calls it for nested values.
  uint writeDepth = 0;
  bool released = false;
  size_t dataCapacity = 0;
  bool treatClassInstancesAsPlainObjects;
  bool treatErrorsAsHostObjects = false;

  // The prototype of `Object`, which IsHostObject() checks objects against. Empty for a Serializer
  // that writes to another delegate's serializer.
  v8::Local<v8::Value> prototypeOfObject;

  // The JS_SERIALIZE_MODE_KEY private symbol, which IsHostObject() checks class instances for.
  // Empty for a Serializer that writes to another delegate's serializer.
  v8::Local<v8::Private> jsSerializeModeKey;

  // The actual ValueSerializer. Note that it's important to define this last because its
  // constructor will call back to the Delegate, which is this object, so we hope that this object
  // is fully initialized before that point! Empty for a Serializer that writes to another
  // delegate's serializer.
  kj::Maybe<v8::ValueSerializer> ownedSer;
  v8::ValueSerializer& ser;
};

// Wraps the v8::ValueDeserializer and v8::ValueDeserializer::Delegate implementation.
// Must be allocated on the stack, and requires that a v8::HandleScope exist in
// the stack.
class Deserializer final: v8::ValueDeserializer::Delegate {
 public:
  // Exactly like Serializer::ExternalHandler, but for Deserializer.
  class ExternalHandler {
   public:
    virtual ~ExternalHandler() noexcept(false) = 0;
  };

  struct Options {
    kj::Maybe<uint32_t> version;
    bool readHeader = true;

    // When the enahnced error serialization feature is enabled, and we are deserializing
    // a serialized error, this option controls whether to include the serialized stack
    // property in the deserialized error, including an instance of a class implemented in
    // JavaScript that inherits from Error. If false, the stack property is not restored
    // and will instead be set to the captured stack at the time of deserialization.
    // This flag has no effect if the enhanced error serialization feature is disabled,
    // or the values being deserialized are not errors (or do not contain any error objects).
    bool preserveStackInErrors = true;

    // ExternalHandler, if any. Typically this would be allocated on the stack just before the
    // Deserializer.
    kj::Maybe<ExternalHandler&> externalHandler;

    // Label header-error diagnostics with the runtime call site (defaults to "unknown").
    // Use a fixed label, never user-controlled data: it appears in logs and the internal exception.
    kj::Maybe<kj::StringPtr> diagnosticContext;
  };

  explicit Deserializer(Lock& js,
      kj::ArrayPtr<const kj::byte> data,
      kj::Maybe<kj::ArrayPtr<std::shared_ptr<v8::BackingStore>>> transferredArrayBuffers = kj::none,
      kj::Maybe<kj::ArrayPtr<std::shared_ptr<v8::BackingStore>>> sharedArrayBuffers = kj::none,
      kj::Maybe<Options> maybeOptions = kj::none);

  explicit Deserializer(
      Lock& js, Serializer::Released& released, kj::Maybe<Options> maybeOptions = kj::none);

  ~Deserializer() noexcept(true) {}  // noexcept(true) because Delegate's is noexcept

  KJ_DISALLOW_COPY_AND_MOVE(Deserializer);

  kj::Maybe<ExternalHandler&> getExternalHandler() {
    return externalHandler;
  }

  JsValue readValue(Lock& js);

  uint32_t readRawUint32();
  uint64_t readRawUint64();

  // Returns a view directly into the original buffer for the number of bytes requested. Always
  // returns the exact amount; throws if not possible.
  kj::ArrayPtr<const kj::byte> readRawBytes(size_t size);

  // Reads a size (readRawUint64) followed by that many bytes.
  kj::ArrayPtr<const kj::byte> readLengthDelimitedBytes();

  // Read a string and make a copy. The copy is necessary since the text is not NUL-terminated on
  // the wire. If you don't need NUL termination, read bytes and use `.asChars()`.
  kj::String readRawString(size_t size);
  kj::String readLengthDelimitedString();

  inline uint32_t getVersion() const {
    return deser.GetWireFormatVersion();
  }

 private:
  // A Deserializer that reads from `deser`, which another delegate drives; see readApiObject().
  struct Borrowed {};
  Deserializer(Lock& js, v8::ValueDeserializer& deser, Borrowed);
  friend v8::MaybeLocal<v8::Object> readApiObject(
      v8::Isolate* isolate, v8::ValueDeserializer& deser, uint32_t tag);

  // Reads the workerd API object that `tag` names through its type's deserializer.
  v8::Local<v8::Object> readApiObject(Lock& js, uint32_t tag);

  // Reads an instance of a class implemented in JavaScript, which is created at once and
  // initialized by finishJsSerializables().
  v8::Local<v8::Object> readJsSerializable(Lock& js);

  // Reads the data of the instances that readJsSerializable() created during the current
  // top-level readValue(), then initializes them with their classes' `deserialize` functions.
  void finishJsSerializables(Lock& js);

  void init(Lock& js,
      kj::ArrayPtr<const kj::byte> data,
      kj::Maybe<kj::ArrayPtr<std::shared_ptr<v8::BackingStore>>> transferredArrayBuffers = kj::none,
      kj::Maybe<Options> maybeOptions = kj::none);

  v8::MaybeLocal<v8::SharedArrayBuffer> GetSharedArrayBufferFromId(
      v8::Isolate* isolate, uint32_t clone_id) override;
  v8::MaybeLocal<v8::Object> ReadHostObject(v8::Isolate* isolate) override;

  kj::Maybe<ExternalHandler&> externalHandler;

  size_t totalInputSize;
  // Empty for a Deserializer that reads from another delegate's deserializer.
  kj::Maybe<v8::ValueDeserializer> ownedDeser;
  v8::ValueDeserializer& deser;
  kj::Maybe<kj::ArrayPtr<std::shared_ptr<v8::BackingStore>>> sharedBackingStores;
  bool preserveStackInErrors = true;

  struct PendingJsSerializable {
    V8Ref<v8::Object> instance;
    V8Ref<v8::Function> deserialize;
  };
  // The instances of classes implemented in JavaScript read by the current top-level readValue(),
  // in the order they were read.
  kj::Vector<PendingJsSerializable> pendingJsSerializables;
  // How many calls to readValue() are in progress: a type's deserializer calls it for nested
  // values.
  uint readDepth = 0;
  // Error.prototype of the current context, once readJsSerializable() needs it.
  kj::Maybe<V8Ref<v8::Value>> errorPrototype;
};

// For serializers implemented outside C++ JSG, which drive a v8::ValueSerializer or
// v8::ValueDeserializer with a delegate of their own but write workerd API objects in the same
// format as Serializer and Deserializer. Each returns an empty result, leaving the exception
// pending, if what it runs throws.

// Writes `object` as Serializer's WriteHostObject() writes an object that is not a native error:
// through its type's serializer if it is a serializable workerd API object, or else throws a
// DataCloneError. Values the type's serializer writes go through `ser`, and so through its
// delegate.
v8::Maybe<bool> writeApiObject(
    v8::Isolate* isolate, v8::ValueSerializer& ser, v8::Local<v8::Object> object);

// Reads the workerd API object that `tag` names, as Deserializer's ReadHostObject() does once it
// has read a tag other than a native error's. JavaScript execution is disallowed meanwhile.
v8::MaybeLocal<v8::Object> readApiObject(
    v8::Isolate* isolate, v8::ValueDeserializer& deser, uint32_t tag);

// Creates the DOMException named DataCloneError with `message` that Serializer's
// ThrowDataCloneError() throws.
v8::MaybeLocal<v8::Value> newDataCloneError(v8::Isolate* isolate, v8::Local<v8::String> message);

// Intended for use with v8::ValueSerializer data released into a kj::Array.
class SerializedBufferDisposer: public kj::ArrayDisposer {
 protected:
  void disposeImpl(void* firstElement,
      size_t elementSize,
      size_t elementCount,
      size_t capacity,
      void (*destroyElement)(void*)) const override;
};
constexpr SerializedBufferDisposer SERIALIZED_BUFFER_DISPOSER;

JsValue structuredClone(
    Lock& js, const JsValue& value, kj::Maybe<kj::Array<JsValue>> maybeTransfer = kj::none);

}  // namespace workerd::jsg
