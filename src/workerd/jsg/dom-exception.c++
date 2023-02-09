// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "dom-exception.h"
#include <kj/string.h>
#include <map>

namespace workerd::jsg {

Ref<DOMException> DOMException::constructor(
    const v8::FunctionCallbackInfo<v8::Value>& args,
    Optional<kj::String> message,
    Optional<kj::String> name) {
  auto exception = jsg::alloc<DOMException>(
      kj::mv(message).orDefault([&] { return kj::String(); }),
      kj::mv(name).orDefault([] { return kj::str("Error"); }));
  // Uses the built-in Error.captureStackTrace method to attach the stack
  // to the wrapper object associated with this DOMException.
  captureStackTrace(args.GetIsolate(), args.This());
  return kj::mv(exception);
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

}  // namespace workerd::jsg
