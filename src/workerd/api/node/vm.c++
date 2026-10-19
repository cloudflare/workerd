// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "vm.h"

#include <workerd/io/worker.h>
#include <workerd/jsg/jsvalue.h>
#include <workerd/jsg/setup.h>

#include <span>

namespace workerd::api::node {
namespace {

using v8::Intercepted;

WD_STRONG_BOOL(IsModule);

// The sandbox remains the caller's object. Interceptors preserve accessors and direct
// references while V8 owns the realm's intrinsics, lexical environment, and global proxy.
Intercepted sandboxGet(v8::Local<v8::Name> name, const v8::PropertyCallbackInfo<v8::Value>& info) {
  auto sandbox = info.DataV2().As<v8::Object>();
  auto context = info.Holder()->GetCreationContextChecked(info.GetIsolate());
  v8::Local<v8::Value> value;
  if (!sandbox->GetRealNamedProperty(context, name).ToLocal(&value)) return Intercepted::kNo;
  if (value == sandbox) value = context->Global();
  info.GetReturnValue().Set(value);
  return Intercepted::kYes;
}

Intercepted sandboxQuery(
    v8::Local<v8::Name> name, const v8::PropertyCallbackInfo<v8::Integer>& info) {
  auto sandbox = info.DataV2().As<v8::Object>();
  auto context = info.Holder()->GetCreationContextChecked(info.GetIsolate());
  v8::PropertyAttribute attributes;
  if (!sandbox->GetRealNamedPropertyAttributes(context, name).To(&attributes)) {
    return Intercepted::kNo;
  }
  info.GetReturnValue().Set(static_cast<int>(attributes));
  return Intercepted::kYes;
}

Intercepted sandboxSet(v8::Local<v8::Name> name,
    v8::Local<v8::Value> value,
    const v8::PropertyCallbackInfo<v8::Boolean>& info) {
  auto sandbox = info.DataV2().As<v8::Object>();
  auto context = info.Holder()->GetCreationContextChecked(info.GetIsolate());
  v8::PropertyAttribute attributes = v8::None;
  if (context->Global()->GetRealNamedPropertyAttributes(context, name).To(&attributes) &&
      (attributes & v8::ReadOnly))
    return Intercepted::kNo;
  if (sandbox->GetRealNamedPropertyAttributes(context, name).To(&attributes) &&
      (attributes & v8::ReadOnly))
    return Intercepted::kNo;
  bool success;
  if (!sandbox->Set(context, name, value).To(&success)) return Intercepted::kYes;
  info.GetReturnValue().Set(success);
  return Intercepted::kYes;
}

Intercepted sandboxDescriptor(
    v8::Local<v8::Name> name, const v8::PropertyCallbackInfo<v8::Value>& info) {
  auto sandbox = info.DataV2().As<v8::Object>();
  auto context = info.Holder()->GetCreationContextChecked(info.GetIsolate());
  if (!sandbox->HasOwnProperty(context, name).FromMaybe(false)) return Intercepted::kNo;
  v8::Local<v8::Value> descriptor;
  if (!sandbox->GetOwnPropertyDescriptor(context, name).ToLocal(&descriptor)) {
    return Intercepted::kYes;
  }
  info.GetReturnValue().Set(descriptor);
  return Intercepted::kYes;
}

Intercepted sandboxDefine(v8::Local<v8::Name> name,
    const v8::PropertyDescriptor& descriptor,
    const v8::PropertyCallbackInfo<v8::Boolean>& info) {
  auto sandbox = info.DataV2().As<v8::Object>();
  auto context = info.Holder()->GetCreationContextChecked(info.GetIsolate());
  v8::PropertyAttribute attributes = v8::None;
  if (context->Global()->GetRealNamedPropertyAttributes(context, name).To(&attributes) &&
      (attributes & v8::ReadOnly) && (attributes & v8::DontDelete))
    return Intercepted::kNo;
  // V8's API takes a mutable descriptor, although DefineProperty does not modify it.
  auto define = [&](v8::PropertyDescriptor& copy) {
    if (descriptor.has_enumerable()) copy.set_enumerable(descriptor.enumerable());
    if (descriptor.has_configurable()) copy.set_configurable(descriptor.configurable());
    bool success;
    if (sandbox->DefineProperty(context, name, copy).To(&success)) {
      info.GetReturnValue().Set(success);
    }
  };
  if (descriptor.has_get() || descriptor.has_set()) {
    v8::PropertyDescriptor copy(
        descriptor.has_get() ? descriptor.get() : v8::Undefined(info.GetIsolate()).As<v8::Value>(),
        descriptor.has_set() ? descriptor.set() : v8::Undefined(info.GetIsolate()).As<v8::Value>());
    define(copy);
  } else if (descriptor.has_value()) {
    if (descriptor.has_writable()) {
      v8::PropertyDescriptor copy(descriptor.value(), descriptor.writable());
      define(copy);
    } else {
      v8::PropertyDescriptor copy(descriptor.value());
      define(copy);
    }
  } else if (descriptor.has_writable()) {
    v8::PropertyDescriptor copy(v8::Undefined(info.GetIsolate()), descriptor.writable());
    define(copy);
  } else {
    v8::PropertyDescriptor copy;
    define(copy);
  }
  return Intercepted::kYes;
}

Intercepted sandboxDelete(
    v8::Local<v8::Name> name, const v8::PropertyCallbackInfo<v8::Boolean>& info) {
  auto context = info.Holder()->GetCreationContextChecked(info.GetIsolate());
  bool success;
  if (!info.DataV2().As<v8::Object>()->Delete(context, name).To(&success)) {
    return Intercepted::kYes;
  }
  if (success) return Intercepted::kNo;
  info.GetReturnValue().Set(false);
  return Intercepted::kYes;
}

void sandboxEnumerate(const v8::PropertyCallbackInfo<v8::Array>& info) {
  auto context = info.Holder()->GetCreationContextChecked(info.GetIsolate());
  v8::Local<v8::Array> names;
  if (info.DataV2()
          .As<v8::Object>()
          ->GetPropertyNames(context, v8::KeyCollectionMode::kOwnOnly, v8::ALL_PROPERTIES,
              v8::IndexFilter::kSkipIndices)
          .ToLocal(&names)) {
    info.GetReturnValue().Set(names);
  }
}

v8::Local<v8::String> indexName(v8::Isolate* isolate, uint32_t index) {
  return jsg::Lock::from(isolate).str(kj::str(index));
}

Intercepted sandboxIndexGet(uint32_t index, const v8::PropertyCallbackInfo<v8::Value>& info) {
  return sandboxGet(indexName(info.GetIsolate(), index), info);
}
Intercepted sandboxIndexSet(
    uint32_t index, v8::Local<v8::Value> value, const v8::PropertyCallbackInfo<v8::Boolean>& info) {
  return sandboxSet(indexName(info.GetIsolate(), index), value, info);
}
Intercepted sandboxIndexQuery(uint32_t index, const v8::PropertyCallbackInfo<v8::Integer>& info) {
  return sandboxQuery(indexName(info.GetIsolate(), index), info);
}
Intercepted sandboxIndexDescriptor(
    uint32_t index, const v8::PropertyCallbackInfo<v8::Value>& info) {
  return sandboxDescriptor(indexName(info.GetIsolate(), index), info);
}
Intercepted sandboxIndexDefine(uint32_t index,
    const v8::PropertyDescriptor& descriptor,
    const v8::PropertyCallbackInfo<v8::Boolean>& info) {
  return sandboxDefine(indexName(info.GetIsolate(), index), descriptor, info);
}
Intercepted sandboxIndexDelete(uint32_t index, const v8::PropertyCallbackInfo<v8::Boolean>& info) {
  return sandboxDelete(indexName(info.GetIsolate(), index), info);
}
void sandboxIndexEnumerate(const v8::PropertyCallbackInfo<v8::Array>& info) {
  auto context = info.Holder()->GetCreationContextChecked(info.GetIsolate());
  v8::Local<v8::Array> names;
  if (!info.DataV2()
           .As<v8::Object>()
           ->GetPropertyNames(context, v8::KeyCollectionMode::kOwnOnly, v8::SKIP_SYMBOLS,
               v8::IndexFilter::kIncludeIndices)
           .ToLocal(&names))
    return;
  auto result = v8::Array::New(info.GetIsolate());
  uint32_t count = 0;
  for (uint32_t i = 0; i < names->Length(); ++i) {
    v8::Local<v8::Value> name;
    if (!names->Get(context, i).ToLocal(&name)) return;
    if (name->IsUint32() && result->Set(context, count++, name).IsNothing()) return;
  }
  info.GetReturnValue().Set(result);
}

jsg::Ref<VmContext> getMainContext(
    jsg::Lock& js, const jsg::TypeHandler<jsg::Ref<VmContext>>& handler) {
  auto context = js.v8Context();
  auto existing = jsg::getContextDataSlot(context, jsg::ContextDataSlot::CONTEXT_MODULE_RESOURCE);
  if (!existing.IsEmpty() && existing->IsValue()) {
    KJ_IF_SOME(resource, handler.tryUnwrap(js, existing.As<v8::Value>())) {
      return kj::mv(resource);
    }
  }
  auto result = js.alloc<VmContext>(js, context);
  result->attach(js, handler);
  return result;
}

v8::MaybeLocal<v8::Object> importAttributes(v8::Isolate* isolate,
    v8::Local<v8::Context> context,
    v8::Local<v8::FixedArray> attributes,
    int stride) {
  auto result = v8::Object::New(isolate);
  if (result->SetPrototype(context, v8::Null(isolate)).IsNothing()) return {};
  for (int i = 0; i < attributes->Length(); i += stride) {
    if (result
            ->CreateDataProperty(context, attributes->Get(i).As<v8::String>(),
                attributes->Get(i + 1).As<v8::Value>())
            .IsNothing())
      return {};
  }
  return result;
}

v8::Local<v8::Value> callCallback(jsg::Lock& js,
    v8::Local<v8::Object> callbacks,
    kj::StringPtr name,
    kj::ArrayPtr<v8::Local<v8::Value>> arguments) {
  auto callback = jsg::check(callbacks->Get(js.v8Context(), js.str(name)));
  if (callback->IsUndefined()) return v8::Undefined(js.v8Isolate);
  JSG_REQUIRE(callback->IsFunction(), TypeError, "Invalid VM callback");
  return jsg::check(callback.As<v8::Function>()->Call(
      js.v8Context(), callbacks, arguments.size(), arguments.begin()));
}

v8::Local<v8::PrimitiveArray> importOptions(jsg::Lock& js, v8::Local<v8::Symbol> token) {
  auto options = v8::PrimitiveArray::New(js.v8Isolate, 1);
  options->Set(js.v8Isolate, 0, token);
  return options;
}

v8::ScriptOrigin scriptOrigin(jsg::Lock& js,
    jsg::JsString filename,
    int lineOffset,
    int columnOffset,
    IsModule module,
    v8::Local<v8::Symbol> token) {
  return v8::ScriptOrigin(filename, lineOffset, columnOffset, false, -1, {}, false, false,
      module.toBool(), importOptions(js, token));
}

void requireVmEnabled(jsg::Lock& js) {
  JSG_REQUIRE(Worker::Isolate::from(js).getApi().getFeatureFlags().getEnableNodeJsVmContext(),
      Error, "VM contexts require the enable_nodejs_vm_context compatibility flag");
}

void requireCompilation(jsg::Lock& js, jsg::JsString code) {
  requireVmEnabled(js);
  if (!js.isEvalAllowed()) {
    js.throwException(jsg::JsValue(v8::Exception::EvalError(
        js.str("Code generation from strings disallowed for this context"_kj))));
  }
  jsg::IsolateBase::from(js.v8Isolate)
      .getObserver()
      .onDynamicEval(js.v8Context(), code, jsg::IsCodeLike::YES);
}

}  // namespace

void VmModuleRecord::visitForGc(jsg::GcVisitor& visitor) {
  visitor.visit(module, callbacks);
  for (auto& dependency: dependencies) visitor.visit(dependency);
}

VmContext::VmContext(jsg::Lock& js, v8::Local<v8::Context> context)
    : context(js.v8Isolate, context),
      importCallbacks(nullptr),
      weakMapGet(nullptr),
      weakMapSet(nullptr) {
  // V8 has no public WeakMap API. Capture its builtins in a private pristine realm,
  // so later mutations of either the host's or guest's globals cannot replace them.
  auto builtins = v8::Context::New(js.v8Isolate);
  jsg::setAlignedPointerInEmbedderData(
      builtins, jsg::ContextPointerSlot::MAX_POINTER_SLOT, nullptr);
  builtins->AllowCodeGenerationFromStrings(false);
  v8::Context::Scope scope(builtins);
  auto constructor =
      jsg::check(builtins->Global()->Get(builtins, js.str("WeakMap"_kj))).As<v8::Function>();
  auto map = jsg::check(constructor->NewInstance(builtins));
  importCallbacks = jsg::V8Ref<v8::Object>(js.v8Isolate, map);
  weakMapGet = jsg::V8Ref<v8::Function>(
      js.v8Isolate, jsg::check(map->Get(builtins, js.str("get"_kj))).As<v8::Function>());
  weakMapSet = jsg::V8Ref<v8::Function>(
      js.v8Isolate, jsg::check(map->Get(builtins, js.str("set"_kj))).As<v8::Function>());
}

void VmContext::attach(jsg::Lock& js, const jsg::TypeHandler<jsg::Ref<VmContext>>& handler) {
  auto local = getContext(js);
  auto wrapper = handler.wrap(js, JSG_THIS);
  jsg::setContextDataSlot(local, jsg::ContextDataSlot::CONTEXT_MODULE_RESOURCE, wrapper);
  jsg::setAlignedPointerInEmbedderData(local, jsg::ContextPointerSlot::CONTEXT_MODULE_CALLBACKS,
      static_cast<jsg::ContextModuleCallbacks*>(this));
}

void VmContext::visitForGc(jsg::GcVisitor& visitor) {
  visitor.visit(context, importCallbacks, weakMapGet, weakMapSet);
}

jsg::JsObject VmContext::getGlobal(jsg::Lock& js) {
  return jsg::JsObject(getContext(js)->Global());
}
v8::Local<v8::Context> VmContext::getContext(jsg::Lock& js) {
  return context.getHandle(js);
}
void VmContext::addModule(jsg::Lock& js,
    jsg::Ref<VmModule>& module,
    v8::Local<v8::Symbol> token,
    v8::Local<v8::Object> callbacks,
    const jsg::TypeHandler<jsg::Ref<VmModule>>& handler) {
  auto live = kj::Vector<jsg::WeakRef<VmModule>>(modules.size() + 1);
  for (auto& candidate: modules) {
    if (candidate.isAlive()) live.add(kj::mv(candidate));
  }
  live.add(module.getWeakRef(js));
  modules = kj::mv(live);
  setCallbackRecord(js, token, callbacks, handler.wrap(js, module.addRef()));
}
kj::Maybe<jsg::Ref<VmModule>> VmContext::findModule(jsg::Lock& js, v8::Local<v8::Module> module) {
  for (auto& candidate: modules) {
    KJ_IF_SOME(resource, candidate.tryAddRef(js)) {
      if (resource->getModule(js) == module) return kj::mv(resource);
    }
  }
  return kj::none;
}
void VmContext::setCallbackRecord(jsg::Lock& js,
    v8::Local<v8::Symbol> token,
    v8::Local<v8::Value> callbacks,
    v8::Local<v8::Value> resource) {
  v8::Local<v8::Value> values[] = {callbacks, resource};
  auto record = v8::Array::New(js.v8Isolate, values, 2);
  v8::Local<v8::Value> args[] = {token, record};
  jsg::check(
      weakMapSet.getHandle(js)->Call(js.v8Context(), importCallbacks.getHandle(js), 2, args));
}
void VmContext::addImportCallback(
    jsg::Lock& js, v8::Local<v8::Symbol> token, v8::Local<v8::Object> callbacks) {
  setCallbackRecord(js, token, callbacks, v8::Undefined(js.v8Isolate));
}

bool VmContext::initializeImportMeta(
    v8::Local<v8::Context> context, v8::Local<v8::Module> module, v8::Local<v8::Object> meta) {
  auto& js = jsg::Lock::current();
  KJ_IF_SOME(resource, findModule(js, module)) {
    try {
      v8::Local<v8::Value> args[] = {meta};
      callCallback(js, resource->record.callbacks.getHandle(js), "initializeImportMeta", args);
    } catch (jsg::JsExceptionThrown&) {
      // The exception is pending in V8 and is propagated by module evaluation.
    } catch (kj::Exception& exception) {
      js.v8Isolate->ThrowException(js.exceptionToJsValue(kj::mv(exception)).getHandle(js));
    }
    return true;
  }
  return false;
}

kj::Maybe<v8::MaybeLocal<v8::Promise>> VmContext::importDynamically(v8::Local<v8::Context> context,
    v8::Local<v8::Data> options,
    v8::Local<v8::String> specifier,
    v8::Local<v8::FixedArray> attributes) {
  auto& js = jsg::Lock::current();
  v8::TryCatch caught(js.v8Isolate);
  // This is a direct V8 callback. Even allocating or rejecting the promise can fail,
  // so none of these operations may use check() and unwind through V8 frames.
  auto rejected = [&](v8::Local<v8::Value> error) -> v8::MaybeLocal<v8::Promise> {
    v8::Local<v8::Promise::Resolver> resolver;
    if (!v8::Promise::Resolver::New(context).ToLocal(&resolver) ||
        resolver->Reject(context, error).IsNothing()) {
      if (caught.HasCaught()) caught.ReThrow();
      return {};
    }
    return resolver->GetPromise();
  };
  auto missingCallback = [&]() -> kj::Maybe<v8::MaybeLocal<v8::Promise>> {
    if (jsg::getOwnerContext(context) == context) return kj::none;
    return rejected(v8::Exception::TypeError(v8::String::NewFromUtf8Literal(
        js.v8Isolate, "A dynamic import callback was not specified for this VM script")));
  };
  KJ_TRY {
    auto result = [&]() -> kj::Maybe<v8::MaybeLocal<v8::Promise>> {
      if (options.IsEmpty() || !options->IsFixedArray()) return missingCallback();
      auto array = options.As<v8::FixedArray>();
      if (array->Length() != 1) return missingCallback();
      auto token = array->Get(0);
      if (!token->IsValue() || !token.As<v8::Value>()->IsSymbol()) return missingCallback();
      v8::Local<v8::Value> getArgs[] = {token.As<v8::Value>()};
      v8::Local<v8::Value> entry;
      if (!weakMapGet.getHandle(js)
               ->Call(context, importCallbacks.getHandle(js), 1, getArgs)
               .ToLocal(&entry))
        return v8::MaybeLocal<v8::Promise>();
      if (!entry->IsArray()) return missingCallback();
      v8::Local<v8::Value> callbacks;
      if (!entry.As<v8::Array>()->Get(context, 0).ToLocal(&callbacks)) {
        return v8::MaybeLocal<v8::Promise>();
      }
      v8::Local<v8::Object> decodedAttributes;
      if (!importAttributes(js.v8Isolate, context, attributes, 2).ToLocal(&decodedAttributes)) {
        return v8::MaybeLocal<v8::Promise>();
      }
      v8::Local<v8::Value> args[] = {specifier, decodedAttributes};
      v8::Local<v8::Value> callback;
      if (!callbacks.As<v8::Object>()
               ->Get(
                   context, v8::String::NewFromUtf8Literal(js.v8Isolate, "importModuleDynamically"))
               .ToLocal(&callback))
        return v8::MaybeLocal<v8::Promise>();
      if (!callback->IsFunction()) {
        return rejected(v8::Exception::TypeError(v8::String::NewFromUtf8Literal(
            js.v8Isolate, "A dynamic import callback was not specified for this VM script")));
      }
      v8::Local<v8::Value> value;
      if (!callback.As<v8::Function>()->Call(context, callbacks, 2, args).ToLocal(&value)) {
        return v8::MaybeLocal<v8::Promise>();
      }
      v8::Local<v8::Promise::Resolver> resolver;
      if (!v8::Promise::Resolver::New(context).ToLocal(&resolver) ||
          resolver->Resolve(context, value).IsNothing())
        return v8::MaybeLocal<v8::Promise>();
      return v8::MaybeLocal<v8::Promise>(resolver->GetPromise());
    }();
    if (!caught.HasCaught()) return result;
    if (!caught.CanContinue() || caught.Exception().IsEmpty()) {
      caught.ReThrow();
      return v8::MaybeLocal<v8::Promise>();
    }
    auto exception = caught.Exception();
    caught.Reset();
    return rejected(exception);
  }
  KJ_CATCH(exception) {
    // Converting a native error can itself fail during termination. Preserve the
    // pending V8 exception instead of allowing the conversion to unwind this callback.
    try {
      auto error = js.exceptionToJsValue(kj::mv(exception));
      caught.Reset();
      return rejected(error.getHandle(js));
    } catch (...) {
      if (caught.HasCaught()) caught.ReThrow();
      return v8::MaybeLocal<v8::Promise>();
    }
  }
}

jsg::JsValue VmScript::run(jsg::Lock& js,
    jsg::Optional<jsg::Ref<VmContext>> context,
    const jsg::TypeHandler<jsg::Ref<VmContext>>& handler) {
  auto target = [&] {
    KJ_IF_SOME(supplied, context) {
      return kj::mv(supplied);
    }
    return getMainContext(js, handler);
  }();
  auto local = target->getContext(js);
  v8::Context::Scope scope(local);
  target->addImportCallback(js, token.getHandle(js), callbacks.getHandle(js));
  auto bound = script.getHandle(js)->BindToCurrentContext();
  return jsg::JsValue(jsg::check(bound->Run(local)));
}

kj::Array<kj::byte> VmScript::createCachedData(jsg::Lock& js) {
  auto cache = v8::ScriptCompiler::CreateCodeCache(script.getHandle(js));
  if (cache == nullptr) return kj::Array<kj::byte>();
  KJ_DEFER(delete cache);
  return kj::heapArray<kj::byte>(cache->data, cache->length);
}

v8::Local<v8::Module> VmModule::getModule(jsg::Lock& js) {
  return record.module.getHandle(js);
}
int VmModule::getStatus(jsg::Lock& js) {
  return getModule(js)->GetStatus();
}
jsg::JsValue VmModule::getError(jsg::Lock& js) {
  JSG_REQUIRE(getModule(js)->GetStatus() == v8::Module::kErrored, Error, "Module has no error");
  return jsg::JsValue(getModule(js)->GetException());
}
jsg::JsValue VmModule::getNamespace(jsg::Lock& js) {
  JSG_REQUIRE(getModule(js)->GetStatus() >= v8::Module::kInstantiated, Error,
      "Module has not been instantiated");
  return jsg::JsValue(getModule(js)->GetModuleNamespace());
}
jsg::JsArray VmModule::getRequests(jsg::Lock& js) {
  auto requests = getModule(js)->GetModuleRequests();
  auto result = v8::Array::New(js.v8Isolate, requests->Length());
  for (int i = 0; i < requests->Length(); ++i) {
    auto request = requests->Get(i).As<v8::ModuleRequest>();
    JSG_REQUIRE(request->GetPhase() == v8::ModuleImportPhase::kEvaluation, TypeError,
        "Source phase imports are not supported in VM modules");
    auto item = v8::Object::New(js.v8Isolate);
    v8::Local<v8::String> specifierKey = js.str("specifier"_kj);
    v8::Local<v8::String> attributesKey = js.str("attributes"_kj);
    jsg::check(item->CreateDataProperty(js.v8Context(), specifierKey, request->GetSpecifier()));
    jsg::check(item->CreateDataProperty(js.v8Context(), attributesKey,
        jsg::check(
            importAttributes(js.v8Isolate, js.v8Context(), request->GetImportAttributes(), 3))));
    jsg::check(result->Set(js.v8Context(), i, item));
  }
  return jsg::JsArray(result);
}
void VmModule::setDependencies(jsg::Lock& js, kj::Array<jsg::Ref<VmModule>> dependencies) {
  JSG_REQUIRE(
      getModule(js)->GetStatus() == v8::Module::kUninstantiated, Error, "Module is already linked");
  JSG_REQUIRE(dependencies.size() == getModule(js)->GetModuleRequests()->Length(), TypeError,
      "Incorrect number of module dependencies");
  auto result = kj::heapArrayBuilder<jsg::V8Ref<v8::Module>>(dependencies.size());
  for (auto& dependency: dependencies) {
    JSG_REQUIRE(dependency->context.get() == context.get(), TypeError,
        "Linked modules must use the same context");
    result.add(jsg::V8Ref<v8::Module>(js.v8Isolate, dependency->getModule(js)));
  }
  record.dependencies = result.finish();
}

v8::MaybeLocal<v8::Module> VmModule::resolve(
    v8::Local<v8::Context> context, size_t index, v8::Local<v8::Module> referrer) {
  auto& js = jsg::Lock::current();
  KJ_TRY {
    KJ_IF_SOME(callbacks, jsg::getContextModuleCallbacks(context)) {
      KJ_IF_SOME(resource, static_cast<VmContext&>(callbacks).findModule(js, referrer)) {
        if (index < resource->record.dependencies.size()) {
          return resource->record.dependencies[index].getHandle(js);
        }
      }
    }
    js.v8Isolate->ThrowException(v8::Exception::Error(
        v8::String::NewFromUtf8Literal(js.v8Isolate, "VM module dependency is not linked")));
    return {};
  }
  KJ_CATCH(exception) {
    try {
      js.v8Isolate->ThrowException(js.exceptionToJsValue(kj::mv(exception)).getHandle(js));
    } catch (...) {
      // Error conversion has already scheduled the V8 exception or termination.
    }
    return {};
  }
}

void VmModule::instantiate(jsg::Lock& js) {
  auto local = context->getContext(js);
  v8::Context::Scope scope(local);
  jsg::check(getModule(js)->InstantiateModule(local, resolve));
}
jsg::JsValue VmModule::evaluate(jsg::Lock& js) {
  auto local = context->getContext(js);
  v8::Context::Scope scope(local);
  // These modules belong to their caller, unlike shared worker startup modules.
  // Keep the current request's I/O ownership through synchronous code and top-level await.
  // Returning the promise unchanged also avoids draining nested module evaluations.
  jsg::Lock::ModuleEvaluationScope evaluationScope(js);
  return jsg::JsValue(jsg::check(getModule(js)->Evaluate(local)));
}
void VmModule::setExport(jsg::Lock& js, jsg::JsString name, jsg::JsValue value) {
  JSG_REQUIRE(getModule(js)->IsSyntheticModule(), TypeError, "Module is not synthetic");
  jsg::check(getModule(js)->SetSyntheticModuleExport(js.v8Isolate, name, value));
}
v8::MaybeLocal<v8::Promise> VmModule::syntheticEvaluation(
    v8::Local<v8::Context> context, v8::Local<v8::Module> module) {
  auto& js = jsg::Lock::current();
  try {
    auto& callbacks = KJ_ASSERT_NONNULL(jsg::getContextModuleCallbacks(context));
    auto resource = KJ_ASSERT_NONNULL(static_cast<VmContext&>(callbacks).findModule(js, module));
    callCallback(js, resource->record.callbacks.getHandle(js), "evaluate", nullptr);
    auto resolver = jsg::check(v8::Promise::Resolver::New(context));
    jsg::check(resolver->Resolve(context, v8::Undefined(js.v8Isolate)));
    return resolver->GetPromise();
  } catch (jsg::JsExceptionThrown&) {
    return {};
  } catch (kj::Exception& exception) {
    js.v8Isolate->ThrowException(js.exceptionToJsValue(kj::mv(exception)).getHandle(js));
    return {};
  }
}

jsg::Ref<VmContext> VmUtil::createContext(jsg::Lock& js,
    jsg::Optional<jsg::V8Ref<v8::Object>> sandbox,
    bool strings,
    bool wasm,
    const jsg::TypeHandler<jsg::Ref<VmContext>>& handler) {
  requireVmEnabled(js);
  auto owner = js.v8Context();
  auto globalTemplate = v8::ObjectTemplate::New(js.v8Isolate);
  KJ_IF_SOME(object, sandbox) {
    auto handle = object.getHandle(js);
    globalTemplate->SetHandler(v8::NamedPropertyHandlerConfiguration(sandboxGet, sandboxSet,
        sandboxQuery, sandboxDelete, sandboxEnumerate, sandboxDefine, sandboxDescriptor, handle));
    globalTemplate->SetHandler(v8::IndexedPropertyHandlerConfiguration(sandboxIndexGet,
        sandboxIndexSet, sandboxIndexQuery, sandboxIndexDelete, sandboxIndexEnumerate,
        sandboxIndexDefine, sandboxIndexDescriptor, handle));
  }
  auto context = v8::Context::New(js.v8Isolate, nullptr, globalTemplate);
  if (!Worker::Isolate::from(js).getApi().getFeatureFlags().getJsWeakRef()) {
    jsg::deleteWeakRefGlobals(js.v8Isolate, context);
  }
#if V8_MAJOR_VERSION >= 15
  context->SetTemporalHostSystemUTCEpochNanosecondsCallback(
      [](v8::Local<v8::Context>) -> int64_t { return 0; });
#endif
  jsg::setAlignedPointerInEmbedderData(context, jsg::ContextPointerSlot::MAX_POINTER_SLOT, nullptr);
  context->SetSecurityToken(owner->GetSecurityToken());
  context->AllowCodeGenerationFromStrings(false);
  jsg::setContextDataSlot(context, jsg::ContextDataSlot::OWNER_CONTEXT, owner);
  jsg::setContextDataSlot(context, jsg::ContextDataSlot::CODE_GENERATION_STRINGS,
      v8::Boolean::New(js.v8Isolate, strings));
  jsg::setContextDataSlot(
      context, jsg::ContextDataSlot::CODE_GENERATION_WASM, v8::Boolean::New(js.v8Isolate, wasm));
  auto result = js.alloc<VmContext>(js, context);
  result->attach(js, handler);
  return result;
}
jsg::Ref<VmContext> VmUtil::mainContext(
    jsg::Lock& js, const jsg::TypeHandler<jsg::Ref<VmContext>>& handler) {
  requireVmEnabled(js);
  return getMainContext(js, handler);
}
jsg::Ref<VmScript> VmUtil::compileScript(jsg::Lock& js,
    jsg::JsString code,
    jsg::JsString filename,
    int lineOffset,
    int columnOffset,
    jsg::V8Ref<v8::Object> callbacks) {
  requireCompilation(js, code);
  auto token = v8::Symbol::New(js.v8Isolate);
  auto origin = scriptOrigin(js, filename, lineOffset, columnOffset, IsModule::NO, token);
  v8::ScriptCompiler::Source source(code, origin);
  auto script = jsg::check(v8::ScriptCompiler::CompileUnboundScript(js.v8Isolate, &source));
  return js.alloc<VmScript>(jsg::V8Ref<v8::UnboundScript>(js.v8Isolate, script),
      jsg::V8Ref<v8::Symbol>(js.v8Isolate, token), kj::mv(callbacks));
}
jsg::JsValue VmUtil::compileFunction(jsg::Lock& js,
    jsg::JsString code,
    kj::Array<jsg::V8Ref<v8::String>> params,
    jsg::JsString filename,
    int lineOffset,
    int columnOffset,
    jsg::Optional<jsg::Ref<VmContext>> context,
    kj::Array<jsg::V8Ref<v8::Object>> extensions,
    jsg::V8Ref<v8::Object> callbacks,
    const jsg::TypeHandler<jsg::Ref<VmContext>>& handler) {
  requireCompilation(js, code);
  auto target = [&] {
    KJ_IF_SOME(supplied, context) {
      return kj::mv(supplied);
    }
    return getMainContext(js, handler);
  }();
  v8::Context::Scope scope(target->getContext(js));
  auto token = v8::Symbol::New(js.v8Isolate);
  auto origin = scriptOrigin(js, filename, lineOffset, columnOffset, IsModule::NO, token);
  v8::ScriptCompiler::Source source(code, origin);
  v8::LocalVector<v8::String> arguments(js.v8Isolate);
  for (auto& param: params) arguments.push_back(param.getHandle(js));
  v8::LocalVector<v8::Object> scopes(js.v8Isolate);
  for (auto& extension: extensions) scopes.push_back(extension.getHandle(js));
  auto function = jsg::check(v8::ScriptCompiler::CompileFunction(
      js.v8Context(), &source, arguments.size(), arguments.data(), scopes.size(), scopes.data()));
  target->addImportCallback(js, token, callbacks.getHandle(js));
  return jsg::JsValue(function);
}
jsg::Ref<VmModule> VmUtil::sourceTextModule(jsg::Lock& js,
    jsg::Ref<VmContext> context,
    jsg::JsString code,
    jsg::JsString identifier,
    int lineOffset,
    int columnOffset,
    jsg::V8Ref<v8::Object> callbacks,
    const jsg::TypeHandler<jsg::Ref<VmModule>>& handler) {
  requireCompilation(js, code);
  v8::Context::Scope scope(context->getContext(js));
  auto token = v8::Symbol::New(js.v8Isolate);
  auto origin = scriptOrigin(js, identifier, lineOffset, columnOffset, IsModule::YES, token);
  v8::ScriptCompiler::Source source(code, origin);
  auto module = jsg::check(v8::ScriptCompiler::CompileModule(js.v8Isolate, &source));
  auto record = VmModuleRecord{
    .module = jsg::V8Ref<v8::Module>(js.v8Isolate, module),
    .callbacks = kj::mv(callbacks),
    .dependencies = nullptr,
  };
  auto callbackHandle = record.callbacks.getHandle(js);
  auto result = js.alloc<VmModule>(context.addRef(), kj::mv(record));
  context->addModule(js, result, token, callbackHandle, handler);
  return result;
}
jsg::Ref<VmModule> VmUtil::syntheticModule(jsg::Lock& js,
    jsg::Ref<VmContext> context,
    kj::Array<jsg::V8Ref<v8::String>> exports,
    jsg::JsString identifier,
    jsg::V8Ref<v8::Object> callbacks,
    const jsg::TypeHandler<jsg::Ref<VmModule>>& handler) {
  requireVmEnabled(js);
  v8::Context::Scope scope(context->getContext(js));
  auto token = v8::Symbol::New(js.v8Isolate);
  v8::LocalVector<v8::String> names(js.v8Isolate);
  for (auto& name: exports) names.push_back(name.getHandle(js));
  auto module = v8::Module::CreateSyntheticModule(js.v8Isolate, identifier,
      std::span<const v8::Local<v8::String>>(names.data(), names.size()),
      VmModule::syntheticEvaluation, importOptions(js, token));
  auto record = VmModuleRecord{
    .module = jsg::V8Ref<v8::Module>(js.v8Isolate, module),
    .callbacks = kj::mv(callbacks),
    .dependencies = nullptr,
  };
  auto callbackHandle = record.callbacks.getHandle(js);
  auto result = js.alloc<VmModule>(context.addRef(), kj::mv(record));
  context->addModule(js, result, token, callbackHandle, handler);
  return result;
}

}  // namespace workerd::api::node
