// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "dom-exception.h"
#include "ser.h"
#include <workerd/jsg/memory.h>
#include <kj/string.h>
#include <map>

namespace workerd::jsg {

Ref<DOMException> DOMException::constructor(
    Lock& js,
    Optional<kj::String> message,
    Optional<kj::String> name) {
  kj::String errMessage = kj::mv(message).orDefault([&] { return kj::String(); });
  return jsg::alloc<DOMException>(
      kj::mv(errMessage),
      kj::mv(name).orDefault([] { return kj::str("Error"); }),
      js.v8Ref(v8::Exception::Error(v8Str(js.v8Isolate, errMessage)).As<v8::Object>()));
}

kj::StringPtr DOMException::getName() {
  return name;
}

kj::StringPtr DOMException::getMessage() {
  return message;
}

int DOMException::getCode() {
  static std::map<kj::StringPtr, int> legacyCodes{
#define MAP_ENTRY(name, code, friendlyName) {friendlyName, code},
    JSG_DOM_EXCEPTION_FOR_EACH_ERROR_NAME(MAP_ENTRY)
#undef MAP_ENTRY
  };
  auto code = legacyCodes.find(name);
  if (code != legacyCodes.end()) {
    return code->second;
  }
  return 0;
}

v8::Local<v8::Value> DOMException::getStack(Lock& js) {
  return js.v8Get(errorForStack.getHandle(js), "stack"_kj);
}

void DOMException::visitForGc(GcVisitor& visitor) {
  visitor.visit(errorForStack);
}

void DOMException::serialize(jsg::Lock& js, jsg::Serializer& serializer) {
  serializer.writeLengthDelimited(message);
  serializer.writeLengthDelimited(name);
  serializer.write(js, JsValue(errorForStack.getHandle(js)));
}

jsg::Ref<DOMException> DOMException::deserialize(
    jsg::Lock& js, uint tag, jsg::Deserializer& deserializer) {
  kj::String message = deserializer.readLengthDelimitedString();
  kj::String name = deserializer.readLengthDelimitedString();
  auto errorForStack = KJ_ASSERT_NONNULL(deserializer.readValue(js).tryCast<JsObject>());
  return jsg::alloc<DOMException>(kj::mv(message),
                                  kj::mv(name),
                                  js.v8Ref<v8::Object>(errorForStack));
}

}  // namespace workerd::jsg
