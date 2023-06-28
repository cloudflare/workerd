#pragma once

#include <workerd/jsg/jsg.h>
#include <workerd/jsg/buffersource.h>
#include <workerd/jsg/ser.h>
#include <v8-value-serializer-version.h>

namespace workerd::api::node {

static constexpr uint32_t kMaxSerializationVersion = v8::CurrentValueSerializerFormatVersion();
static constexpr uint32_t kMinSerializationVersion = 13;

class SerializerHandle final: public jsg::Object {
public:
  struct Options {
    jsg::Optional<uint32_t> version;
    JSG_STRUCT(version);
  };

  SerializerHandle(jsg::Lock& js, jsg::Optional<Options> options = nullptr);
  static jsg::Ref<SerializerHandle> constructor(jsg::Lock& js,
                                                jsg::Optional<Options> options);
  void writeHeader();
  bool writeValue(jsg::Lock& js, jsg::Value value);
  kj::Array<kj::byte> releaseBuffer();
  void transferArrayBuffer(jsg::Lock& js, uint32_t number, jsg::V8Ref<v8::Object> buf);
  void writeUint32(uint32_t value);
  void writeUint64(uint32_t hi, uint32_t lo);
  void writeDouble(double value);
  void writeRawBytes(jsg::BufferSource source);
  void setTreatArrayBufferViewsAsHostObjects(bool flag);

  JSG_RESOURCE_TYPE(SerializerHandle) {
    JSG_METHOD(writeHeader);
    JSG_METHOD(writeValue);
    JSG_METHOD(releaseBuffer);
    JSG_METHOD(transferArrayBuffer);
    JSG_METHOD(writeUint32);
    JSG_METHOD(writeUint64);
    JSG_METHOD(writeDouble);
    JSG_METHOD(writeRawBytes);
    JSG_METHOD(setTreatArrayBufferViewsAsHostObjects);
  }
  JSG_REFLECTION(delegate);

  using HostObjectDelegate = jsg::Value(v8::Local<v8::Object>);

private:
  class Delegate final: public v8::ValueSerializer::Delegate {
  public:
    Delegate(v8::Isolate* isolate, SerializerHandle& handle);
    ~Delegate() override = default;
    void ThrowDataCloneError(v8::Local<v8::String>) override;
    v8::Maybe<bool> WriteHostObject(v8::Isolate*, v8::Local<v8::Object>) override;
    v8::Maybe<uint32_t> GetSharedArrayBufferId(v8::Isolate*,
                                               v8::Local<v8::SharedArrayBuffer>) override;
  private:
    v8::Isolate* isolate;
    SerializerHandle& handle;
  };

  kj::Own<Delegate> inner;
  v8::ValueSerializer ser;
  jsg::PropertyReflection<jsg::Optional<jsg::Function<HostObjectDelegate>>> delegate;
  friend class Delegate;
};

class DeserializerHandle final: public jsg::Object {
public:
  struct Options {
    jsg::Optional<uint32_t> version;
    JSG_STRUCT(version);
  };

  DeserializerHandle(jsg::Lock& js,
                     jsg::BufferSource source,
                     jsg::Optional<Options> options = nullptr);
  static jsg::Ref<DeserializerHandle> constructor(jsg::Lock& js,
                                                  jsg::BufferSource source,
                                                  jsg::Optional<Options> options);
  bool readHeader(jsg::Lock& js);
  v8::Local<v8::Value> readValue(jsg::Lock& js);
  void transferArrayBuffer(jsg::Lock& js, uint32_t id, jsg::V8Ref<v8::Object> ab);
  uint32_t getWireFormatVersion();
  uint32_t readUint32();
  kj::Array<uint32_t> readUint64();
  double readDouble();
  uint32_t readRawBytes(uint64_t length);

  JSG_RESOURCE_TYPE(DeserializerHandle) {
    JSG_METHOD(readHeader);
    JSG_METHOD(readValue);
    JSG_METHOD(transferArrayBuffer);
    JSG_METHOD(getWireFormatVersion);
    JSG_METHOD(readUint32);
    JSG_METHOD(readUint64);
    JSG_METHOD(readDouble);
    JSG_METHOD(readRawBytes);
  }
  JSG_REFLECTION(delegate);

  using HostObjectDelegate = jsg::V8Ref<v8::Object>();

private:
  class Delegate final: public v8::ValueDeserializer::Delegate {
  public:
    Delegate(DeserializerHandle& handle);

    v8::MaybeLocal<v8::Object> ReadHostObject(v8::Isolate* isolate) override;

  private:
    DeserializerHandle& handle;
  };

  kj::Own<Delegate> inner;
  kj::Array<kj::byte> buffer;
  v8::ValueDeserializer des;

  jsg::PropertyReflection<jsg::Optional<jsg::Function<HostObjectDelegate>>> delegate;
  friend class Delegate;
};

class V8Module: public jsg::Object {
public:
  static constexpr auto MAX_SERIALIZATION_VERSION = kMaxSerializationVersion;
  static constexpr auto MIN_SERIALIZATION_VERSION = kMinSerializationVersion;

  JSG_RESOURCE_TYPE(V8Module) {
    JSG_NESTED_TYPE(SerializerHandle);
    JSG_NESTED_TYPE(DeserializerHandle);

    JSG_STATIC_CONSTANT(MAX_SERIALIZATION_VERSION);
    JSG_STATIC_CONSTANT(MIN_SERIALIZATION_VERSION);
  }
};

#define EW_NODE_V8_ISOLATE_TYPES            \
    api::node::SerializerHandle,            \
    api::node::SerializerHandle::Options,   \
    api::node::DeserializerHandle,          \
    api::node::DeserializerHandle::Options, \
    api::node::V8Module

}  // namespace workerd::api::node
