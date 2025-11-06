#include "ffi.h"

#include <workerd/jsg/util.h>
#include <workerd/jsg/wrappable.h>
#include <workerd/rust/jsg/lib.rs.h>

#include <kj/common.h>

using namespace kj_rs;

namespace workerd::rust::jsg {

size_t create_resource_template(v8::Isolate* isolate, const ResourceDescriptor& descriptor) {
  // Construct lazily.
  v8::EscapableHandleScope scope(isolate);

  v8::Local<v8::FunctionTemplate> constructor;
  KJ_IF_SOME(descriptor, descriptor.constructor) {
    constructor = v8::FunctionTemplate::New(isolate,
        reinterpret_cast<v8::FunctionCallback>(reinterpret_cast<void*>(descriptor.callback)));
  } else {
    constructor = v8::FunctionTemplate::New(isolate, &workerd::jsg::throwIllegalConstructor);
  }

  auto prototype = constructor->PrototypeTemplate();

  // Signatures protect our methods from being invoked with the wrong `this`.
  auto signature = v8::Signature::New(isolate, constructor);

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
        v8::Local<v8::Value>(), signature, 0, v8::ConstructorBehavior::kThrow);
    prototype->Set(::workerd::jsg::v8StrIntern(isolate, kj::str(method.name)), functionTemplate);
  }

  auto result = scope.Escape(constructor);
  // slot.Reset(isolate, result);
  return to_ffi(v8::Global<v8::FunctionTemplate>(isolate, result));
}

LocalValue wrap_resource(Isolate* isolate, size_t resource, LocalFunctionTemplate tmpl) {
  auto self = reinterpret_cast<void*>(resource);
  auto local_tmpl = local_from_ffi<v8::FunctionTemplate>(tmpl);
  v8::Local<v8::Object> object = workerd::jsg::check(
      local_tmpl->InstanceTemplate()->NewInstance(isolate->GetCurrentContext()));
  auto tagAddress = const_cast<uint16_t*>(&::workerd::jsg::Wrappable::WORKERD_RUST_WRAPPABLE_TAG);
  object->SetAlignedPointerInInternalField(::workerd::jsg::Wrappable::WRAPPABLE_TAG_FIELD_INDEX,
      tagAddress,
      static_cast<v8::EmbedderDataTypeTag>(::workerd::jsg::Wrappable::WRAPPABLE_TAG_FIELD_INDEX));
  object->SetAlignedPointerInInternalField(::workerd::jsg::Wrappable::WRAPPED_OBJECT_FIELD_INDEX,
      self,
      static_cast<v8::EmbedderDataTypeTag>(::workerd::jsg::Wrappable::WRAPPED_OBJECT_FIELD_INDEX));
  return to_ffi(kj::mv(object));
}

size_t unwrap_resource(Isolate* isolate, LocalValue value) {
  auto v8_obj = local_from_ffi<v8::Object>(value);
  KJ_ASSERT(v8_obj->GetAlignedPointerFromInternalField(
                ::workerd::jsg::Wrappable::WRAPPABLE_TAG_FIELD_INDEX) ==
      const_cast<uint16_t*>(&::workerd::jsg::Wrappable::WORKERD_RUST_WRAPPABLE_TAG));
  return reinterpret_cast<size_t>(v8_obj->GetAlignedPointerFromInternalField(
      ::workerd::jsg::Wrappable::WRAPPED_OBJECT_FIELD_INDEX));
}

LocalObject new_local_object(Isolate* isolate) {
  v8::Local<v8::Object> object = v8::Object::New(isolate);
  return to_ffi(kj::mv(object));
}

void set_local_object_property(
    Isolate* isolate, LocalObject object, ::rust::Str key, LocalValue value) {
  auto v8_obj = local_from_ffi<v8::Object>(object);
  [[maybe_unused]] auto result = v8_obj->Set(isolate->GetCurrentContext(),
      v8::String::NewFromUtf8(isolate, key.cbegin(), v8::NewStringType::kInternalized, key.size())
          .ToLocalChecked(),
      local_from_ffi<v8::Value>(value));
}

void set_return_value(FunctionCallbackInfo* args, LocalValue value) {
  args->GetReturnValue().Set(local_from_ffi<v8::Value>(value));
}

LocalValue new_local_number(Isolate* isolate, double value) {
  v8::Local<v8::Number> val = v8::Number::New(isolate, value);
  return to_ffi(kj::mv(val));
}

LocalValue new_local_string(Isolate* isolate, ::rust::Str value) {
  v8::Local<v8::String> val =
      v8::String::NewFromUtf8(isolate, value.cbegin(), v8::NewStringType::kNormal, value.size())
          .ToLocalChecked();
  return to_ffi(kj::mv(val));
}

GlobalValue local_to_global_value(Isolate* isolate, LocalValue value) {
  v8::Global<v8::Value> global(isolate, local_from_ffi<v8::Value>(value));
  return to_ffi(kj::mv(global));
}

LocalValue global_to_local_value(Isolate* isolate, GlobalValue value) {
  v8::Global<v8::Value> glbl = global_from_ffi<v8::Value>(value);
  v8::Local<v8::Value> local = v8::Local<v8::Value>::New(isolate, glbl);
  return to_ffi(kj::mv(local));
}

LocalValue clone_local_value(Isolate* isolate, LocalValue value) {
  auto ptr_void = reinterpret_cast<void*>(&value);
  auto local = *reinterpret_cast<v8::Local<v8::Value>*>(ptr_void);
  auto copy = v8::Local<v8::Value>::New(isolate, local);
  // Forget local so it doesn't get destroyed when the scope is finished.
  [[maybe_unused]] auto forgot = to_ffi(kj::mv(local));
  return to_ffi(kj::mv(copy));
}

GlobalValue clone_global_value(Isolate* isolate, GlobalValue value) {
  auto ptr_void = reinterpret_cast<void*>(&value);
  auto glbl = kj::mv(*reinterpret_cast<v8::Global<v8::Value>*>(ptr_void));
  auto copy = v8::Global<v8::Value>(isolate, glbl);
  // Forget glbl so it doesn't get destroyed when the scope is finished.
  [[maybe_unused]] auto forgot = to_ffi(kj::mv(glbl));
  return to_ffi(kj::mv(copy));
}

bool eq_local_value(LocalValue lhs, LocalValue rhs) {
  return local_from_ffi<v8::Value>(lhs) == local_from_ffi<v8::Value>(rhs);
}

}  // namespace workerd::rust::jsg
