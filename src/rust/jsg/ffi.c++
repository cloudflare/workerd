#include "ffi.h"

#include <workerd/jsg/util.h>
#include <workerd/jsg/wrappable.h>
#include <workerd/rust/jsg/lib.rs.h>

#include <kj/common.h>

using namespace kj_rs;

namespace workerd::rust::jsg {

size_t create_resource_template(v8::Isolate* isolate, const ResourceDescriptor& descriptor) {
  // Construct lazily.
  // v8::EscapableHandleScope scope(isolate);

  v8::Local<v8::FunctionTemplate> constructor;
  KJ_IF_SOME(c, descriptor.constructor) {
    KJ_UNIMPLEMENTED("constructors are not implemented yet");
    // constructor =
    // v8::FunctionTemplate::New(isolate, &ConstructorCallback<TypeWrapper, T>::callback);
  } else {
    constructor = v8::FunctionTemplate::New(isolate, &workerd::jsg::throwIllegalConstructor);
  }

  auto prototype = constructor->PrototypeTemplate();

  // Signatures protect our methods from being invoked with the wrong `this`.
  // auto signature = v8::Signature::New(isolate, constructor);

  auto instance = constructor->InstanceTemplate();

  instance->SetInternalFieldCount(workerd::jsg::Wrappable::INTERNAL_FIELD_COUNT);

  auto classname = ::workerd::jsg::v8StrIntern(isolate, kj::str(descriptor.name));

  if (workerd::jsg::getShouldSetToStringTag(isolate)) {
    prototype->Set(v8::Symbol::GetToStringTag(isolate), classname, v8::PropertyAttribute::DontEnum);
  }

  // Previously, miniflare would use the lack of a Symbol.toStringTag on a class to
  // detect a type that came from the runtime. That's obviously a bit problematic because
  // Symbol.toStringTag is required for full compliance on standard web platform APIs.
  // To help use cases where it is necessary to detect if a class is a runtime class, we
  // will add a special symbol to the prototype of the class to indicate. Note that
  // because this uses the global symbol registry user code could still mark their own
  // classes with this symbol but that's unlikely to be a problem in any practical case.
  auto internalMarker =
      v8::Symbol::For(isolate, ::workerd::jsg::v8StrIntern(isolate, "cloudflare:internal-class"));
  prototype->Set(internalMarker, internalMarker,
      static_cast<v8::PropertyAttribute>(v8::PropertyAttribute::DontEnum |
          v8::PropertyAttribute::DontDelete | v8::PropertyAttribute::ReadOnly));

  constructor->SetClassName(classname);

  // auto& typeWrapper = static_cast<TypeWrapper&>(*this);

  // ResourceTypeBuilder<TypeWrapper, T, isContext> builder(
  //     typeWrapper, isolate, constructor, instance, prototype, signature);

  // if constexpr (isDetected<GetConfiguration, T>()) {
  //   T::template registerMembers<decltype(builder), T>(builder, configuration);
  // } else {
  //   T::template registerMembers<decltype(builder), T>(builder);
  // }

  for (const auto& method: descriptor.static_methods) {
    auto functionTemplate = v8::FunctionTemplate::New(isolate,
        reinterpret_cast<v8::FunctionCallback>(reinterpret_cast<void*>(method.callback)),
        v8::Local<v8::Value>(), v8::Local<v8::Signature>(), 0, v8::ConstructorBehavior::kThrow);
    functionTemplate->RemovePrototype();
    constructor->Set(::workerd::jsg::v8StrIntern(isolate, kj::str(method.name)), functionTemplate);
  }

  for (const auto& method: descriptor.methods) {
    auto functionTemplate = v8::FunctionTemplate::New(isolate,
        reinterpret_cast<v8::FunctionCallback>(reinterpret_cast<void*>(method.callback)),
        v8::Local<v8::Value>(), v8::Local<v8::Signature>(), 0, v8::ConstructorBehavior::kThrow);
    prototype->Set(::workerd::jsg::v8StrIntern(isolate, kj::str(method.name)), functionTemplate);
  }

  // auto result = scope.Escape(constructor);
  // slot.Reset(isolate, result);
  return to_ffi(v8::Global<v8::FunctionTemplate>(isolate, constructor));
}

LocalValue wrap_resource(Isolate* isolate, size_t resource, LocalFunctionTemplate tmpl) {
  auto local_tmpl = local_from_ffi<v8::FunctionTemplate>(tmpl);
  v8::Local<v8::Object> obj = workerd::jsg::check(
      local_tmpl->InstanceTemplate()->NewInstance(isolate->GetCurrentContext()));
  // TODO: Attach to the wrapper (call attachWrapper)
  return to_ffi(kj::mv(obj));
}

}  // namespace workerd::rust::jsg
