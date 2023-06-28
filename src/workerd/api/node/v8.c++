#include "v8.h"
#include <workerd/jsg/dom-exception.h>

namespace workerd::api::node {

class SerializerHandle::Delegate: public v8::ValueSerializer::Delegate {
public:
  Delegate(v8::Isolate* isolate, SerializerHandle& handle)
      : isolate(isolate),
        handle(handle) {}
  ~Delegate() override = default;

  void ThrowDataCloneError(v8::Local<v8::String> message) override {
    isolate->ThrowException(jsg::makeDOMException(isolate, message, "DataCloneError"));
  }

  v8::Maybe<bool> WriteHostObject(
      v8::Isolate* isolate,
      v8::Local<v8::Object> object) override {
    auto& js = jsg::Lock::from(isolate);
    KJ_IF_MAYBE(maybeFn, handle.delegate.get(js, kj::str("_writeHostObject"))) {
      KJ_IF_MAYBE(fn, *maybeFn) {
        return v8::Just((*fn)(js, object).getHandle(js)->BooleanValue(isolate));
      }
    }
    return v8::ValueSerializer::Delegate::WriteHostObject(isolate, object);
  }

  v8::Maybe<uint32_t> GetSharedArrayBufferId(
      v8::Isolate* isolate,
      v8::Local<v8::SharedArrayBuffer> sab) override {
    auto& js = jsg::Lock::from(isolate);
    KJ_IF_MAYBE(maybeFn, handle.delegate.get(js, kj::str("_getSharedArrayBufferId"))) {
      KJ_IF_MAYBE(fn, *maybeFn) {
        return v8::Just(jsg::check((*fn)(js, sab).getHandle(js)->Uint32Value(js.v8Context())));
      }
    }
    return v8::ValueSerializer::Delegate::GetSharedArrayBufferId(isolate, sab);
  }

private:
  v8::Isolate* isolate;
  SerializerHandle& handle;
};

class DeserializerHandle::Delegate: public v8::ValueDeserializer::Delegate {
public:
  Delegate(DeserializerHandle& handle): handle(handle) {}

  v8::MaybeLocal<v8::Object> ReadHostObject(v8::Isolate* isolate) override {
    auto& js = jsg::Lock::from(isolate);
    v8::Isolate::AllowJavascriptExecutionScope allow_js(isolate);
    KJ_IF_MAYBE(maybeFn, handle.delegate.get(js, kj::str("_readHostObject"))) {
      KJ_IF_MAYBE(fn, *maybeFn) {
        auto handle = (*fn)(js).getHandle(js);
        JSG_REQUIRE(handle->IsObject(), TypeError, "_readHostObject must return an object");
        return v8::MaybeLocal<v8::Object>(handle.As<v8::Object>());
      }
    }
    return v8::ValueDeserializer::Delegate::ReadHostObject(js.v8Isolate);
  }

private:
  DeserializerHandle& handle;
};

SerializerHandle::SerializerHandle(jsg::Lock& js)
    : inner(kj::heap<SerializerHandle::Delegate>(js.v8Isolate, *this)),
      ser(js.v8Isolate, inner.get()) {}

jsg::Ref<SerializerHandle> SerializerHandle::constructor(jsg::Lock& js) {
  return jsg::alloc<SerializerHandle>(js);
}

void SerializerHandle::writeHeader() {
  ser.WriteHeader();
}

bool SerializerHandle::writeValue(jsg::Lock& js, jsg::Value value) {
  return jsg::check(ser.WriteValue(js.v8Context(), value.getHandle(js)));
}

kj::Array<kj::byte> SerializerHandle::releaseBuffer() {
  auto pair = ser.Release();
  return kj::Array(pair.first, pair.second, jsg::SERIALIZED_BUFFER_DISPOSER);
}

void SerializerHandle::transferArrayBuffer(jsg::Lock& js, uint32_t number,
                                           jsg::V8Ref<v8::Object> buf) {
  auto handle = buf.getHandle(js);
  JSG_REQUIRE(handle->IsArrayBuffer(), TypeError, "buffer must be an ArrayBuffer");
  ser.TransferArrayBuffer(number, handle.As<v8::ArrayBuffer>());
}

void SerializerHandle::writeUint32(uint32_t value) {
  ser.WriteUint32(value);
}

void SerializerHandle::writeUint64(uint32_t hi, uint32_t lo) {
  uint64_t hi64 = hi;
  uint64_t lo64 = lo;
  ser.WriteUint64((hi64 << 32) | lo64);
}

void SerializerHandle::writeDouble(double value) {
  ser.WriteDouble(value);
}

void SerializerHandle::writeRawBytes(jsg::BufferSource source) {
  kj::ArrayPtr<kj::byte> ptr = source.asArrayPtr();
  ser.WriteRawBytes(ptr.begin(), ptr.size());
}

void SerializerHandle::setTreatArrayBufferViewsAsHostObjects(bool flag) {
  ser.SetTreatArrayBufferViewsAsHostObjects(flag);
}

DeserializerHandle::DeserializerHandle(jsg::Lock& js, jsg::BufferSource source)
    : inner(kj::heap<Delegate>(*this)),
      buffer(kj::heapArray(source.asArrayPtr())),
      des(js.v8Isolate, buffer.begin(), buffer.size(), inner.get()) {}

jsg::Ref<DeserializerHandle> DeserializerHandle::constructor(jsg::Lock& js,
                                                             jsg::BufferSource source) {
  return jsg::alloc<DeserializerHandle>(js, kj::mv(source));
}

bool DeserializerHandle::readHeader(jsg::Lock& js) {
  return jsg::check(des.ReadHeader(js.v8Context()));
}

v8::Local<v8::Value> DeserializerHandle::readValue(jsg::Lock& js) {
  return jsg::check(des.ReadValue(js.v8Context()));
}

void DeserializerHandle::transferArrayBuffer(jsg::Lock& js, uint32_t id,
                                             jsg::V8Ref<v8::Object> ab) {
  auto handle = ab.getHandle(js);
  JSG_REQUIRE(handle->IsArrayBuffer() || handle->IsSharedArrayBuffer(), TypeError,
      "arrayBuffer must be an ArrayBuffer or SharedArrayBuffer");
  if (handle->IsArrayBuffer()) {
    des.TransferArrayBuffer(id, handle.As<v8::ArrayBuffer>());
    return;
  }
  des.TransferSharedArrayBuffer(id, handle.As<v8::SharedArrayBuffer>());
}

uint32_t DeserializerHandle::getWireFormatVersion() {
  return des.GetWireFormatVersion();
}

uint32_t DeserializerHandle::readUint32() {
  uint32_t value;
  JSG_REQUIRE(des.ReadUint32(&value), Error, "ReadUint32() failed");
  return value;
}

kj::Array<uint32_t> DeserializerHandle::readUint64() {
  uint64_t value;
  JSG_REQUIRE(des.ReadUint64(&value), Error, "ReadUint64() failed");
  auto ret = kj::heapArray<uint32_t>(2);
  ret[0] = static_cast<uint32_t>(value >> 32);
  ret[1] = static_cast<uint32_t>(value);
  return kj::mv(ret);
}

double DeserializerHandle::readDouble() {
  double value;
  JSG_REQUIRE(des.ReadDouble(&value), Error, "ReadDouble() failed");
  return value;
}

uint32_t DeserializerHandle::readRawBytes(uint64_t length) {
  const void* data;
  JSG_REQUIRE(des.ReadRawBytes(length, &data), Error, "ReadRawBytes() failed");
  const uint8_t* pos = reinterpret_cast<const uint8_t*>(data);
  return static_cast<uint32_t>(pos - buffer.begin());
}

}  // namespace workerd::api::node
