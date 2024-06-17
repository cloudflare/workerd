// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "jsg.h"
#include "setup.h"

namespace workerd::jsg {

// TODO(cleanup): Factor out toObject(), getInterned() into some sort of v8 tools module?

void exposeGlobalScopeType(v8::Isolate* isolate, v8::Local<v8::Context> context) {
  auto global = context->Global();

  const auto toObject = [context](v8::Local<v8::Value> value) {
    return check(value->ToObject(context));
  };
  const auto getInterned = [isolate, context](v8::Local<v8::Object> object, const char* s) {
    return check(object->Get(context, v8StrIntern(isolate, s)));
  };

  auto constructor = getInterned(global, "constructor");
  auto name = getInterned(toObject(constructor), "name");

  KJ_ASSERT(check(global->Set(context, name, constructor)));
}

v8::Local<v8::Symbol> getSymbolDispose(v8::Isolate* isolate) {
  return v8::Symbol::GetDispose(isolate);
}
v8::Local<v8::Symbol> getSymbolAsyncDispose(v8::Isolate* isolate) {
  return v8::Symbol::GetAsyncDispose(isolate);
}

void throwIfConstructorCalledAsFunction(
    const v8::FunctionCallbackInfo<v8::Value>& args,
    const std::type_info& type) {
  if (!args.IsConstructCall()) {
    throwTypeError(args.GetIsolate(), kj::str(
        "Failed to construct '", typeName(type),
        "': Please use the 'new' operator, this object constructor cannot be called "
        "as a function."));
  }
}

void scheduleUnimplementedConstructorError(
    const v8::FunctionCallbackInfo<v8::Value>& args,
    const std::type_info& type) {
  auto isolate = args.GetIsolate();
  isolate->ThrowError(v8StrIntern(isolate,
      kj::str("Failed to construct '", typeName(type), "': the constructor is not implemented.")));
}

void scheduleUnimplementedMethodError(
    const v8::FunctionCallbackInfo<v8::Value>& args,
    const std::type_info& type, const char* methodName) {
  auto isolate = args.GetIsolate();
  isolate->ThrowError(v8StrIntern(isolate,
      kj::str("Failed to execute '", methodName, "' on '", typeName(type),
              "': the method is not implemented.")));
}

void scheduleUnimplementedPropertyError(
    v8::Isolate* isolate,
    const std::type_info& type, const char* propertyName) {
  isolate->ThrowError(v8StrIntern(isolate,
      kj::str("Failed to get the '", propertyName, "' property on '", typeName(type),
              "': the property is not implemented.")));
}

}  // namespace workerd::jsg
