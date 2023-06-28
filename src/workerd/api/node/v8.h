#pragma once

#include <workerd/jsg/jsg.h>
#include <workerd/jsg/buffersource.h>
#include <workerd/jsg/ser.h>

namespace workerd::api::node {

class SerializerHandle final: public jsg::Object {
public:
  SerializerHandle(jsg::Lock& js);
  static jsg::Ref<SerializerHandle> constructor(jsg::Lock& js);
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
  class Delegate;
  kj::Own<Delegate> inner;
  v8::ValueSerializer ser;
  jsg::PropertyReflection<jsg::Optional<jsg::Function<HostObjectDelegate>>> delegate;
  friend class Delegate;
};

class DeserializerHandle final: public jsg::Object {
public:
  DeserializerHandle(jsg::Lock& js, jsg::BufferSource source);
  static jsg::Ref<DeserializerHandle> constructor(jsg::Lock& js, jsg::BufferSource source);
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
  class Delegate;
  kj::Own<Delegate> inner;
  kj::Array<kj::byte> buffer;
  v8::ValueDeserializer des;

  jsg::PropertyReflection<jsg::Optional<jsg::Function<HostObjectDelegate>>> delegate;
  friend class Delegate;
};

class V8Module: public jsg::Object {
public:
  JSG_RESOURCE_TYPE(V8Module) {
    JSG_NESTED_TYPE(SerializerHandle);
    JSG_NESTED_TYPE(DeserializerHandle);
  }
};

#define EW_NODE_V8_ISOLATE_TYPES        \
    api::node::SerializerHandle,        \
    api::node::DeserializerHandle,      \
    api::node::V8Module

}  // namespace workerd::api::node
