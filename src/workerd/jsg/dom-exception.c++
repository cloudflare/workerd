// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "dom-exception.h"
#include <kj/string.h>
#include <map>

namespace workerd::jsg {

Ref<DOMException> DOMException::constructor(Optional<v8::Global<v8::String>> message,
                                               Optional<kj::String> name, v8::Isolate* isolate) {
  v8::Global<v8::String> errMessage;
  KJ_IF_MAYBE(m, message) {
    errMessage = kj::mv(*m);
  } else {
    errMessage = v8::Global<v8::String>(isolate, v8::String::Empty(isolate));
  }
  auto errorForStack = v8::Exception::Error(errMessage.Get(isolate)).As<v8::Object>();
  return jsg::alloc<DOMException>(kj::mv(errMessage), kj::mv(name),
                                  v8::Global<v8::Object>(isolate, errorForStack));
}

kj::String DOMException::getName() {
  KJ_IF_MAYBE(n, name) {
    return kj::str(*n);
  }
  return kj::str("Error");
}

v8::Local<v8::String> DOMException::getMessage(v8::Isolate* isolate) {
  return message.Get(isolate);
}

int DOMException::getCode() {
  static std::map<kj::StringPtr, int> legacyCodes{
#define MAP_ENTRY(name, code, friendlyName) {friendlyName, code},
    JSG_DOM_EXCEPTION_FOR_EACH_ERROR_NAME(MAP_ENTRY)
#undef MAP_ENTRY
  };
  KJ_IF_MAYBE(n, name) {
    auto code = legacyCodes.find(*n);
    if (code != legacyCodes.end()) {
      return code->second;
    }
  }
  return 0;
}

v8::Local<v8::Value> DOMException::getStack(v8::Isolate* isolate) {
  auto stackString = v8Str(isolate, "stack", v8::NewStringType::kInternalized);
  return check(errorForStack.Get(isolate)->Get(isolate->GetCurrentContext(), stackString));
}

}  // namespace workerd::jsg
