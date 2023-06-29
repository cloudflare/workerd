// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "jsg.h"
#include "modules.h"
#include "resource.h"
#include "src/workerd/jsg/jsg.h"

namespace workerd::jsg {


template<typename TypeWrapper, typename Self, bool isContext>
struct ResourceTypeBuilder {
  // Used by the JSG_METHOD macro to register a method on a resource type.

  ResourceTypeBuilder(
      jsg::Lock& js,
      TypeWrapper& typeWrapper,
      v8::Isolate* isolate,
      v8::Local<v8::Context> context,
      v8::Local<v8::FunctionTemplate> constructor,
      v8::Local<v8::ObjectTemplate> instance,
      v8::Local<v8::ObjectTemplate> prototype,
      v8::Local<v8::Signature> signature)
      : js(js),
        typeWrapper(typeWrapper),
        isolate(isolate),
        context(context),
        constructor(constructor),
        instance(instance),
        prototype(prototype),
        signature(signature) { }

  template<typename Type>
  inline void registerInherit() {
    constructor->Inherit(typeWrapper.template getTemplate<isContext>(isolate, (Type*)nullptr));
  }

  template<const char* name>
  inline void registerInheritIntrinsic(v8::Intrinsic intrinsic) {
    auto intrinsicPrototype = v8::FunctionTemplate::New(isolate);
    intrinsicPrototype->RemovePrototype();
    auto prototypeString = ::workerd::jsg::v8StrIntern(isolate, "prototype");
    intrinsicPrototype->SetIntrinsicDataProperty(prototypeString, intrinsic);
    constructor->Inherit(intrinsicPrototype);
  }

  template <typename Method, Method method>
  inline void registerCallable() {
    // Note that we set the call handler on the instance and not the prototype.
    // TODO(cleanup): Specifying the name (for error messages) as "(called as function)" is a bit
    //   hacky but it's hard to do better while reusing `MethodCallback`.
    static const char NAME[] = "(called as function)";
    instance->SetCallAsFunctionHandler(
        &MethodCallback<TypeWrapper, NAME, isContext,
                        Self, Method, method,
                        ArgumentIndexes<Method>>::callback);
  }

  template<const char* name, typename Method, Method method>
  inline void registerMethod() {
    prototype->Set(isolate, name, v8::FunctionTemplate::New(isolate,
        &MethodCallback<TypeWrapper, name, isContext, Self, Method, method,
                        ArgumentIndexes<Method>>::callback,
        v8::Local<v8::Value>(), signature, 0, v8::ConstructorBehavior::kThrow));
  }

  template<const char* name, typename Method, Method method>
  inline void registerStaticMethod() {
    // Notably, we specify an empty signature because a static method invocation will have no holder
    // object.
    auto functionTemplate = v8::FunctionTemplate::New(isolate,
        &StaticMethodCallback<TypeWrapper, name, Self, Method, method,
                              ArgumentIndexes<Method>>::callback,
        v8::Local<v8::Value>(), v8::Local<v8::Signature>(), 0, v8::ConstructorBehavior::kThrow);
    functionTemplate->RemovePrototype();
    constructor->Set(v8StrIntern(isolate, name), functionTemplate);
  }

  template<const char* name, typename Getter, Getter getter, typename Setter, Setter setter>
  inline void registerInstanceProperty() {
    using Gcb = GetterCallback<TypeWrapper, name, Getter, getter, isContext>;

    instance->SetNativeDataProperty(
        v8StrIntern(isolate, name),
        Gcb::callback, &SetterCallback<TypeWrapper, name, Setter, setter, isContext>::callback,
        v8::Local<v8::Value>(),
        Gcb::enumerable ? v8::PropertyAttribute::None : v8::PropertyAttribute::DontEnum);
  }

  template<const char* name, typename Getter, Getter getter, typename Setter, Setter setter>
  inline void registerPrototypeProperty() {
    using Gcb = GetterCallback<TypeWrapper, name, Getter, getter, isContext>;

    prototype->SetAccessor(
        v8StrIntern(isolate, name),
        Gcb::callback,
        &SetterCallback<TypeWrapper, name, Setter, setter, isContext>::callback,
        v8::Local<v8::Value>(),
        v8::AccessControl::DEFAULT,
        Gcb::enumerable ? v8::PropertyAttribute::None : v8::PropertyAttribute::DontEnum);
  }

  template<const char* name, typename Getter, Getter getter>
  inline void registerReadonlyInstanceProperty() {
    using Gcb = GetterCallback<TypeWrapper, name, Getter, getter, isContext>;

    instance->SetNativeDataProperty(
        v8StrIntern(isolate, name),
        &Gcb::callback, nullptr, v8::Local<v8::Value>(),
        Gcb::enumerable ? v8::PropertyAttribute::ReadOnly
                        : static_cast<v8::PropertyAttribute>(
                            v8::PropertyAttribute::ReadOnly | v8::PropertyAttribute::DontEnum));
  }

  template<typename T>
  inline void registerReadonlyInstanceProperty(kj::StringPtr name, T value) {
    auto v8Name = v8StrIntern(isolate, name);
    auto v8Value = typeWrapper.wrap(isolate, nullptr, kj::mv(value));
    instance->Set(v8Name, v8Value, v8::PropertyAttribute::ReadOnly);
  }

  template<const char* name, typename Getter, Getter getter>
  inline void registerReadonlyPrototypeProperty() {
    using Gcb = GetterCallback<TypeWrapper, name, Getter, getter, isContext>;

    prototype->SetAccessor(
        v8StrIntern(isolate, name),
        &Gcb::callback,
        nullptr,
        v8::Local<v8::Value>(),
        v8::AccessControl::DEFAULT,
        Gcb::enumerable ? v8::PropertyAttribute::ReadOnly
                        : static_cast<v8::PropertyAttribute>(
                            v8::PropertyAttribute::ReadOnly | v8::PropertyAttribute::DontEnum));
  }


  template<const char* name, typename Getter, Getter getter, bool readOnly>
  inline void registerLazyInstanceProperty() {
    using Gcb = GetterCallback<TypeWrapper, name, Getter, getter, isContext>;
    v8::PropertyAttribute attributes =
        Gcb::enumerable ? v8::PropertyAttribute::None : v8::PropertyAttribute::DontEnum;
    if (readOnly) {
      attributes = static_cast<v8::PropertyAttribute>(attributes | v8::PropertyAttribute::ReadOnly);
    }
    instance->SetLazyDataProperty(
        v8StrIntern(isolate, name),
        &Gcb::callback, v8::Local<v8::Value>(),
        attributes);
  }


  template<const char* name, typename T>
  inline void registerStaticConstant(T value) {
    // The main difference between this and a read-only property is that a static constant has no
    // getter but is simply a primitive value set at constructor creation time.

    auto v8Name = v8StrIntern(isolate, name);
    auto v8Value = typeWrapper.wrap(isolate, nullptr, kj::mv(value));

    constructor->Set(v8Name, v8Value, v8::PropertyAttribute::ReadOnly);
    constructor->PrototypeTemplate()->Set(v8Name, v8Value, v8::PropertyAttribute::ReadOnly);
  }

  template<const char* name, typename Method, Method method>
  inline void registerIterable() {
    prototype->Set(v8::Symbol::GetIterator(isolate), v8::FunctionTemplate::New(isolate,
        &MethodCallback<TypeWrapper, name, isContext, Self, Method, method,
                        ArgumentIndexes<Method>>::callback,
        v8::Local<v8::Value>(), signature, 0, v8::ConstructorBehavior::kThrow),
        v8::PropertyAttribute::DontEnum);
  }

  template<const char* name, typename Method, Method method>
  inline void registerAsyncIterable() {
    prototype->Set(v8::Symbol::GetAsyncIterator(isolate), v8::FunctionTemplate::New(isolate,
        &MethodCallback<TypeWrapper, name, isContext, Self, Method, method,
                        ArgumentIndexes<Method>>::callback,
        v8::Local<v8::Value>(), signature, 0, v8::ConstructorBehavior::kThrow),
        v8::PropertyAttribute::DontEnum);
  }

  template<typename Type, const char* name>
  inline void registerNestedType() {
    static_assert(Type::JSG_KIND == ::workerd::jsg::JsgKind::RESOURCE,
        "Type is not a resource type, and therefore cannot not be declared nested");

    constexpr auto hasGetTemplate = ::workerd::jsg::isDetected<
        ::workerd::jsg::HasGetTemplateOverload, decltype(typeWrapper), Type>();
    static_assert(hasGetTemplate,
          "Type must be listed in JSG_DECLARE_ISOLATE_TYPE to be declared nested.");

    prototype->Set(isolate, name, typeWrapper.getTemplate(isolate, (Type*)nullptr));
  }

  kj::ArrayPtr<const char> findModule(Bundle::Reader bundle, kj::StringPtr moduleName) {
    for (auto module: bundle.getModules()) {
      if (module.getName() == moduleName) {
        return module.getSrc().asChars();
      }
    }

    KJ_FAIL_REQUIRE("Module not found", moduleName);
  }

  inline void registerNestedJsType(Bundle::Reader bundle, kj::StringPtr moduleName, kj::StringPtr typeName) {
    // // Must pass true for `is_module`, but we can skip everything else.
    // const int resourceLineOffset = 0;
    // const int resourceColumnOffset = 0;
    // const bool resourceIsSharedCrossOrigin = false;
    // const int scriptId = -1;
    // const bool resourceIsOpaque = false;
    // const bool isWasm = false;
    // const bool isModule = true;
    // v8::ScriptOrigin origin(js.v8Isolate,
    //                         v8StrIntern(js.v8Isolate, moduleName),
    //                         resourceLineOffset,
    //                         resourceColumnOffset,
    //                         resourceIsSharedCrossOrigin, scriptId, {},
    //                         resourceIsOpaque, isWasm, isModule);
    // v8::Local<v8::String> contentStr;
    // contentStr = jsg::v8Str(js.v8Isolate, findModule(bundle, moduleName));
    // v8::ScriptCompiler::Source source(contentStr, origin);
    // auto module = jsg::check(v8::ScriptCompiler::CompileModule(js.v8Isolate, &source));
    // jsg::instantiateModule(js, module);

    // KJ_FAIL_REQUIRE("NOT IMPLEMENTED", moduleName, typeName);
    // prototype->Set(isolate, name, typeWrapper.getTemplate(isolate, (Type*)nullptr));
  }

  inline void registerTypeScriptRoot() { /* only needed for RTTI */ }

  template<const char* tsOverride>
  inline void registerTypeScriptOverride() { /* only needed for RTTI */ }

  template<const char* tsDefine>
  inline void registerTypeScriptDefine() { /* only needed for RTTI */ }

private:
  jsg::Lock& js;
  TypeWrapper& typeWrapper;
  v8::Isolate* isolate;
  v8::Local<v8::Context> context;
  v8::Local<v8::FunctionTemplate> constructor;
  v8::Local<v8::ObjectTemplate> instance;
  v8::Local<v8::ObjectTemplate> prototype;
  v8::Local<v8::Signature> signature;
};

template<typename TypeWrapper, typename Self>
struct JsTypesLoader {
  JsTypesLoader(v8::Isolate* isolate, v8::Local<v8::Context> context) : isolate(isolate), context(context) {}

  template<typename Type>
  inline void registerInherit() { }

  template<const char* name>
  inline void registerInheritIntrinsic(v8::Intrinsic intrinsic) { }

  template <typename Method, Method method>
  inline void registerCallable() { }

  template<const char* name, typename Method, Method method>
  inline void registerMethod() { }

  template<const char* name, typename Method, Method method>
  inline void registerStaticMethod() { }

  template<const char* name, typename Getter, Getter getter, typename Setter, Setter setter>
  inline void registerInstanceProperty() { }

  template<const char* name, typename Getter, Getter getter, typename Setter, Setter setter>
  inline void registerPrototypeProperty() { }

  template<const char* name, typename Getter, Getter getter>
  inline void registerReadonlyInstanceProperty() { }

  template<typename T>
  inline void registerReadonlyInstanceProperty(kj::StringPtr name, T value) { }

  template<const char* name, typename Getter, Getter getter>
  inline void registerReadonlyPrototypeProperty() { }


  template<const char* name, typename Getter, Getter getter, bool readOnly>
  inline void registerLazyInstanceProperty() { }


  template<const char* name, typename T>
  inline void registerStaticConstant(T value) { }

  template<const char* name, typename Method, Method method>
  inline void registerIterable() { }

  template<const char* name, typename Method, Method method>
  inline void registerAsyncIterable() { }

  template<typename Type, const char* name>
  inline void registerNestedType() { }

  kj::ArrayPtr<const char> findModule(Bundle::Reader bundle, kj::StringPtr moduleName) {
    for (auto module: bundle.getModules()) {
      if (module.getName() == moduleName) {
        return module.getSrc().asChars();
      }
    }

    KJ_FAIL_REQUIRE("Module not found", moduleName);
  }

  inline void registerNestedJsType(Bundle::Reader bundle, kj::StringPtr moduleName, kj::StringPtr typeName) {
    // // Must pass true for `is_module`, but we can skip everything else.
    const int resourceLineOffset = 0;
    const int resourceColumnOffset = 0;
    const bool resourceIsSharedCrossOrigin = false;
    const int scriptId = -1;
    const bool resourceIsOpaque = false;
    const bool isWasm = false;
    const bool isModule = true;
    v8::ScriptOrigin origin(isolate,
                            v8StrIntern(isolate, moduleName),
                            resourceLineOffset,
                            resourceColumnOffset,
                            resourceIsSharedCrossOrigin, scriptId, {},
                            resourceIsOpaque, isWasm, isModule);
    v8::Local<v8::String> contentStr;
    contentStr = jsg::v8Str(isolate, findModule(bundle, moduleName));
    v8::ScriptCompiler::Source source(contentStr, origin);
    auto module = jsg::check(v8::ScriptCompiler::CompileModule(isolate, &source));
    jsg::instantiateModule(isolate, context, module);

    auto moduleNs = module->GetModuleNamespace()->ToObject(context).ToLocalChecked();

    auto names = check(moduleNs->GetPropertyNames(context,
        v8::KeyCollectionMode::kOwnOnly,
        v8::ALL_PROPERTIES,
        v8::IndexFilter::kIncludeIndices));
    auto global = context->Global();

    for (auto i: kj::zeroTo(names->Length())) {
      auto name = check(names->Get(context, i));
      KJ_ASSERT(check(global->Set(context, name, check(moduleNs->Get(context, name)))));
    }
  }

  inline void registerTypeScriptRoot() { /* only needed for RTTI */ }

  template<const char* tsOverride>
  inline void registerTypeScriptOverride() { /* only needed for RTTI */ }

  template<const char* tsDefine>
  inline void registerTypeScriptDefine() { /* only needed for RTTI */ }

private:
  v8::Isolate* isolate;
  v8::Local<v8::Context> context;
};


template <typename TypeWrapper, typename T>
class ResourceWrapper {
  // TypeWrapper mixin for resource types (application-defined C++ classes declared with a
  // JSG_RESOURCE_TYPE block).

public:
  using Configuration = DetectedOr<NullConfiguration, GetConfiguration, T>;
  // If the JSG_RESOURCE_TYPE macro declared a configuration parameter, then `Configuration` will
  // be that type, otherwise NullConfiguration which accepts any configuration.

  template <typename MetaConfiguration>
  ResourceWrapper(MetaConfiguration&& configuration)
      : configuration(kj::fwd<MetaConfiguration>(configuration)) { }

  inline void initTypeWrapper() {
    static_cast<TypeWrapper&>(*this).resourceTypeMap.insert(typeid(T),
        [](TypeWrapper& wrapper, v8::Isolate* isolate)
        -> typename DynamicResourceTypeMap<TypeWrapper>::DynamicTypeInfo {
      kj::Maybe<typename DynamicResourceTypeMap<TypeWrapper>::ReflectionInitializer&> rinit;
      if constexpr (T::jsgHasReflection) {
        rinit = [](jsg::Object& object, TypeWrapper& wrapper) {
          static_cast<T&>(object).jsgInitReflection(wrapper);
        };
      }
      return { wrapper.getTemplate(isolate, (T*)nullptr), rinit };
    });
  }

  static constexpr const std::type_info& getName(T*) { return typeid(T); }

  // `Ref<T>` is NOT a resource type -- TypeHandler<Ref<T>> should use the value-oriented
  // implementation.
  static constexpr const std::type_info& getName(Ref<T>*) { return typeid(T); }

  v8::Local<v8::Object> wrap(
      v8::Local<v8::Context> context, kj::Maybe<v8::Local<v8::Object>> creator,
      Ref<T>&& value) {
    // Wrap a value of type T.

    auto isolate = context->GetIsolate();

    KJ_IF_MAYBE(h, value->tryGetHandle(isolate)) {
      return *h;
    } else {
      auto& type = typeid(*value);
      auto& wrapper = static_cast<TypeWrapper&>(*this);
      // Check if *value is actually a subclass of T. If so, we need to dynamically look up the
      // correct wrapper. But in the common case that it's exactly T, we can skip the lookup.
      v8::Local<v8::FunctionTemplate> tmpl;
      if (type == typeid(T)) {
        tmpl = getTemplate(isolate, nullptr);
        if constexpr (T::jsgHasReflection) {
          value->jsgInitReflection(wrapper);
        }
      } else {
        auto info = wrapper.getDynamicTypeInfo(isolate, type);
        tmpl = info.tmpl;
        KJ_IF_MAYBE(i, info.reflectionInitializer) {
          (*i)(*value, wrapper);
        }
      }
      v8::Local<v8::Object> object = check(tmpl->InstanceTemplate()->NewInstance(context));
      value.attachWrapper(isolate, object);
      return object;
    }
  }

  template <typename... Args>
  JsContext<T> newContext(v8::Isolate* isolate, T*, Args&&... args) {
    // Construct an instance of this type to be used as the Javascript global object, creating
    // a new JavaScript context. Unfortunately, we have to do some things differently in this
    // case, because of quirks in how V8 handles the global object. There appear to be bugs
    // that prevent it from being treated uniformly for callback purposes. See:
    //
    //     https://groups.google.com/d/msg/v8-users/RET5b3KOa5E/3EvpRBzwAQAJ
    //
    // Because of this, our entire type registration system threads through an extra template
    // parameter `bool isContext`. When the application decides to create a context using this
    // type as the global, we instantiate this separate branch specifically for that type.
    // Fortunately, for types that are never used as the global object, we never have to
    // instantiate the `isContext = true` branch.

    auto tmpl = getTemplate<true>(isolate, nullptr)->InstanceTemplate();
    v8::Local<v8::Context> context = v8::Context::New(isolate, nullptr, tmpl);
    auto global = context->Global();

    auto ptr = jsg::alloc<T>(kj::fwd<Args>(args)...);
    if constexpr (T::jsgHasReflection) {
      ptr->jsgInitReflection(static_cast<TypeWrapper&>(*this));
    }
    ptr.attachWrapper(isolate, global);

    // Disable `eval(code)` and `new Function(code)`. (Actually, setting this to `false` really
    // means "call the callback registered on the isolate to check" -- setting it to `true` means
    // "skip callback and just allow".)
    context->AllowCodeGenerationFromStrings(false);

    // We do not allow use of WeakRef or FinalizationRegistry because they introduce
    // non-deterministic behavior.
    check(global->Delete(context, v8StrIntern(isolate, "WeakRef"_kj)));
    check(global->Delete(context, v8StrIntern(isolate, "FinalizationRegistry"_kj)));

    // Store a pointer to this object in slot 1, to be extracted in callbacks.
    context->SetAlignedPointerInEmbedderData(1, ptr.get());

    // (Note: V8 docs say: "Note that index 0 currently has a special meaning for Chrome's
    // debugger." We aren't Chrome, but it does appear that some versions of V8 will mess with
    // slot 0, causing us to segfault if we try to put anything there. So we avoid it and use slot
    // 1, which seems to work just fine.)

    // Expose the type of the global scope in the global scope itself.
    exposeGlobalScopeType(isolate, context);

    v8::Context::Scope context_scope(context);

    registerJsTypes(isolate, context);

    return JsContext<T>(context, kj::mv(ptr));
  }

  kj::Maybe<T&> tryUnwrap(
      v8::Local<v8::Context> context, v8::Local<v8::Value> handle, T*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    // Try to unwrap a value of type T.

    if (handle->IsObject()) {
      v8::Local<v8::Object> instance = v8::Local<v8::Object>::Cast(handle)
          ->FindInstanceInPrototypeChain(getTemplate(context->GetIsolate(), nullptr));
      if (!instance.IsEmpty()) {
        return extractInternalPointer<T, false>(context, instance);
      }
    }

    return nullptr;
  }

  kj::Maybe<Ref<T>> tryUnwrap(
      v8::Local<v8::Context> context, v8::Local<v8::Value> handle, Ref<T>*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    // Try to unwrap a value of type Ref<T>.

    KJ_IF_MAYBE(p, tryUnwrap(context, handle, (T*)nullptr, parentObject)) {
      return Ref<T>(kj::addRef(*p));
    } else {
      return nullptr;
    }
  }

  template <bool isContext = false>
  v8::Local<v8::FunctionTemplate> getTemplate(v8::Isolate* isolate, T*) {
    v8::Global<v8::FunctionTemplate>& slot = isContext ? contextConstructor : memoizedConstructor;
    if (slot.IsEmpty()) {
      auto result = makeConstructor<isContext>(isolate);
      slot.Reset(isolate, result);
      return result;
    } else {
      return slot.Get(isolate);
    }
  }

private:
  Configuration configuration;
  v8::Global<v8::FunctionTemplate> memoizedConstructor;
  v8::Global<v8::FunctionTemplate> contextConstructor;

  template <bool isContext>
  v8::Local<v8::FunctionTemplate> makeConstructor(v8::Isolate* isolate) {
    // Construct lazily.
    v8::EscapableHandleScope scope(isolate);

    v8::Local<v8::FunctionTemplate> constructor;
    if constexpr(!isContext && hasConstructorMethod((T*)nullptr)) {
      constructor = v8::FunctionTemplate::New(
          isolate, &ConstructorCallback<TypeWrapper, T>::callback);
    } else {
      constructor = v8::FunctionTemplate::New(isolate, &throwIllegalConstructor);
    }

    auto prototype = constructor->PrototypeTemplate();

    // Signatures protect our methods from being invoked with the wrong `this`.
    auto signature = v8::Signature::New(isolate, constructor);

    auto instance = constructor->InstanceTemplate();

    instance->SetInternalFieldCount(Wrappable::INTERNAL_FIELD_COUNT);

    constructor->SetClassName(v8StrIntern(isolate, typeName(typeid(T))));

    static_assert(kj::isSameType<typename T::jsgThis, T>(),
        "Name passed to JSG_RESOURCE_TYPE() must be the class's own name.");

    auto& typeWrapper = static_cast<TypeWrapper&>(*this);

    auto& js = Lock::from(isolate);
    ResourceTypeBuilder<TypeWrapper, T, isContext> builder(
        js, typeWrapper, isolate, isolate->GetCurrentContext(), constructor, instance, prototype, signature);

    if constexpr (isDetected<GetConfiguration, T>()) {
      T::template registerMembers<decltype(builder), T>(builder, configuration);
    } else {
      T::template registerMembers<decltype(builder), T>(builder);
    }

    return scope.Escape(constructor);
  }

  void registerJsTypes(v8::Isolate* isolate, v8::Local<v8::Context> context) {
    JsTypesLoader<TypeWrapper, T> loader(isolate, context);

    if constexpr (isDetected<GetConfiguration, T>()) {
      T::template registerMembers<decltype(loader), T>(loader, configuration);
    } else {
      T::template registerMembers<decltype(loader), T>(loader);
    }
  }

  template <typename, typename, typename, typename>
  friend struct ConstructorCallback;
};

template <typename Self, typename T>
class TypeWrapperBase<Self, T, JsgKind::RESOURCE>
    : public ResourceWrapper<Self, T> {
  // Specialization of TypeWrapperBase for types that have a JSG_RESOURCE_TYPE block.

public:
  template <typename MetaConfiguration>
  TypeWrapperBase(MetaConfiguration& config)
      : ResourceWrapper<Self, T>(config) {}

  void unwrap() = delete;  // ResourceWrapper only implements tryUnwrap(), not unwrap()
};

} // namespace workerd::jsg
