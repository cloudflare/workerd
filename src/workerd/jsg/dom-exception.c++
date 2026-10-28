// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "dom-exception.h"

#include "jsvalue.h"
#include "ser.h"

#include <workerd/jsg/memory.h>

#include <kj/string.h>

namespace workerd::jsg {

namespace {

v8::Local<v8::Private> namePrivate(v8::Isolate* isolate) {
  return v8::Private::ForApi(isolate,
      v8::String::NewFromUtf8Literal(
          isolate, "workerd:DOMException.name", v8::NewStringType::kInternalized));
}

v8::Local<v8::Private> messagePrivate(v8::Isolate* isolate) {
  return v8::Private::ForApi(isolate,
      v8::String::NewFromUtf8Literal(
          isolate, "workerd:DOMException.message", v8::NewStringType::kInternalized));
}

int getCode(v8::Isolate* isolate, v8::Local<v8::String> name) {
  // Every name with a code is ASCII and shorter than this.
  char buffer[32];
  if (name->Length() >= sizeof(buffer)) return 0;
  size_t length = name->WriteUtf8V2(isolate, buffer, sizeof(buffer));
  return getDOMExceptionCode(kj::arrayPtr(buffer, length));
}

v8::Maybe<void> defineReadOnly(v8::Isolate* isolate,
    v8::Local<v8::Context> context,
    v8::Local<v8::Object> object,
    v8::Local<v8::String> key,
    v8::Local<v8::Value> value) {
  bool defined;
  if (!object->DefineOwnProperty(context, key, value, v8::PropertyAttribute::ReadOnly)
           .To(&defined)) {
    return v8::Nothing<void>();
  }
  KJ_ASSERT(defined, "could not define a DOMException property");
  return v8::JustVoid();
}

}  // namespace

int getDOMExceptionCode(kj::ArrayPtr<const char> name) {
  static constexpr struct {
    kj::StringPtr name;
    int code;
  } LEGACY_CODES[] = {
#define TABLE_ENTRY(name, code, friendlyName) {friendlyName##_kj, code},
    JSG_DOM_EXCEPTION_FOR_EACH_ERROR_NAME(TABLE_ENTRY)
#undef TABLE_ENTRY
  };
  for (auto& entry: LEGACY_CODES) {
    if (entry.name.asArray() == name) return entry.code;
  }
  return 0;
}

v8::Maybe<void> initDOMException(v8::Isolate* isolate,
    v8::Local<v8::Context> context,
    v8::Local<v8::Object> object,
    v8::Local<v8::String> name,
    v8::Local<v8::String> message) {
  // The order of `code`, `name` and `message` is the order in which they have always been own
  // properties of a DOMException, from JSG's lazy instance properties.
  auto code = v8::Integer::New(isolate, getCode(isolate, name));
  if (defineReadOnly(isolate, context, object,
          v8::String::NewFromUtf8Literal(isolate, "code", v8::NewStringType::kInternalized), code)
          .IsNothing() ||
      defineReadOnly(isolate, context, object,
          v8::String::NewFromUtf8Literal(isolate, "name", v8::NewStringType::kInternalized), name)
          .IsNothing() ||
      defineReadOnly(isolate, context, object,
          v8::String::NewFromUtf8Literal(isolate, "message", v8::NewStringType::kInternalized),
          message)
          .IsNothing()) {
    return v8::Nothing<void>();
  }
  if (object->SetPrivate(context, namePrivate(isolate), name).IsNothing() ||
      object->SetPrivate(context, messagePrivate(isolate), message).IsNothing()) {
    return v8::Nothing<void>();
  }
  return v8::JustVoid();
}

v8::MaybeLocal<v8::Object> newDOMException(v8::Isolate* isolate,
    v8::Local<v8::Context> context,
    v8::Local<v8::String> name,
    v8::Local<v8::String> message,
    v8::MaybeLocal<v8::String> stack) {
  auto prototype = getContextDataSlot(context, ContextDataSlot::DOM_EXCEPTION_PROTOTYPE);
  KJ_ASSERT(prototype->IsValue() && prototype.As<v8::Value>()->IsObject(),
      "the context has no DOMException prototype");
  auto object = v8::Object::New(isolate, prototype.As<v8::Value>(), nullptr, nullptr, 0);
  if (initDOMException(isolate, context, object, name, message).IsNothing()) return {};

  auto stackName =
      v8::String::NewFromUtf8Literal(isolate, "stack", v8::NewStringType::kInternalized);
  v8::Local<v8::String> stackValue;
  if (stack.ToLocal(&stackValue)) {
    // Writable and enumerable, but, as JSG has always defined it, not configurable.
    v8::PropertyDescriptor prop(stackValue, true);
    prop.set_enumerable(true);
    if (object->DefineProperty(context, stackName, prop).IsNothing()) return {};
  } else {
    if (v8::Exception::CaptureStackTrace(context, object).IsNothing()) return {};
    // Error properties are not enumerable, but a DOMException's always have been.
    v8::PropertyDescriptor prop;
    prop.set_enumerable(true);
    if (object->DefineProperty(context, stackName, prop).IsNothing()) return {};
  }
  return object;
}

kj::Maybe<DOMExceptionFields> tryGetDOMExceptionFields(
    v8::Isolate* isolate, v8::Local<v8::Value> value) {
  if (!value->IsObject()) return kj::none;
  auto object = value.As<v8::Object>();
  auto context = isolate->GetCurrentContext();
  // A private property is never looked up on the prototype chain, and reading one runs no
  // JavaScript, not even for a Proxy.
  v8::Local<v8::Value> name;
  v8::Local<v8::Value> message;
  if (!object->GetPrivate(context, namePrivate(isolate)).ToLocal(&name) || !name->IsString() ||
      !object->GetPrivate(context, messagePrivate(isolate)).ToLocal(&message) ||
      !message->IsString()) {
    return kj::none;
  }
  return DOMExceptionFields{name.As<v8::String>(), message.As<v8::String>()};
}

void setDOMExceptionPrototype(v8::Local<v8::Context> context, v8::Local<v8::Object> prototype) {
  setContextDataSlot(context, ContextDataSlot::DOM_EXCEPTION_PROTOTYPE, prototype);
}

void serializeDOMException(
    Lock& js, Serializer& serializer, v8::Local<v8::Object> object, DOMExceptionFields fields) {
  serializer.writeLengthDelimited(JsString(fields.name).toString(js));
  serializer.writeLengthDelimited(JsString(fields.message).toString(js));

  // It's a bit unfortunate that the stack here ends up also including the name and message
  // so we end up duplicating some of the information here, but that's OK. It's better to
  // keep this implementation simple rather than to implement any kind of deduplication.
  auto stack = JsObject(object).get(js, "stack"_kj);
  if (stack.isUndefined()) {
    serializer.writeLengthDelimited(""_kj);
  } else {
    serializer.writeLengthDelimited(stack.toString(js));
  }
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

  auto errName = kj::mv(name).orDefault([] { return kj::str("Error"); });
  // The name and message are also this class's lazy instance properties, so only the brand is set.
  auto isolate = js.v8Isolate;
  jsg::check(args.This()->SetPrivate(js.v8Context(), namePrivate(isolate), js.str(errName)));
  jsg::check(args.This()->SetPrivate(js.v8Context(), messagePrivate(isolate), js.str(errMessage)));

  return js.alloc<DOMException>(kj::mv(errMessage), kj::mv(errName));
}

kj::StringPtr DOMException::getName() const {
  return name;
}

kj::StringPtr DOMException::getMessage() const {
  return message;
}

int DOMException::getCode() const {
  return getDOMExceptionCode(name.asArray());
}

void DOMException::serialize(jsg::Lock& js, jsg::Serializer& serializer) {
  // Serializer::writeApiObject() writes every DOMException itself, by its brand, without calling
  // this.
  KJ_UNREACHABLE;
}

jsg::JsObject DOMException::deserialize(jsg::Lock& js, uint tag, jsg::Deserializer& deserializer) {
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
      return js.domException(kj::mv(message), kj::mv(name), kj::mv(stack));
    }
    default:
      KJ_UNREACHABLE;
  }
}

}  // namespace workerd::jsg
