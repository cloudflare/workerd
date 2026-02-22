#include "ffi.h"

#include <workerd/jsg/jsg.h>
#include <workerd/jsg/util.h>
#include <workerd/jsg/wrappable.h>
#include <workerd/rust/jsg/ffi-inl.h>
#include <workerd/rust/jsg/lib.rs.h>
#include <workerd/rust/jsg/v8.rs.h>

#include <kj/common.h>

#include <memory>

using namespace kj_rs;

namespace workerd::rust::jsg {

#define DEFINE_TYPED_ARRAY_NEW(name, v8_type, elem_type)                                           \
  Local local_new_##name(Isolate* isolate, const elem_type* data, size_t length) {                 \
    auto backingStore = v8::ArrayBuffer::NewBackingStore(isolate, length * sizeof(elem_type));     \
    memcpy(backingStore->Data(), data, length * sizeof(elem_type));                                \
    auto arrayBuffer = v8::ArrayBuffer::New(isolate, std::move(backingStore));                     \
    return to_ffi(v8::v8_type::New(arrayBuffer, 0, length));                                       \
  }

#define DEFINE_TYPED_ARRAY_UNWRAP(name, v8_type, elem_type)                                        \
  ::rust::Vec<elem_type> unwrap_##name(Isolate* isolate, Local value) {                            \
    auto v8Val = local_from_ffi<v8::Value>(kj::mv(value));                                         \
    KJ_REQUIRE(v8Val->Is##v8_type());                                                              \
    auto typed = v8Val.As<v8::v8_type>();                                                          \
    ::rust::Vec<elem_type> result;                                                                 \
    result.reserve(typed->Length());                                                               \
    auto data = reinterpret_cast<elem_type*>(                                                      \
        static_cast<uint8_t*>(typed->Buffer()->Data()) + typed->ByteOffset());                     \
    for (size_t i = 0; i < typed->Length(); i++) {                                                 \
      result.push_back(data[i]);                                                                   \
    }                                                                                              \
    return result;                                                                                 \
  }

#define DEFINE_TYPED_ARRAY_GET(name, v8_type, elem_type)                                           \
  elem_type local_##name##_get(Isolate* isolate, const Local& array, size_t index) {               \
    auto typed = local_as_ref_from_ffi<v8::v8_type>(array);                                        \
    KJ_REQUIRE(index < typed->Length(), "index out of bounds");                                    \
    auto data = reinterpret_cast<elem_type*>(                                                      \
        static_cast<uint8_t*>(typed->Buffer()->Data()) + typed->ByteOffset());                     \
    return data[index];                                                                            \
  }

// =============================================================================

// RustResource implementation - calls into Rust via CXX bridge
RustResource::~RustResource() {
  cppgc_invoke_drop(this);
}

void RustResource::Trace(cppgc::Visitor* visitor) const {
  auto ffi_visitor = to_ffi(visitor);
  cppgc_invoke_trace(this, &ffi_visitor);
}

const char* RustResource::GetHumanReadableName() const {
  return cppgc_invoke_get_name(this);
}

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

Local local_new_boolean(Isolate* isolate, bool value) {
  v8::Local<v8::Boolean> val = v8::Boolean::New(isolate, value);
  return to_ffi(kj::mv(val));
}

Local local_new_object(Isolate* isolate) {
  v8::Local<v8::Object> object = v8::Object::New(isolate);
  return to_ffi(kj::mv(object));
}

Local local_new_null(Isolate* isolate) {
  v8::Local<v8::Primitive> null = v8::Null(isolate);
  return to_ffi(kj::mv(null));
}

Local local_new_undefined(Isolate* isolate) {
  v8::Local<v8::Primitive> undefined = v8::Undefined(isolate);
  return to_ffi(kj::mv(undefined));
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

bool local_is_boolean(const Local& val) {
  return local_as_ref_from_ffi<v8::Value>(val)->IsBoolean();
}

bool local_is_number(const Local& val) {
  return local_as_ref_from_ffi<v8::Value>(val)->IsNumber();
}

bool local_is_null(const Local& val) {
  return local_as_ref_from_ffi<v8::Value>(val)->IsNull();
}

bool local_is_undefined(const Local& val) {
  return local_as_ref_from_ffi<v8::Value>(val)->IsUndefined();
}

bool local_is_null_or_undefined(const Local& val) {
  return local_as_ref_from_ffi<v8::Value>(val)->IsNullOrUndefined();
}

bool local_is_object(const Local& val) {
  return local_as_ref_from_ffi<v8::Value>(val)->IsObject();
}

bool local_is_native_error(const Local& val) {
  return local_as_ref_from_ffi<v8::Value>(val)->IsNativeError();
}

bool local_is_array(const Local& val) {
  return local_as_ref_from_ffi<v8::Value>(val)->IsArray();
}

bool local_is_uint8_array(const Local& val) {
  return local_as_ref_from_ffi<v8::Value>(val)->IsUint8Array();
}

bool local_is_uint16_array(const Local& val) {
  return local_as_ref_from_ffi<v8::Value>(val)->IsUint16Array();
}

bool local_is_uint32_array(const Local& val) {
  return local_as_ref_from_ffi<v8::Value>(val)->IsUint32Array();
}

bool local_is_int8_array(const Local& val) {
  return local_as_ref_from_ffi<v8::Value>(val)->IsInt8Array();
}

bool local_is_int16_array(const Local& val) {
  return local_as_ref_from_ffi<v8::Value>(val)->IsInt16Array();
}

bool local_is_int32_array(const Local& val) {
  return local_as_ref_from_ffi<v8::Value>(val)->IsInt32Array();
}

bool local_is_float32_array(const Local& val) {
  return local_as_ref_from_ffi<v8::Value>(val)->IsFloat32Array();
}

bool local_is_float64_array(const Local& val) {
  return local_as_ref_from_ffi<v8::Value>(val)->IsFloat64Array();
}

bool local_is_bigint64_array(const Local& val) {
  return local_as_ref_from_ffi<v8::Value>(val)->IsBigInt64Array();
}

bool local_is_biguint64_array(const Local& val) {
  return local_as_ref_from_ffi<v8::Value>(val)->IsBigUint64Array();
}

bool local_is_array_buffer(const Local& val) {
  return local_as_ref_from_ffi<v8::Value>(val)->IsArrayBuffer();
}

bool local_is_array_buffer_view(const Local& val) {
  return local_as_ref_from_ffi<v8::Value>(val)->IsArrayBufferView();
}

::rust::String local_type_of(Isolate* isolate, const Local& val) {
  auto v8Val = local_as_ref_from_ffi<v8::Value>(val);
  v8::Local<v8::String> typeStr = v8Val->TypeOf(isolate);
  v8::String::Utf8Value utf8(isolate, typeStr);
  return ::rust::String(*utf8, utf8.length());
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

// Local<Array>
Local local_new_array(Isolate* isolate, size_t length) {
  return to_ffi(v8::Array::New(isolate, length));
}

uint32_t local_array_length(Isolate* isolate, const Local& array) {
  return local_as_ref_from_ffi<v8::Array>(array)->Length();
}

Local local_array_get(Isolate* isolate, const Local& array, uint32_t index) {
  auto context = isolate->GetCurrentContext();
  auto v8Array = local_as_ref_from_ffi<v8::Array>(array);
  return to_ffi(::workerd::jsg::check(v8Array->Get(context, index)));
}

void local_array_set(Isolate* isolate, Local& array, uint32_t index, Local value) {
  auto context = isolate->GetCurrentContext();
  auto v8Array = local_as_ref_from_ffi<v8::Array>(array);
  ::workerd::jsg::check(v8Array->Set(context, index, local_from_ffi<v8::Value>(kj::mv(value))));
}

// TypedArray creation functions
DEFINE_TYPED_ARRAY_NEW(uint8_array, Uint8Array, uint8_t)
DEFINE_TYPED_ARRAY_NEW(uint16_array, Uint16Array, uint16_t)
DEFINE_TYPED_ARRAY_NEW(uint32_array, Uint32Array, uint32_t)
DEFINE_TYPED_ARRAY_NEW(int8_array, Int8Array, int8_t)
DEFINE_TYPED_ARRAY_NEW(int16_array, Int16Array, int16_t)
DEFINE_TYPED_ARRAY_NEW(int32_array, Int32Array, int32_t)
DEFINE_TYPED_ARRAY_NEW(float32_array, Float32Array, float)
DEFINE_TYPED_ARRAY_NEW(float64_array, Float64Array, double)
DEFINE_TYPED_ARRAY_NEW(bigint64_array, BigInt64Array, int64_t)
DEFINE_TYPED_ARRAY_NEW(biguint64_array, BigUint64Array, uint64_t)

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

bool unwrap_boolean(Isolate* isolate, Local value) {
  return local_from_ffi<v8::Value>(kj::mv(value))->ToBoolean(isolate)->Value();
}

double unwrap_number(Isolate* isolate, Local value) {
  return ::workerd::jsg::check(
      local_from_ffi<v8::Value>(kj::mv(value))->ToNumber(isolate->GetCurrentContext()))
      ->Value();
}

size_t unwrap_resource(Isolate* isolate, Local value) {
  auto v8_obj = local_from_ffi<v8::Object>(kj::mv(value));
  KJ_ASSERT(v8_obj->GetAlignedPointerFromInternalField(
                ::workerd::jsg::Wrappable::WRAPPABLE_TAG_FIELD_INDEX,
                static_cast<v8::EmbedderDataTypeTag>(
                    ::workerd::jsg::Wrappable::WRAPPABLE_TAG_FIELD_INDEX)) ==
      const_cast<uint16_t*>(&::workerd::jsg::Wrappable::WORKERD_RUST_WRAPPABLE_TAG));
  return reinterpret_cast<size_t>(v8_obj->GetAlignedPointerFromInternalField(
      ::workerd::jsg::Wrappable::WRAPPED_OBJECT_FIELD_INDEX,
      static_cast<v8::EmbedderDataTypeTag>(::workerd::jsg::Wrappable::WRAPPED_OBJECT_FIELD_INDEX)));
}

// TypedArray unwrap functions
DEFINE_TYPED_ARRAY_UNWRAP(uint8_array, Uint8Array, uint8_t)
DEFINE_TYPED_ARRAY_UNWRAP(uint16_array, Uint16Array, uint16_t)
DEFINE_TYPED_ARRAY_UNWRAP(uint32_array, Uint32Array, uint32_t)
DEFINE_TYPED_ARRAY_UNWRAP(int8_array, Int8Array, int8_t)
DEFINE_TYPED_ARRAY_UNWRAP(int16_array, Int16Array, int16_t)
DEFINE_TYPED_ARRAY_UNWRAP(int32_array, Int32Array, int32_t)
DEFINE_TYPED_ARRAY_UNWRAP(float32_array, Float32Array, float)
DEFINE_TYPED_ARRAY_UNWRAP(float64_array, Float64Array, double)
DEFINE_TYPED_ARRAY_UNWRAP(bigint64_array, BigInt64Array, int64_t)
DEFINE_TYPED_ARRAY_UNWRAP(biguint64_array, BigUint64Array, uint64_t)

// Uses V8's Array::Iterate() which is faster than indexed access.
// Returns Global handles because Local handles get reused during iteration.
::rust::Vec<Global> local_array_iterate(Isolate* isolate, Local value) {
  auto context = isolate->GetCurrentContext();
  auto v8Val = local_from_ffi<v8::Value>(kj::mv(value));

  KJ_REQUIRE(v8Val->IsArray(), "Value must be an array");
  auto arr = v8Val.As<v8::Array>();

  struct Data {
    Isolate* isolate;
    ::rust::Vec<Global>* result;
  };

  ::rust::Vec<Global> result;
  result.reserve(arr->Length());
  Data data{isolate, &result};

  auto iterateResult = arr->Iterate(context,
      [](uint32_t index, v8::Local<v8::Value> element,
          void* userData) -> v8::Array::CallbackResult {
    auto* d = static_cast<Data*>(userData);
    d->result->push_back(to_ffi(v8::Global<v8::Value>(d->isolate, element)));
    return v8::Array::CallbackResult::kContinue;
  },
      &data);

  KJ_REQUIRE(iterateResult.IsJust(), "Iteration failed");
  return result;
}

// Local<TypedArray>
size_t local_typed_array_length(Isolate* isolate, const Local& array) {
  return local_as_ref_from_ffi<v8::TypedArray>(array)->Length();
}

// TypedArray element getter functions
DEFINE_TYPED_ARRAY_GET(uint8_array, Uint8Array, uint8_t)
DEFINE_TYPED_ARRAY_GET(uint16_array, Uint16Array, uint16_t)
DEFINE_TYPED_ARRAY_GET(uint32_array, Uint32Array, uint32_t)
DEFINE_TYPED_ARRAY_GET(int8_array, Int8Array, int8_t)
DEFINE_TYPED_ARRAY_GET(int16_array, Int16Array, int16_t)
DEFINE_TYPED_ARRAY_GET(int32_array, Int32Array, int32_t)
DEFINE_TYPED_ARRAY_GET(float32_array, Float32Array, float)
DEFINE_TYPED_ARRAY_GET(float64_array, Float64Array, double)
DEFINE_TYPED_ARRAY_GET(bigint64_array, BigInt64Array, int64_t)
DEFINE_TYPED_ARRAY_GET(biguint64_array, BigUint64Array, uint64_t)

// Global<T>
void global_reset(Global* value) {
  auto* glbl = global_as_ref_from_ffi<v8::Value>(*value);
  glbl->Reset();
}

Global global_clone(const Global& value) {
  return Global{.ptr = value.ptr};
}

Local global_to_local(Isolate* isolate, const Global& value) {
  auto& glbl = global_as_ref_from_ffi<v8::Value>(value);
  v8::Local<v8::Value> local = v8::Local<v8::Value>::New(isolate, glbl);
  return to_ffi(kj::mv(local));
}

// TracedReference
TracedReference traced_reference_from_local(Isolate* isolate, Local value) {
  v8::TracedReference<v8::Object> traced(isolate, local_from_ffi<v8::Object>(kj::mv(value)));
  return to_ffi(kj::mv(traced));
}

Local traced_reference_to_local(Isolate* isolate, const TracedReference& value) {
  auto& traced = traced_reference_as_ref_from_ffi<v8::Object>(value);
  return to_ffi(traced.Get(isolate));
}

void traced_reference_reset(TracedReference* value) {
  auto* traced = traced_reference_as_ref_from_ffi<v8::Object>(*value);
  traced->Reset();
}

bool traced_reference_is_empty(const TracedReference& value) {
  auto& traced = traced_reference_as_ref_from_ffi<v8::Object>(value);
  return traced.IsEmpty();
}

// FunctionCallbackInfo
Isolate* fci_get_isolate(FunctionCallbackInfo* args) {
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

Global create_resource_template(Isolate* isolate, const ResourceDescriptor& descriptor) {
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
  auto* realm =
      static_cast<Realm*>(isolate->GetData(::workerd::jsg::SetDataIndex::SET_DATA_RUST_REALM));
  KJ_ASSERT(realm != nullptr, "Rust Realm not set on isolate");
  return realm;
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
    default:
      // DOM-style exceptions (OperationError, DataError, etc.) and Error fall back to Error.
      // TODO(soon): Use js.domException() to create proper DOMException objects.
      return to_ffi(v8::Exception::Error(message));
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

// cppgc - Allocate Rust objects directly on the GC heap
size_t cppgc_rust_resource_size() {
  return sizeof(RustResource);
}

RustResource* cppgc_make_garbage_collected(Isolate* isolate, size_t size, size_t alignment) {
  auto* heap = isolate->GetCppHeap();
  KJ_ASSERT(heap != nullptr, "CppHeap not available on isolate");
  KJ_ASSERT(alignment <= 16, "Alignment {} exceeds maximum of 16", alignment);

  // Allocate RustResource with additional bytes for the Rust object.
  // The Rust object will be written into the space after the RustResource header.
  if (alignment <= 8) {
    return cppgc::MakeGarbageCollected<RustResource>(
        heap->GetAllocationHandle(), cppgc::AdditionalBytes(size));
  }

  return cppgc::MakeGarbageCollected<RustResourceAlign16>(
      heap->GetAllocationHandle(), cppgc::AdditionalBytes(size));
}

uintptr_t* cppgc_rust_resource_data(RustResource* resource) {
  return resource->data;
}

const uintptr_t* cppgc_rust_resource_data_const(const RustResource* resource) {
  return resource->data;
}

void cppgc_visitor_trace(CppgcVisitor* visitor, const TracedReference& handle) {
  auto* v8_visitor = cppgc_visitor_from_ffi(visitor);
  auto& traced = traced_reference_as_ref_from_ffi<v8::Object>(handle);
  v8_visitor->Trace(traced);
}

// Persistent inline storage functions
// Note: cppgc::Persistent stores an internal pointer to a PersistentNode, so it can be
// stored inline without issues. The internal node is heap-allocated by cppgc.

size_t cppgc_persistent_size() {
  return sizeof(CppgcPersistent);
}

void cppgc_persistent_construct(size_t storage, RustResource* resource) {
  new (reinterpret_cast<void*>(storage)) CppgcPersistent(resource);
}

void cppgc_persistent_destruct(size_t storage) {
  std::destroy_at(reinterpret_cast<CppgcPersistent*>(storage));
}

RustResource* cppgc_persistent_get(size_t storage) {
  return reinterpret_cast<const CppgcPersistent*>(storage)->Get();
}

void cppgc_persistent_assign(size_t storage, RustResource* resource) {
  *reinterpret_cast<CppgcPersistent*>(storage) = resource;
}

// WeakPersistent inline storage functions

size_t cppgc_weak_persistent_size() {
  return sizeof(CppgcWeakPersistent);
}

void cppgc_weak_persistent_construct(size_t storage, RustResource* resource) {
  new (reinterpret_cast<void*>(storage)) CppgcWeakPersistent(resource);
}

void cppgc_weak_persistent_destruct(size_t storage) {
  std::destroy_at(reinterpret_cast<CppgcWeakPersistent*>(storage));
}

RustResource* cppgc_weak_persistent_get(size_t storage) {
  return reinterpret_cast<const CppgcWeakPersistent*>(storage)->Get();
}

void cppgc_weak_persistent_assign(size_t storage, RustResource* resource) {
  *reinterpret_cast<CppgcWeakPersistent*>(storage) = resource;
}

// Member inline storage functions

size_t cppgc_member_size() {
  return sizeof(CppgcMember);
}

void cppgc_member_construct(size_t storage, RustResource* resource) {
  new (reinterpret_cast<void*>(storage)) CppgcMember(resource);
}

void cppgc_member_destruct(size_t storage) {
  std::destroy_at(reinterpret_cast<CppgcMember*>(storage));
}

RustResource* cppgc_member_get(size_t storage) {
  return reinterpret_cast<const CppgcMember*>(storage)->Get();
}

void cppgc_member_assign(size_t storage, RustResource* resource) {
  *reinterpret_cast<CppgcMember*>(storage) = resource;
}

void cppgc_visitor_trace_member(CppgcVisitor* visitor, size_t storage) {
  auto* v8_visitor = cppgc_visitor_from_ffi(visitor);
  v8_visitor->Trace(*reinterpret_cast<const CppgcMember*>(storage));
}

// WeakMember inline storage functions

size_t cppgc_weak_member_size() {
  return sizeof(CppgcWeakMember);
}

void cppgc_weak_member_construct(size_t storage, RustResource* resource) {
  new (reinterpret_cast<void*>(storage)) CppgcWeakMember(resource);
}

void cppgc_weak_member_destruct(size_t storage) {
  std::destroy_at(reinterpret_cast<CppgcWeakMember*>(storage));
}

RustResource* cppgc_weak_member_get(size_t storage) {
  return reinterpret_cast<const CppgcWeakMember*>(storage)->Get();
}

void cppgc_weak_member_assign(size_t storage, RustResource* resource) {
  *reinterpret_cast<CppgcWeakMember*>(storage) = resource;
}

void cppgc_visitor_trace_weak_member(CppgcVisitor* visitor, size_t storage) {
  auto* v8_visitor = cppgc_visitor_from_ffi(visitor);
  v8_visitor->Trace(*reinterpret_cast<const CppgcWeakMember*>(storage));
}

}  // namespace workerd::rust::jsg
