// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/jsg/jsg.h>

#include <kj/vector.h>

namespace workerd::jsg {

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

  kj::Maybe<ExternalHandler&> externalHandler;

  kj::Vector<JsValue> sharedArrayBuffers;
  kj::Vector<JsValue> arrayBuffers;
  kj::Vector<std::shared_ptr<v8::BackingStore>> sharedBackingStores;
  kj::Vector<std::shared_ptr<v8::BackingStore>> backingStores;
  bool released = false;
  bool treatClassInstancesAsPlainObjects;

  // Initialized to point at the prototype of `Object` if and only if
  // `treatClassInstancesAsPlainObjects` is false (in which case we will need to check against this
  // prototype in IsHostObject()).
  v8::Local<v8::Value> prototypeOfObject;

  // The actual ValueSerializer. Note that it's important to define this last because its
  // constructor will call back to the Delegate, which is this object, so we hope that this object
  // is fully initialized before that point!
  v8::ValueSerializer ser;
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

    // ExternalHandler, if any. Typically this would be allocated on the stack just before the
    // Deserializer.
    kj::Maybe<ExternalHandler&> externalHandler;
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
  void init(Lock& js,
      kj::Maybe<kj::ArrayPtr<std::shared_ptr<v8::BackingStore>>> transferredArrayBuffers = kj::none,
      kj::Maybe<Options> maybeOptions = kj::none);

  v8::MaybeLocal<v8::SharedArrayBuffer> GetSharedArrayBufferFromId(
      v8::Isolate* isolate, uint32_t clone_id) override;
  v8::MaybeLocal<v8::Object> ReadHostObject(v8::Isolate* isolate) override;

  kj::Maybe<ExternalHandler&> externalHandler;

  size_t totalInputSize;
  v8::ValueDeserializer deser;
  kj::Maybe<kj::ArrayPtr<std::shared_ptr<v8::BackingStore>>> sharedBackingStores;
};

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
