#pragma once

#include <workerd/jsg/modules.h>
#include <rust/cxx.h>
#include <kj-rs/kj-rs.h>

namespace workerd::rust::jsg {
  using LocalValue = v8::Local<v8::Value>;
  using V8Isolate = v8::Isolate;
  using ModuleCallback = kj::Function<void>();

  struct ModuleRegistry {
    virtual ~ModuleRegistry() = default;
    virtual void addBuiltinModule(::rust::Str specifier) = 0;
  };

  template <typename Registry>
  struct RustModuleRegistry : public ::workerd::rust::jsg::ModuleRegistry {
    virtual ~RustModuleRegistry() = default;
    RustModuleRegistry(Registry& registry) : registry(registry) {}
    void addBuiltinModule(::rust::Str specifier) override {
      auto kj_specifier = kj::str(specifier);
      registry.addBuiltinModule(kj_specifier, [&kj_specifier](::workerd::jsg::Lock& js, ::workerd::jsg::ModuleRegistry::ResolveMethod, kj::Maybe<const kj::Path&>&) mutable -> kj::Maybe<::workerd::jsg::ModuleRegistry::ModuleInfo> {
        // auto isolate = js.v8Isolate;
        KJ_UNIMPLEMENTED();
        // return kj::Maybe(ModuleInfo(js, kj_specifier, kj::none, ObjectModuleInfo(js, wrap)));
      }, ::workerd::jsg::ModuleType::BUILTIN);
    }
    v8::Local<v8::FunctionTemplate> getTemplate(v8::Isolate* isolate) {
      KJ_UNIMPLEMENTED();
      // v8::Global<v8::FunctionTemplate>& slot = memoizedConstructor;
      // if (slot.IsEmpty()) {
      //   // Construct lazily.
      //   v8::EscapableHandleScope scope(isolate);

      //   v8::Local<v8::FunctionTemplate> constructor;
      //   if constexpr (!isContext && hasConstructorMethod((T*)nullptr)) {
      //     constructor =
      //         v8::FunctionTemplate::New(isolate, &ConstructorCallback<TypeWrapper, T>::callback);
      //   } else {
      //     constructor = v8::FunctionTemplate::New(isolate, &throwIllegalConstructor);
      //   }

      //   auto prototype = constructor->PrototypeTemplate();

      //   // Signatures protect our methods from being invoked with the wrong `this`.
      //   auto signature = v8::Signature::New(isolate, constructor);

      //   auto instance = constructor->InstanceTemplate();

      //   instance->SetInternalFieldCount(Wrappable::INTERNAL_FIELD_COUNT);

      //   auto classname = v8StrIntern(isolate, typeName(typeid(T)));

      //   if (getShouldSetToStringTag(isolate)) {
      //     prototype->Set(
      //         v8::Symbol::GetToStringTag(isolate), classname, v8::PropertyAttribute::DontEnum);
      //   }

      //   // Previously, miniflare would use the lack of a Symbol.toStringTag on a class to
      //   // detect a type that came from the runtime. That's obviously a bit problematic because
      //   // Symbol.toStringTag is required for full compliance on standard web platform APIs.
      //   // To help use cases where it is necessary to detect if a class is a runtime class, we
      //   // will add a special symbol to the prototype of the class to indicate. Note that
      //   // because this uses the global symbol registry user code could still mark their own
      //   // classes with this symbol but that's unlikely to be a problem in any practical case.
      //   auto internalMarker =
      //       v8::Symbol::For(isolate, v8StrIntern(isolate, "cloudflare:internal-class"));
      //   prototype->Set(internalMarker, internalMarker,
      //       static_cast<v8::PropertyAttribute>(v8::PropertyAttribute::DontEnum |
      //           v8::PropertyAttribute::DontDelete | v8::PropertyAttribute::ReadOnly));

      //   constructor->SetClassName(classname);

      //   static_assert(kj::isSameType<typename T::jsgThis, T>(),
      //       "Name passed to JSG_RESOURCE_TYPE() must be the class's own name.");

      //   auto& typeWrapper = static_cast<TypeWrapper&>(*this);

      //   // ResourceTypeBuilder<TypeWrapper, T, isContext> builder(
      //   //     typeWrapper, isolate, constructor, instance, prototype, signature);

      //   // if constexpr (isDetected<GetConfiguration, T>()) {
      //   //   T::template registerMembers<decltype(builder), T>(builder, configuration);
      //   // } else {
      //   //   T::template registerMembers<decltype(builder), T>(builder);
      //   // }

      //   // auto result = scope.Escape(constructor);
      //   // slot.Reset(isolate, result);
      //   // return result;
      // } else {
      //   return slot.Get(isolate);
      // }
    }
    Registry& registry;
  };

  inline void register_add_builtin_module(ModuleRegistry& registry, ::rust::Str specifier) {
    registry.addBuiltinModule(specifier);
  }
}
