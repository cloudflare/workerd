#include "ffi.h"

#include <workerd/jsg/util.h>
#include <workerd/jsg/wrappable.h>
#include <workerd/rust/jsg/ffi-inl.h>
#include <workerd/rust/jsg/lib.rs.h>
#include <workerd/rust/jsg/v8.rs.h>

#include <kj/common.h>

using namespace kj_rs;

namespace workerd::rust::jsg {

// Local<T>
void local_drop(Local value) {
  // Convert from FFI representation and let v8::Local destructor handle cleanup
  local_from_ffi<v8::Value>(kj::mv(value));
}

Local local_clone(const Local& value) {
  return Local{.ptr = value.ptr};
}

Global local_to_global(Isolate* isolate, Local value) {
  v8::Global<v8::Value> global(isolate, local_from_ffi<v8::Value>(kj::mv(value)));
  return to_ffi(kj::mv(global));
}

Local local_new_number(Isolate* isolate, double value) {
  v8::Local<v8::Number> val = v8::Number::New(isolate, value);
  return to_ffi(kj::mv(val));
}

Local local_new_string(Isolate* isolate, ::rust::Str value) {
  auto val = ::workerd::jsg::check(
      v8::String::NewFromUtf8(isolate, value.cbegin(), v8::NewStringType::kNormal, value.size()));
  return to_ffi(kj::mv(val));
}

Local local_new_object(Isolate* isolate) {
  v8::Local<v8::Object> object = v8::Object::New(isolate);
  return to_ffi(kj::mv(object));
}

bool local_eq(const Local& lhs, const Local& rhs) {
  return local_as_ref_from_ffi<v8::Value>(lhs) == local_as_ref_from_ffi<v8::Value>(rhs);
}

bool local_has_value(const Local& val) {
  return *local_as_ref_from_ffi<v8::Value>(val) != nullptr;
}

bool local_is_string(const Local& val) {
  return local_as_ref_from_ffi<v8::Value>(val)->IsString();
}

// Local<Object>
void local_object_set_property(Isolate* isolate, Local& object, ::rust::Str key, Local value) {
  auto v8_obj = local_as_ref_from_ffi<v8::Object>(object);
  auto context = isolate->GetCurrentContext();
  auto v8_key = ::workerd::jsg::check(
      v8::String::NewFromUtf8(isolate, key.cbegin(), v8::NewStringType::kInternalized, key.size()));
  ::workerd::jsg::check(v8_obj->Set(context, v8_key, local_from_ffi<v8::Value>(kj::mv(value))));
}

bool local_object_has_property(Isolate* isolate, const Local& object, ::rust::Str key) {
  auto v8_obj = local_as_ref_from_ffi<v8::Object>(object);
  auto context = isolate->GetCurrentContext();
  auto v8_key = ::workerd::jsg::check(
      v8::String::NewFromUtf8(isolate, key.cbegin(), v8::NewStringType::kInternalized, key.size()));
  return v8_obj->Has(context, v8_key).FromJust();
}

kj::Maybe<Local> local_object_get_property(Isolate* isolate, const Local& object, ::rust::Str key) {
  auto v8_obj = local_as_ref_from_ffi<v8::Object>(object);
  auto context = isolate->GetCurrentContext();
  auto v8_key = ::workerd::jsg::check(
      v8::String::NewFromUtf8(isolate, key.cbegin(), v8::NewStringType::kInternalized, key.size()));
  v8::Local<v8::Value> result;
  if (!v8_obj->Get(context, v8_key).ToLocal(&result)) {
    return kj::none;
  }
  return to_ffi(kj::mv(result));
}

// Wrappers
Local wrap_resource(Isolate* isolate, size_t resource, const Global& tmpl) {
  auto self = reinterpret_cast<void*>(resource);
  auto& global_tmpl = global_as_ref_from_ffi<v8::FunctionTemplate>(tmpl);
  auto local_tmpl = v8::Local<v8::FunctionTemplate>::New(isolate, global_tmpl);
  v8::Local<v8::Object> object = ::workerd::jsg::check(
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

// Unwrappers
::rust::String unwrap_string(Isolate* isolate, Local value) {
  v8::Local<v8::String> v8Str = ::workerd::jsg::check(
      local_from_ffi<v8::Value>(kj::mv(value))->ToString(isolate->GetCurrentContext()));
  v8::String::ValueView view(isolate, v8Str);
  if (!view.is_one_byte()) {
    return ::rust::String(reinterpret_cast<const char16_t*>(view.data16()), view.length());
  }
  return ::rust::String::latin1(reinterpret_cast<const char*>(view.data8()), view.length());
}

size_t unwrap_resource(Isolate* isolate, Local value) {
  auto v8_obj = local_from_ffi<v8::Object>(kj::mv(value));
  KJ_ASSERT(v8_obj->GetAlignedPointerFromInternalField(
                ::workerd::jsg::Wrappable::WRAPPABLE_TAG_FIELD_INDEX) ==
      const_cast<uint16_t*>(&::workerd::jsg::Wrappable::WORKERD_RUST_WRAPPABLE_TAG));
  return reinterpret_cast<size_t>(v8_obj->GetAlignedPointerFromInternalField(
      ::workerd::jsg::Wrappable::WRAPPED_OBJECT_FIELD_INDEX));
}

// Global<T>

void global_drop(Global value) {
  global_from_ffi<v8::Value>(kj::mv(value));
}

Global global_clone(const Global& value) {
  return Global{.ptr = value.ptr};
}

Local global_to_local(Isolate* isolate, const Global& value) {
  auto& glbl = global_as_ref_from_ffi<v8::Value>(value);
  v8::Local<v8::Value> local = v8::Local<v8::Value>::New(isolate, glbl);
  return to_ffi(kj::mv(local));
}

void global_make_weak(Isolate* isolate, Global* value, size_t data) {
  auto glbl = global_as_ref_from_ffi<v8::Object>(*value);
  // Pass the State pointer directly to V8's weak callback.
  // When GC collects the object, we call back into Rust's invoke_weak_drop
  // which reads the drop_fn from the State and invokes it.
  glbl->SetWeak(reinterpret_cast<void*>(data), [](const v8::WeakCallbackInfo<void>& info) {
    auto state = reinterpret_cast<size_t>(info.GetParameter());
    invoke_weak_drop(state);
  }, v8::WeakCallbackType::kParameter);
}

// FunctionCallbackInfo
v8::Isolate* fci_get_isolate(FunctionCallbackInfo* args) {
  return args->GetIsolate();
}

Local fci_get_this(FunctionCallbackInfo* args) {
  return to_ffi(args->This());
}

size_t fci_get_length(FunctionCallbackInfo* args) {
  return args->Length();
}

Local fci_get_arg(FunctionCallbackInfo* args, size_t index) {
  return to_ffi((*args)[index]);
}

void fci_set_return_value(FunctionCallbackInfo* args, Local value) {
  args->GetReturnValue().Set(local_from_ffi<v8::Value>(kj::mv(value)));
}

Global create_resource_template(v8::Isolate* isolate, const ResourceDescriptor& descriptor) {
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

  auto classname = ::workerd::jsg::check(v8::String::NewFromUtf8(
      isolate, descriptor.name.data(), v8::NewStringType::kNormal, descriptor.name.size()));

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
    auto name = ::workerd::jsg::check(v8::String::NewFromUtf8(
        isolate, method.name.data(), v8::NewStringType::kInternalized, method.name.size()));
    constructor->Set(name, functionTemplate);
  }

  for (const auto& method: descriptor.methods) {
    auto functionTemplate = v8::FunctionTemplate::New(isolate,
        reinterpret_cast<v8::FunctionCallback>(reinterpret_cast<void*>(method.callback)),
        v8::Local<v8::Value>(), signature, 0, v8::ConstructorBehavior::kThrow);
    auto name = ::workerd::jsg::check(v8::String::NewFromUtf8(
        isolate, method.name.data(), v8::NewStringType::kInternalized, method.name.size()));
    prototype->Set(name, functionTemplate);
  }

  auto result = scope.Escape(constructor);
  return to_ffi(v8::Global<v8::FunctionTemplate>(isolate, result));
}

Realm* realm_from_isolate(Isolate* isolate) {
  auto realm = ::workerd::jsg::getAlignedPointerFromEmbedderData<Realm>(
      isolate->GetCurrentContext(), ::workerd::jsg::ContextPointerSlot::RUST_REALM);
  return &KJ_ASSERT_NONNULL(realm);
}

// Errors
Local exception_create(Isolate* isolate, ExceptionType exception_type, ::rust::Str description) {
  auto message = ::workerd::jsg::check(v8::String::NewFromUtf8(
      isolate, description.data(), v8::NewStringType::kInternalized, description.size()));
  switch (exception_type) {
    case ExceptionType::RangeError:
      return to_ffi(v8::Exception::RangeError(message));
    case ExceptionType::ReferenceError:
      return to_ffi(v8::Exception::ReferenceError(message));
    case ExceptionType::SyntaxError:
      return to_ffi(v8::Exception::SyntaxError(message));
    case ExceptionType::TypeError:
      return to_ffi(v8::Exception::TypeError(message));
    case ExceptionType::Error:
      return to_ffi(v8::Exception::Error(message));
    default:
      KJ_UNREACHABLE;
  }
}

// Isolate
void isolate_throw_exception(Isolate* isolate, Local exception) {
  isolate->ThrowException(local_from_ffi<v8::Value>(kj::mv(exception)));
}

void isolate_throw_error(Isolate* isolate, ::rust::Str description) {
  auto message = ::workerd::jsg::check(v8::String::NewFromUtf8(
      isolate, description.data(), v8::NewStringType::kInternalized, description.size()));
  isolate->ThrowError(message);
}

bool isolate_is_locked(Isolate* isolate) {
  return v8::Locker::IsLocked(isolate);
}

}  // namespace workerd::rust::jsg
