// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "dom-exception.h"

#include "jsvalue.h"
#include "ser.h"
#include "setup.h"

#include <workerd/jsg/memory.h>

#include <kj/string.h>

#include <map>

namespace workerd::jsg {

namespace {
// Skips interceptors and the prototype chain, but calls the property's getter if it has one.
kj::Maybe<v8::Local<v8::Value>> tryGetOwnProperty(
    Lock& js, v8::Local<v8::Object> object, kj::StringPtr name) {
  auto context = js.v8Context();
  v8::Local<v8::String> key = js.strIntern(name);
  if (!check(object->HasRealNamedProperty(context, key))) return kj::none;
  return check(object->GetRealNamedProperty(context, key));
}

// An instance of getBootstrapDomExceptionBase() whose prototype is `prototype`.
JsObject newDomExceptionWrapper(Lock& js, JsObject prototype) {
  auto context = js.v8Context();
  auto tmpl = IsolateBase::getDomExceptionTemplate(js.v8Isolate);
  auto object = check(tmpl->InstanceTemplate()->NewInstance(context));
  check(object->SetPrototype(context, prototype));
  return JsObject(object);
}

// Gives `object` V8's stack, unless `stack` is given, which becomes an own non-enumerable data
// property.
void setDomExceptionStack(Lock& js, JsObject object, kj::Maybe<JsString> stack) {
  auto context = js.v8Context();
  v8::Local<v8::Object> handle = object;
  KJ_IF_SOME(s, stack) {
    v8::Local<v8::String> stackName = js.strIntern("stack"_kj);
    check(handle->DefineOwnProperty(context, stackName, s, v8::DontEnum));
  } else {
    check(v8::Exception::CaptureStackTrace(context, handle));
  }
}
}  // namespace

kj::Maybe<JsObject> tryGetBootstrapDomExceptionPrototype(Lock& js) {
  auto& modules = KJ_UNWRAP_OR(tryGetBootstrapModules(js), return kj::none);
  auto exports = KJ_UNWRAP_OR(modules.tryGetLoadedExports(js, "dom-exception"_kj), return kj::none);
  auto exportsObject = KJ_UNWRAP_OR(exports.tryCast<JsObject>(), return kj::none);
  auto ctor =
      KJ_UNWRAP_OR(tryGetOwnProperty(js, exportsObject, "DOMException"_kj), return kj::none);
  if (!ctor->IsFunction()) return kj::none;
  auto prototype =
      KJ_UNWRAP_OR(tryGetOwnProperty(js, ctor.As<v8::Object>(), "prototype"_kj), return kj::none);
  if (!prototype->IsObject()) return kj::none;
  auto result = JsObject(prototype.As<v8::Object>());
  if (!result.hasPrivate(js, DOM_EXCEPTION_PROTOTYPE_KEY)) return kj::none;
  return result;
}

JsObject getBootstrapDomExceptionBase(Lock& js) {
  return JsObject(
      check(IsolateBase::getDomExceptionTemplate(js.v8Isolate)->GetFunction(js.v8Context())));
}

JsObject newBootstrapDomException(Lock& js,
    JsObject prototype,
    kj::StringPtr name,
    kj::StringPtr message,
    kj::Maybe<kj::StringPtr> stack) {
  // As the TypeScript constructor makes one. V8 formats the stack's header lazily, from the name
  // and message the prototype's getters return.
  auto result = newDomExceptionWrapper(js, prototype);
  // In the TypeScript constructor's order, so that both make instances of the same shape.
  result.setPrivate(js, DOM_EXCEPTION_BRAND_KEY, js.boolean(true));
  result.setPrivate(js, ERROR_NAME_KEY, js.str(name));
  result.setPrivate(js, ERROR_MESSAGE_KEY, js.str(message));
  setDomExceptionStack(js, result, stack.map([&](kj::StringPtr s) { return js.str(s); }));
  return result;
}

JsObject newUninitializedBootstrapDomException(
    Lock& js, JsObject prototype, kj::Maybe<JsString> stack) {
  auto result = newDomExceptionWrapper(js, prototype);
  setDomExceptionStack(js, result, stack);
  return result;
}

kj::Maybe<BootstrapDomExceptionInfo> tryGetBootstrapDomExceptionInfo(
    Lock& js, v8::Local<v8::Value> value) {
  if (!value->IsObject()) return kj::none;
  auto object = JsObject(value.As<v8::Object>());
  if (!object.hasPrivate(js, DOM_EXCEPTION_BRAND_KEY)) return kj::none;
  auto name =
      KJ_UNWRAP_OR(object.getPrivate(js, ERROR_NAME_KEY).tryCast<JsString>(), return kj::none);
  auto message =
      KJ_UNWRAP_OR(object.getPrivate(js, ERROR_MESSAGE_KEY).tryCast<JsString>(), return kj::none);
  return BootstrapDomExceptionInfo{.name = name, .message = message};
}

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

  return js.alloc<DOMException>(
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
  // so we end up duplicating some of the information here, but that's OK. It's better to
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

JsObject DOMException::deserialize(jsg::Lock& js, uint tag, jsg::Deserializer& deserializer) {
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
      auto errorForStack = KJ_UNWRAP_OR(deserializer.readValue(js).tryCast<JsObject>(), {
        JSG_FAIL_REQUIRE(
            DOMDataCloneError, "Deserialization failed: DOMException payload is not an object");
      });
      auto messageJs = KJ_UNWRAP_OR(errorForStack.get(js, "message"_kj).tryCast<JsString>(), {
        JSG_FAIL_REQUIRE(
            DOMDataCloneError, "Deserialization failed: DOMException message is not a string");
      });
      auto stackJs = KJ_UNWRAP_OR(errorForStack.get(js, "stack").tryCast<JsString>(), {
        JSG_FAIL_REQUIRE(
            DOMDataCloneError, "Deserialization failed: DOMException stack is not a string");
      });
      kj::String message = messageJs.toString(js);
      kj::String stack = stackJs.toString(js);
      return js.domException(kj::mv(name), kj::mv(message), kj::mv(stack));
    }
    default:
      KJ_UNREACHABLE;
  }
}

v8::MaybeLocal<v8::Object> newDOMException(
    v8::Isolate* isolate, kj::ArrayPtr<const char> name, v8::Local<v8::String> message) {
  // A missing context is a bug in the caller: fail loudly in debug builds, and in release builds
  // throw an internal error rather than crash.
  KJ_DASSERT(isolate->InContext(), "creating a DOMException requires an entered context");
  // Creating a DOMException enters V8 with the context (DefineProperty, CaptureStackTrace), which
  // would clear a pending termination and let the script run on.
  if (isolate->IsExecutionTerminating() || isolate->HasPendingException()) return {};
  try {
    KJ_REQUIRE(isolate->InContext(), "creating a DOMException requires an entered context");
    auto& js = Lock::from(isolate);
    v8::Local<v8::Object> exception = js.domException(kj::str(name), kj::str(message));
    return exception;
  } catch (JsExceptionThrown&) {
    // Creating the DOMException threw, for instance because the stack is exhausted; that
    // exception stays pending.
    return {};
  } catch (...) {
    throwInternalError(isolate, kj::getCaughtExceptionAsKj());
    return {};
  }
}

kj::Maybe<DOMExceptionNameAndMessage> tryGetDOMExceptionNameAndMessage(
    v8::Isolate* isolate, v8::Local<v8::Value> value) {
  KJ_REQUIRE(isolate->InContext(), "unwrapping a DOMException requires an entered context");
  auto& js = Lock::from(isolate);
  return js.tryGetDomExceptionInfo(JsValue(value)).map([](Lock::DomExceptionInfo&& info) {
    return DOMExceptionNameAndMessage{
      .name = kj::mv(info.name),
      .message = kj::mv(info.message),
    };
  });
}

}  // namespace workerd::jsg
