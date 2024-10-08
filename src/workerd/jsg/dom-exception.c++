// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "dom-exception.h"

#include "ser.h"

#include <workerd/jsg/memory.h>

#include <kj/string.h>

#include <map>

namespace workerd::jsg {

Ref<DOMException> DOMException::constructor(const v8::FunctionCallbackInfo<v8::Value>& args,
    Optional<kj::String> message,
    Optional<kj::String> name) {
  Lock& js = Lock::from(args.GetIsolate());
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
      kj::mv(errMessage), kj::mv(name).orDefault([] { return kj::str("Error"); }));
}

kj::StringPtr DOMException::getName() const {
  return name;
}

kj::StringPtr DOMException::getMessage() const {
  return message;
}

int DOMException::getCode() const {
  static const std::map<kj::StringPtr, int> legacyCodes{
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

void DOMException::serialize(jsg::Lock& js, jsg::Serializer& serializer) {
  serializer.writeLengthDelimited(name);
  serializer.writeLengthDelimited(message);

  // It's a bit unfortunate that the stack here ends up also including the name and message
  // so we end up duplicating some of the information here, but that's ok. It's better to
  // keep this implementation simple rather than to implement any kind of deduplication.
  KJ_IF_SOME(stack, this->stack.get(js, "stack")) {
    serializer.writeLengthDelimited(stack);
  } else {
    // This branch shouldn't really be taken in the typical case. It's only here
    // to handle the case where the stack property could not be unwrapped for some
    // reason. We don't need to treat it as an error case, just set the stack to
    // the empty string and move on.
    serializer.writeLengthDelimited(""_kj);
  }
}

jsg::Ref<DOMException> DOMException::deserialize(
    jsg::Lock& js, uint tag, jsg::Deserializer& deserializer) {
  switch (tag) {
    case SERIALIZATION_TAG_V2: {
      kj::String name = deserializer.readLengthDelimitedString();
      kj::String message = deserializer.readLengthDelimitedString();
      kj::String stack = deserializer.readLengthDelimitedString();
      return js.domException(kj::mv(name), kj::mv(message), kj::mv(stack));
    }
    case SERIALIZATION_TAG: {
      // This is the original serialization of DOMException. It was only
      // used for a very short period of time (a matter of weeks) but there's
      // still a remote chance that someone might use it in some persisted state
      // somewhere. So let's go ahead and support it.
      kj::String name = deserializer.readLengthDelimitedString();
      auto errorForStack = KJ_ASSERT_NONNULL(deserializer.readValue(js).tryCast<JsObject>());
      kj::String message =
          KJ_ASSERT_NONNULL(errorForStack.get(js, "message"_kj).tryCast<JsString>()).toString(js);
      kj::String stack =
          KJ_ASSERT_NONNULL(errorForStack.get(js, "stack").tryCast<JsString>()).toString(js);
      return js.domException(kj::mv(message), kj::mv(name), kj::mv(stack));
    }
    default:
      KJ_UNREACHABLE;
  }
}

}  // namespace workerd::jsg
