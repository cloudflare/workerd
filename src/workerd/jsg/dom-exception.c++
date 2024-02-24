// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "dom-exception.h"
#include <workerd/jsg/memory.h>
#include <kj/string.h>
#include <map>

namespace workerd::jsg {

Ref<DOMException> DOMException::constructor(
    const v8::FunctionCallbackInfo<v8::Value>& args,
    Optional<kj::String> message,
    Optional<kj::String> name) {
  jsg::Lock& js = jsg::Lock::from(args.GetIsolate());
  kj::String errMessage = kj::mv(message).orDefault([&] { return kj::String(); });

  // V8 gives Error objects a non-standard (but widely known) `stack` property, and Web IDL
  // requires that DOMException get any non-standard properties that Error gets. Chrome honors
  // this requirement only for runtime-generated DOMExceptions -- script-generated DOMExceptions
  // don't get `stack`, even though script-generated Errors do. It's more convenient and, IMO,
  // more conformant to just give all DOMExceptions a `stack` property.
  jsg::check(v8::Exception::CaptureStackTrace(js.v8Context(), args.This()));

  // This part is a bit of a hack. By default, the various properties on JavaScript errors
  // are not enumerable. However, our implementation of DOMException has always defined
  // them as enumerable, which means just setting the stack above would be a breaking change.
  // To maintain backwards compat we have to define the stack as enumerable here.
  v8::PropertyDescriptor prop;
  prop.set_enumerable(true);
  v8::Local<v8::String> stackName = js.str("stack"_kjc);
  jsg::check(args.This()->DefineProperty(js.v8Context(), stackName, prop));

  return jsg::alloc<DOMException>(
      kj::mv(errMessage),
      kj::mv(name).orDefault([] { return kj::str("Error"); }));
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

void DOMException::visitForGc(GcVisitor& visitor) {}

}  // namespace workerd::jsg
