// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "ffi.h"

#include <workerd/jsg/jsg.h>
#include <workerd/jsg/setup.h>
#include <workerd/jsg/util.h>
#include <workerd/jsg/wrappable.h>
#include <workerd/rust/jsg/ffi-inl.h>
#include <workerd/rust/jsg/lib.rs.h>
#include <workerd/rust/jsg/v8.rs.h>

#include <kj-rs/convert.h>

#include <kj/common.h>
#include <kj/string.h>

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

// BackingStore — the size_t handle is the address of a heap-allocated
// std::shared_ptr<v8::BackingStore> allocated with `new`.

// Create an interned V8 string from a Rust identifier name (rust::String or rust::Str).
// NewFromUtf8 returns an empty MaybeLocal (without scheduling a JS exception) when the
// string exceeds v8::String::kMaxLength; the KJ_REQUIRE prevents a confusing ICE inside
// jsg::check() by catching the overlong name early with a clear error message.
template <typename Name>
static v8::Local<v8::String> makeInternedStr(v8::Isolate* isolate, const Name& name) {
  KJ_REQUIRE(name.size() <= static_cast<size_t>(v8::String::kMaxLength),
      "Rust identifier name exceeds V8 string length limit", name);
  return ::workerd::jsg::check(
      v8::String::NewFromUtf8(isolate, name.data(), v8::NewStringType::kInternalized, name.size()));
}

// Wrappable implementation - calls into Rust via CXX bridge
Wrappable::~Wrappable() {
  wrappable_invoke_drop(*this);
}

void Wrappable::jsgVisitForGc(::workerd::jsg::GcVisitor& visitor) {
  auto ffi_visitor = to_ffi(&visitor);
  wrappable_invoke_trace(*this, &ffi_visitor);
}

kj::StringPtr Wrappable::jsgGetMemoryName() const {
  // memory_name() on the Rust side returns a &'static str backed by a
  // compile-time c"..." literal, so the pointer is valid for the process
  // lifetime. Construct kj::StringPtr directly from data+size — no copy,
  // no allocation, no caching needed.
  auto name = wrappable_invoke_get_name(*this);
  return kj::StringPtr(name.data(), name.size());
}

size_t Wrappable::jsgGetMemorySelfSize() const {
  return sizeof(Wrappable);
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

bool local_is_float16_array(const Local& val) {
  return local_as_ref_from_ffi<v8::Value>(val)->IsFloat16Array();
}

bool local_is_uint8clamped_array(const Local& val) {
  return local_as_ref_from_ffi<v8::Value>(val)->IsUint8ClampedArray();
}

bool local_is_array_buffer(const Local& val) {
  return local_as_ref_from_ffi<v8::Value>(val)->IsArrayBuffer();
}

bool local_is_array_buffer_view(const Local& val) {
  return local_as_ref_from_ffi<v8::Value>(val)->IsArrayBufferView();
}

bool local_is_function(const Local& val) {
  return local_as_ref_from_ffi<v8::Value>(val)->IsFunction();
}

bool local_is_symbol(const Local& val) {
  return local_as_ref_from_ffi<v8::Value>(val)->IsSymbol();
}

bool local_is_name(const Local& val) {
  return local_as_ref_from_ffi<v8::Value>(val)->IsName();
}

bool local_is_shared_array_buffer(const Local& val) {
  return local_as_ref_from_ffi<v8::Value>(val)->IsSharedArrayBuffer();
}

::rust::String local_type_of(Isolate* isolate, const Local& val) {
  auto v8Val = local_as_ref_from_ffi<v8::Value>(val);
  v8::Local<v8::String> typeStr = v8Val->TypeOf(isolate);
  v8::String::Utf8Value utf8(isolate, typeStr);
  return ::rust::String(*utf8, utf8.length());
}

// Utf8Value
Utf8Value utf8_value_new(Isolate* isolate, Local value) {
  auto* v = new v8::String::Utf8Value(isolate, local_from_ffi<v8::Value>(kj::mv(value)));
  return Utf8Value{reinterpret_cast<size_t>(v)};
}

void utf8_value_drop(Utf8Value value) {
  delete reinterpret_cast<v8::String::Utf8Value*>(value.ptr);
}

size_t utf8_value_length(const Utf8Value& value) {
  return reinterpret_cast<const v8::String::Utf8Value*>(value.ptr)->length();
}

const uint8_t* utf8_value_data(const Utf8Value& value) {
  return reinterpret_cast<const uint8_t*>(
      **reinterpret_cast<const v8::String::Utf8Value*>(value.ptr));
}

// Local<String>
Local local_string_empty(Isolate* isolate) {
  return to_ffi(v8::String::Empty(isolate));
}

int32_t local_string_length(const Local& value) {
  return local_as_ref_from_ffi<v8::String>(value)->Length();
}

bool local_string_is_one_byte(const Local& value) {
  // Note: IsOneByte() reflects V8's internal string representation, not the logical
  // content. A string containing only Latin-1 characters may still return false if V8
  // stores it as two-byte (e.g. after concatenation), i.e. false negatives are possible.
  // Use ContainsOnlyOneByte() for a content-based check, keeping in mind that it scans
  // the entire string.
  return local_as_ref_from_ffi<v8::String>(value)->IsOneByte();
}

bool local_string_contains_only_one_byte(const Local& value) {
  return local_as_ref_from_ffi<v8::String>(value)->ContainsOnlyOneByte();
}

size_t local_string_utf8_length(Isolate* isolate, const Local& value) {
  return local_as_ref_from_ffi<v8::String>(value)->Utf8LengthV2(isolate);
}

void local_string_write_v2(Isolate* isolate,
    const Local& value,
    uint32_t offset,
    uint32_t length,
    uint16_t* buffer,
    int32_t flags) {
  local_as_ref_from_ffi<v8::String>(value)->WriteV2(isolate, offset, length, buffer, flags);
}

void local_string_write_one_byte_v2(Isolate* isolate,
    const Local& value,
    uint32_t offset,
    uint32_t length,
    uint8_t* buffer,
    int32_t flags) {
  local_as_ref_from_ffi<v8::String>(value)->WriteOneByteV2(isolate, offset, length, buffer, flags);
}

size_t local_string_write_utf8_v2(
    Isolate* isolate, const Local& value, uint8_t* buffer, size_t capacity, int32_t flags) {
  return local_as_ref_from_ffi<v8::String>(value)->WriteUtf8V2(
      isolate, reinterpret_cast<char*>(buffer), capacity, flags);
}

bool local_string_equals(const Local& value, const Local& other) {
  return local_as_ref_from_ffi<v8::String>(value)->StringEquals(
      local_as_ref_from_ffi<v8::String>(other));
}

bool local_string_is_flat(const Local& value) {
  return local_as_ref_from_ffi<v8::String>(value)->IsFlat();
}

Local local_string_concat(Isolate* isolate, Local left, Local right) {
  return to_ffi(v8::String::Concat(isolate, local_from_ffi<v8::String>(kj::mv(left)),
      local_from_ffi<v8::String>(kj::mv(right))));
}

Local local_string_internalize(Isolate* isolate, const Local& value) {
  return to_ffi(local_as_ref_from_ffi<v8::String>(value)->InternalizeString(isolate));
}

MaybeLocal local_string_new_from_utf8(
    Isolate* isolate, const uint8_t* data, int32_t length, bool internalized) {
  auto type = internalized ? v8::NewStringType::kInternalized : v8::NewStringType::kNormal;
  return maybe_local_to_ffi(
      v8::String::NewFromUtf8(isolate, reinterpret_cast<const char*>(data), type, length));
}

MaybeLocal local_string_new_from_one_byte(
    Isolate* isolate, const uint8_t* data, int32_t length, bool internalized) {
  auto type = internalized ? v8::NewStringType::kInternalized : v8::NewStringType::kNormal;
  return maybe_local_to_ffi(v8::String::NewFromOneByte(isolate, data, type, length));
}

MaybeLocal local_string_new_from_two_byte(
    Isolate* isolate, const uint16_t* data, int32_t length, bool internalized) {
  auto type = internalized ? v8::NewStringType::kInternalized : v8::NewStringType::kNormal;
  return maybe_local_to_ffi(v8::String::NewFromTwoByte(isolate, data, type, length));
}

bool maybe_local_is_empty(const MaybeLocal& value) {
  auto ptr_void = reinterpret_cast<const void*>(&value.ptr);
  return reinterpret_cast<const v8::MaybeLocal<v8::Value>*>(ptr_void)->IsEmpty();
}

// Local<Name>
int32_t local_name_get_identity_hash(const Local& value) {
  return local_as_ref_from_ffi<v8::Name>(value)->GetIdentityHash();
}

// Local<Symbol>
Local local_symbol_new(Isolate* isolate) {
  return to_ffi(v8::Symbol::New(isolate));
}

Local local_symbol_new_with_description(Isolate* isolate, Local description) {
  return to_ffi(v8::Symbol::New(isolate, local_from_ffi<v8::String>(kj::mv(description))));
}

MaybeLocal local_symbol_description(Isolate* isolate, const Local& value) {
  auto sym = local_as_ref_from_ffi<v8::Symbol>(value);
  v8::Local<v8::Value> desc = sym->Description(isolate);
  if (desc->IsUndefined()) {
    return maybe_local_to_ffi(v8::MaybeLocal<v8::String>());
  }
  // Description is always a String when present.
  return maybe_local_to_ffi(v8::MaybeLocal<v8::String>(desc.As<v8::String>()));
}

// Local<Function>
Local local_function_call(
    Isolate* isolate, const Local& function, const Local& recv, ::rust::Slice<const Local> args) {
  auto context = isolate->GetCurrentContext();
  auto fn = local_as_ref_from_ffi<v8::Function>(function);
  auto receiver = local_as_ref_from_ffi<v8::Value>(recv);

  v8::LocalVector<v8::Value> v8Args(isolate, args.size());
  for (size_t i = 0; i < args.size(); i++) {
    v8Args[i] = local_as_ref_from_ffi<v8::Value>(args[i]);
  }

  return to_ffi(::workerd::jsg::check(fn->Call(context, receiver, v8Args.size(), v8Args.data())));
}

// Local<Object>
void local_object_set_property(Isolate* isolate, Local& object, ::rust::Str key, Local value) {
  auto v8_obj = local_as_ref_from_ffi<v8::Object>(object);
  auto context = isolate->GetCurrentContext();
  auto v8_key = makeInternedStr(isolate, key);
  ::workerd::jsg::check(v8_obj->Set(context, v8_key, local_from_ffi<v8::Value>(kj::mv(value))));
}

bool local_object_has_property(Isolate* isolate, const Local& object, ::rust::Str key) {
  auto v8_obj = local_as_ref_from_ffi<v8::Object>(object);
  auto context = isolate->GetCurrentContext();
  auto v8_key = makeInternedStr(isolate, key);
  return v8_obj->Has(context, v8_key).FromJust();
}

kj::Maybe<Local> local_object_get_property(Isolate* isolate, const Local& object, ::rust::Str key) {
  auto v8_obj = local_as_ref_from_ffi<v8::Object>(object);
  auto context = isolate->GetCurrentContext();
  auto v8_key = makeInternedStr(isolate, key);
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

// Local<ArrayBuffer>
Local local_new_array_buffer(Isolate* isolate, const uint8_t* data, size_t length) {
  auto backingStore = v8::ArrayBuffer::NewBackingStore(isolate, length);
  if (length > 0) {
    memcpy(backingStore->Data(), data, length);
  }
  return to_ffi(v8::ArrayBuffer::New(isolate, std::move(backingStore)));
}

// "empty" means zero-initialized with no source data to copy from, as opposed to
// local_new_array_buffer which copies caller-supplied bytes into the buffer.
Local local_new_array_buffer_empty(Isolate* isolate, size_t byte_length) {
  return to_ffi(v8::ArrayBuffer::New(isolate, byte_length));
}

kj::Maybe<Local> array_buffer_new_with_mode(
    Isolate* isolate, size_t byte_length, BackingStoreInitializationMode mode) {
  auto maybe = v8::ArrayBuffer::MaybeNew(
      isolate, byte_length, static_cast<v8::BackingStoreInitializationMode>(mode));
  if (maybe.IsEmpty()) return kj::none;
  return to_ffi(maybe.ToLocalChecked());
}

Local array_buffer_from_backing_store(Isolate* isolate, size_t ptr) {
  return to_ffi(
      v8::ArrayBuffer::New(isolate, *reinterpret_cast<std::shared_ptr<v8::BackingStore>*>(ptr)));
}

size_t local_array_buffer_byte_length(Isolate* isolate, const Local& buffer) {
  return local_as_ref_from_ffi<v8::ArrayBuffer>(buffer)->ByteLength();
}

uint8_t* local_array_buffer_data(Isolate* isolate, const Local& buffer) {
  return static_cast<uint8_t*>(local_as_ref_from_ffi<v8::ArrayBuffer>(buffer)->Data());
}

size_t local_array_buffer_get_backing_store(Isolate* isolate, const Local& buffer) {
  return reinterpret_cast<size_t>(new std::shared_ptr<v8::BackingStore>(
      local_as_ref_from_ffi<v8::ArrayBuffer>(buffer)->GetBackingStore()));
}

// Local<ArrayBufferView>
size_t local_array_buffer_view_byte_offset(Isolate* isolate, const Local& view) {
  return local_as_ref_from_ffi<v8::ArrayBufferView>(view)->ByteOffset();
}

size_t local_array_buffer_view_byte_length(Isolate* isolate, const Local& view) {
  return local_as_ref_from_ffi<v8::ArrayBufferView>(view)->ByteLength();
}

uint8_t* local_array_buffer_view_buffer_data(Isolate* isolate, const Local& view) {
  return static_cast<uint8_t*>(local_as_ref_from_ffi<v8::ArrayBufferView>(view)->Buffer()->Data());
}

Local local_array_buffer_view_get_buffer(Isolate* isolate, const Local& view) {
  return to_ffi(local_as_ref_from_ffi<v8::ArrayBufferView>(view)->Buffer());
}

size_t local_array_buffer_view_element_size(Isolate* isolate, const Local& view) {
  auto& v8Val = local_as_ref_from_ffi<v8::Value>(view);
  if (v8Val->IsUint8Array() || v8Val->IsInt8Array() || v8Val->IsUint8ClampedArray()) return 1;
  if (v8Val->IsUint16Array() || v8Val->IsInt16Array()) return 2;
  if (v8Val->IsUint32Array() || v8Val->IsInt32Array() || v8Val->IsFloat32Array()) return 4;
  if (v8Val->IsFloat64Array() || v8Val->IsBigInt64Array() || v8Val->IsBigUint64Array()) return 8;
  return 0;  // DataView — no fixed element size
}

bool local_array_buffer_view_is_integer_type(Isolate* isolate, const Local& view) {
  auto& v8Val = local_as_ref_from_ffi<v8::Value>(view);
  // Float32Array, Float64Array, and DataView are not integer types.
  return v8Val->IsTypedArray() && !v8Val->IsFloat32Array() && !v8Val->IsFloat64Array();
}

// BackingStore
size_t backing_store_new_resizable(size_t byte_length, size_t max_byte_length) {
  return reinterpret_cast<size_t>(new std::shared_ptr<v8::BackingStore>(
      v8::ArrayBuffer::NewResizableBackingStore(byte_length, max_byte_length)));
}

void backing_store_drop(size_t ptr) {
  delete reinterpret_cast<std::shared_ptr<v8::BackingStore>*>(ptr);
}

uint8_t* backing_store_data(size_t ptr) {
  return static_cast<uint8_t*>(
      reinterpret_cast<std::shared_ptr<v8::BackingStore>*>(ptr)->get()->Data());
}

size_t backing_store_byte_length(size_t ptr) {
  return reinterpret_cast<std::shared_ptr<v8::BackingStore>*>(ptr)->get()->ByteLength();
}

size_t backing_store_max_byte_length(size_t ptr) {
  return reinterpret_cast<std::shared_ptr<v8::BackingStore>*>(ptr)->get()->MaxByteLength();
}

bool backing_store_is_shared(size_t ptr) {
  return reinterpret_cast<std::shared_ptr<v8::BackingStore>*>(ptr)->get()->IsShared();
}

bool backing_store_is_resizable_by_user_javascript(size_t ptr) {
  return reinterpret_cast<std::shared_ptr<v8::BackingStore>*>(ptr)
      ->get()
      ->IsResizableByUserJavaScript();
}

// ArrayBuffer detach/detachable/was-detached
void local_array_buffer_detach(Isolate* isolate, Local& buffer) {
  local_as_ref_from_ffi<v8::ArrayBuffer>(buffer)->Detach(v8::Local<v8::Value>()).Check();
}

bool local_array_buffer_was_detached(Isolate* isolate, const Local& buffer) {
  return local_as_ref_from_ffi<v8::ArrayBuffer>(buffer)->WasDetached();
}

bool local_array_buffer_is_detachable(Isolate* isolate, const Local& buffer) {
  return local_as_ref_from_ffi<v8::ArrayBuffer>(buffer)->IsDetachable();
}

// ArrayBuffer is_shared (value-level check)
bool local_array_buffer_is_shared(const Local& value) {
  return local_as_ref_from_ffi<v8::Value>(value)->IsSharedArrayBuffer();
}

// TypedArray creation functions
// TODO(perf): These macros duplicate patterns in buffersource.h — unify when the
// Rust FFI stabilises.
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
Local wrap_resource(Isolate* isolate, kj::Rc<Wrappable> wrappable, const Global& tmpl) {
  // Check if already wrapped
  KJ_IF_SOME(handle, wrappable->tryGetHandle(isolate)) {
    return to_ffi(v8::Local<v8::Value>::Cast(handle));
  }

  auto& global_tmpl = global_as_ref_from_ffi<v8::FunctionTemplate>(tmpl);
  auto local_tmpl = v8::Local<v8::FunctionTemplate>::New(isolate, global_tmpl);
  v8::Local<v8::Object> object = ::workerd::jsg::check(
      local_tmpl->InstanceTemplate()->NewInstance(isolate->GetCurrentContext()));

  // attachWrapper sets up CppgcShim, TracedReference, internal fields, etc.
  wrappable->attachWrapper(isolate, object, true);

  // Override tag to identify as Rust object for unwrapping
  auto tagAddress = const_cast<uint16_t*>(&::workerd::jsg::Wrappable::WORKERD_RUST_WRAPPABLE_TAG);
  object->SetAlignedPointerInInternalField(::workerd::jsg::Wrappable::WRAPPABLE_TAG_FIELD_INDEX,
      tagAddress,
      static_cast<v8::EmbedderDataTypeTag>(::workerd::jsg::Wrappable::WRAPPABLE_TAG_FIELD_INDEX));

  return to_ffi(v8::Local<v8::Value>::Cast(object));
}

void wrappable_attach_wrapper(kj::Rc<Wrappable> wrappable, FunctionCallbackInfo& args) {
  auto* isolate = args.GetIsolate();
  auto object = args.This();

  // attachWrapper sets up CppgcShim, TracedReference, internal fields, etc.
  wrappable->attachWrapper(isolate, object, true);

  // Override tag to identify as Rust object for unwrapping
  auto tagAddress = const_cast<uint16_t*>(&::workerd::jsg::Wrappable::WORKERD_RUST_WRAPPABLE_TAG);
  object->SetAlignedPointerInInternalField(::workerd::jsg::Wrappable::WRAPPABLE_TAG_FIELD_INDEX,
      tagAddress,
      static_cast<v8::EmbedderDataTypeTag>(::workerd::jsg::Wrappable::WRAPPABLE_TAG_FIELD_INDEX));
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

kj::Rc<Wrappable> unwrap_resource(Isolate* isolate, Local value) {
  auto v8_val = local_from_ffi<v8::Value>(kj::mv(value));
  // Non-object values (numbers, strings, booleans, etc.) are never wrapped resources.
  if (!v8_val->IsObject()) return nullptr;
  auto v8_obj = v8_val.As<v8::Object>();
  // Plain JS objects have no internal fields; check before reading to avoid V8 fatal error.
  if (v8_obj->InternalFieldCount() < ::workerd::jsg::Wrappable::INTERNAL_FIELD_COUNT ||
      v8_obj->GetAlignedPointerFromInternalField(
          ::workerd::jsg::Wrappable::WRAPPABLE_TAG_FIELD_INDEX,
          static_cast<v8::EmbedderDataTypeTag>(
              ::workerd::jsg::Wrappable::WRAPPABLE_TAG_FIELD_INDEX)) !=
          const_cast<uint16_t*>(&::workerd::jsg::Wrappable::WORKERD_RUST_WRAPPABLE_TAG)) {
    return nullptr;
  }
  auto* ptr = static_cast<Wrappable*>(
      reinterpret_cast<::workerd::jsg::Wrappable*>(v8_obj->GetAlignedPointerFromInternalField(
          ::workerd::jsg::Wrappable::WRAPPED_OBJECT_FIELD_INDEX,
          static_cast<v8::EmbedderDataTypeTag>(
              ::workerd::jsg::Wrappable::WRAPPED_OBJECT_FIELD_INDEX))));
  return ptr->toRc();
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

uintptr_t local_typed_array_buffer_data(Isolate* isolate, const Local& array) {
  return reinterpret_cast<uintptr_t>(
      local_as_ref_from_ffi<v8::TypedArray>(array)->Buffer()->Data());
}

size_t local_typed_array_byte_offset(Isolate* isolate, const Local& array) {
  return local_as_ref_from_ffi<v8::TypedArray>(array)->ByteOffset();
}

size_t local_typed_array_byte_length(Isolate* isolate, const Local& array) {
  return local_as_ref_from_ffi<v8::TypedArray>(array)->ByteLength();
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
DEFINE_TYPED_ARRAY_GET(uint8clamped_array, Uint8ClampedArray, uint8_t)

// Global<T>
void global_reset(Global& value) {
  global_as_ref_from_ffi<v8::Value>(value)->Reset();
}

Global global_clone(Isolate* isolate, const Global& value) {
  auto& original = global_as_ref_from_ffi<v8::Value>(value);
  return to_ffi(v8::Global<v8::Value>(isolate, original));
}

Local global_to_local(Isolate* isolate, const Global& value) {
  auto& glbl = global_as_ref_from_ffi<v8::Value>(value);
  v8::Local<v8::Value> local = v8::Local<v8::Value>::New(isolate, glbl);
  return to_ffi(kj::mv(local));
}

// Wrappable - data access
const TraitObjectPtr& wrappable_get_trait_object(const Wrappable& wrappable) {
  return wrappable.trait_object;
}

void wrappable_clear_trait_object(Wrappable& wrappable) {
  wrappable.trait_object = {0, 0, 0, 0};
}

kj::uint wrappable_strong_refcount(const Wrappable& wrappable) {
  return wrappable.getStrongRefcount();
}

// Wrappable lifecycle
kj::Rc<Wrappable> wrappable_new(TraitObjectPtr ptr) {
  auto rc = kj::rc<Wrappable>();
  rc->trait_object = kj::mv(ptr);
  rc->addStrongRef();
  return kj::mv(rc);
}

kj::Rc<Wrappable> wrappable_to_rc(Wrappable& wrappable) {
  return wrappable.toRc();
}

void wrappable_add_strong_ref(Wrappable& wrappable) {
  wrappable.addStrongRef();
}

void wrappable_remove_strong_ref(Wrappable& wrappable, bool is_strong) {
  // maybeDeferDestruction() requires a kj::Own<Wrappable> to take ownership of.
  // We must temporarily increment the refcount via addRef() to create that handle.
  //
  // Refcount accounting:
  //   addRef(wrappable)      → +1 (creates `own`)
  //   maybeDeferDestruction  → internally stores `own` in RefToDelete
  //   ~RefToDelete           → if is_strong: calls removeStrongRef(), then drops `own` → -1
  // Net effect: 0 (the actual kj::Rc decrement happens later when Rust drops the KjRc).
  //
  // is_strong must match the Ref's current strong flag. If GC tracing already transitioned
  // the ref to weak (strong=false), passing true here would double-decrement strongRefcount.
  auto own = kj::addRef(wrappable);
  wrappable.maybeDeferDestruction(is_strong, kj::mv(own), &wrappable);
}

void wrappable_visit_global(GcVisitor* visitor, uintptr_t* global, TracedReference& traced) {
  auto* gcVisitor = gc_visitor_from_ffi(visitor);
  auto& strongHandle = *reinterpret_cast<v8::Global<v8::Value>*>(global);
  auto& tracedHandle = traced_ref_from_ffi(traced);
  gcVisitor->visit(strongHandle, tracedHandle);
}

void traced_reference_reset(TracedReference& traced) {
  traced_ref_from_ffi(traced).Reset();
}

void wrappable_visit_ref(
    Wrappable& wrappable, uintptr_t* ref_parent, bool* ref_strong, GcVisitor* visitor) {
  auto* gcVisitor = gc_visitor_from_ffi(visitor);

  // Convert opaque uintptr_t to kj::Maybe<Wrappable&>
  kj::Maybe<::workerd::jsg::Wrappable&> parentMaybe;
  if (*ref_parent != 0) {
    parentMaybe = *reinterpret_cast<::workerd::jsg::Wrappable*>(*ref_parent);
  }

  wrappable.visitRef(*gcVisitor, parentMaybe, *ref_strong);

  // Write back
  KJ_IF_SOME(p, parentMaybe) {
    *ref_parent = reinterpret_cast<uintptr_t>(&p);
  } else {
    *ref_parent = 0;
  }
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

  auto internalMarker =
      v8::Symbol::For(isolate, ::workerd::jsg::v8StrIntern(isolate, "cloudflare:internal-class"));
  prototype->Set(internalMarker, internalMarker,
      static_cast<v8::PropertyAttribute>(v8::PropertyAttribute::DontEnum |
          v8::PropertyAttribute::DontDelete | v8::PropertyAttribute::ReadOnly));

  constructor->SetClassName(classname);

  for (const auto& method: descriptor.static_methods) {
    auto functionTemplate = v8::FunctionTemplate::New(isolate,
        reinterpret_cast<v8::FunctionCallback>(reinterpret_cast<void*>(method.callback)),
        v8::Local<v8::Value>(), v8::Local<v8::Signature>(), 0, v8::ConstructorBehavior::kThrow);
    functionTemplate->RemovePrototype();
    constructor->Set(makeInternedStr(isolate, method.name), functionTemplate);
  }

  for (const auto& method: descriptor.methods) {
    auto functionTemplate = v8::FunctionTemplate::New(isolate,
        reinterpret_cast<v8::FunctionCallback>(reinterpret_cast<void*>(method.callback)),
        v8::Local<v8::Value>(), signature, 0, v8::ConstructorBehavior::kThrow);
    auto name = makeInternedStr(isolate, method.name);
    prototype->Set(name, functionTemplate);
  }

  const bool specCompliant = workerd::jsg::getSpecCompliantPropertyAttributes(isolate);

  // Mirrors ResourceTypeBuilder constructor (resource.h:1263-1273): always create and install
  // the inspectProperties ObjectTemplate under the kResourceTypeInspect API symbol.  node:util's
  // inspect() checks for this symbol on the prototype to identify JSG resource types and uses
  // the dictionary to enumerate inspect-only properties by name.  This must be present even on
  // resource types with no #[jsg_inspect_property] fields, because inspect() also uses the
  // symbol's presence to decide how to walk the prototype chain.
  auto kResourceTypeInspectStr = ::workerd::jsg::v8StrIntern(isolate, "kResourceTypeInspect");
  auto kResourceTypeInspectSymbol = v8::Symbol::ForApi(isolate, kResourceTypeInspectStr);
  auto inspectProperties = v8::ObjectTemplate::New(isolate);
  prototype->Set(kResourceTypeInspectSymbol, inspectProperties,
      static_cast<v8::PropertyAttribute>(
          v8::PropertyAttribute::ReadOnly | v8::PropertyAttribute::DontEnum));

  for (const auto& prop: descriptor.properties) {
    auto v8Name = makeInternedStr(isolate, prop.name);

    // Helper: build a FunctionTemplate for a getter or setter callback, applying
    // spec_compliant_property_attributes name/length rules when enabled.
    // `isGetter` true → length=0, name="get <prop>"; false → length=1, name="set <prop>".
    auto makePropFn = [&](size_t callback, bool isGetter) {
      v8::Local<v8::FunctionTemplate> fn;
      if (specCompliant) {
        int len = isGetter ? 0 : 1;
        // Per Web IDL, spec-compliant getters/setters use empty signature (matching C++
        // registerPrototypeProperty/registerReadonlyPrototypeProperty in resource.h:1438-1441).
        fn = v8::FunctionTemplate::New(isolate,
            reinterpret_cast<v8::FunctionCallback>(reinterpret_cast<void*>(callback)),
            v8::Local<v8::Value>(), v8::Local<v8::Signature>(), len,
            v8::ConstructorBehavior::kThrow);
        auto prefix = isGetter ? "get " : "set ";
        fn->SetClassName(::workerd::jsg::v8Str(isolate, kj::str(prefix, prop.name)));
      } else {
        fn = v8::FunctionTemplate::New(
            isolate, reinterpret_cast<v8::FunctionCallback>(reinterpret_cast<void*>(callback)));
      }
      return fn;
    };

    switch (prop.kind) {
      case PropertyKind::Prototype: {
        // Mirrors registerPrototypeProperty / registerReadonlyPrototypeProperty in resource.h.
        auto getterFn = makePropFn(prop.getter_callback, true /* isGetter */);
        KJ_IF_SOME(setterCb, prop.setter_callback) {
          auto setterFn = makePropFn(setterCb, false /* isGetter */);
          // Normal (non-Unimplemented) prototype properties are enumerable — use None, matching
          // C++ registerPrototypeProperty (resource.h:1454-1455) with Gcb::enumerable = true.
          prototype->SetAccessorProperty(v8Name, getterFn, setterFn, v8::PropertyAttribute::None);
        } else {
          // Read-only prototype properties are also enumerable — use ReadOnly only, matching
          // C++ registerReadonlyPrototypeProperty (resource.h:1498-1501) with Gcb::enumerable = true.
          prototype->SetAccessorProperty(
              v8Name, getterFn, v8::Local<v8::FunctionTemplate>(), v8::PropertyAttribute::ReadOnly);
        }
        break;
      }
      case PropertyKind::Instance: {
        // Mirrors registerInstanceProperty / registerReadonlyInstanceProperty in resource.h.
        //
        // We use ObjectTemplate::SetAccessorProperty with FunctionTemplates rather than
        // SetNativeDataProperty because our Rust callbacks are FunctionCallbackInfo-style
        // (matching #[jsg_method]), not PropertyCallbackInfo-style.
        // SetAccessorProperty on the InstanceTemplate installs the accessor as an own
        // property on every instance, matching JSG_INSTANCE_PROPERTY semantics.
        auto getterFn = makePropFn(prop.getter_callback, true /* isGetter */);
        KJ_IF_SOME(setterCb, prop.setter_callback) {
          auto setterFn = makePropFn(setterCb, false /* isGetter */);
          instance->SetAccessorProperty(v8Name, getterFn, setterFn, v8::PropertyAttribute::None);
        } else {
          instance->SetAccessorProperty(
              v8Name, getterFn, v8::Local<v8::FunctionTemplate>(), v8::PropertyAttribute::ReadOnly);
        }
        break;
      }
      case PropertyKind::Inspect: {
        // Mirrors registerInspectProperty in resource.h (lines 1521-1535).
        //
        // 1. Create a unique per-property symbol (so the getter is inaccessible via string lookup).
        // 2. Register name → symbol in inspectProperties so node:util can enumerate it by name.
        // 3. Install the getter under the unique symbol on the prototype (ReadOnly | DontEnum).
        //
        // spec_compliant_property_attributes has no effect on inspect properties.
        auto symbol = v8::Symbol::New(isolate, v8Name);
        inspectProperties->Set(v8Name, symbol, v8::PropertyAttribute::ReadOnly);
        auto getterFn = v8::FunctionTemplate::New(isolate,
            reinterpret_cast<v8::FunctionCallback>(reinterpret_cast<void*>(prop.getter_callback)));
        prototype->SetAccessorProperty(symbol, getterFn, v8::Local<v8::FunctionTemplate>(),
            static_cast<v8::PropertyAttribute>(
                v8::PropertyAttribute::ReadOnly | v8::PropertyAttribute::DontEnum));
        break;
      }
    }
  }

  for (const auto& constant: descriptor.static_constants) {
    auto name = makeInternedStr(isolate, constant.name);
    auto value = v8::Number::New(isolate, constant.value);

    // Per Web IDL, constants are {writable: false, enumerable: true, configurable: false}.
    auto attrs = ::workerd::jsg::getSpecCompliantPropertyAttributes(isolate)
        ? static_cast<v8::PropertyAttribute>(
              v8::PropertyAttribute::ReadOnly | v8::PropertyAttribute::DontDelete)
        : v8::PropertyAttribute::ReadOnly;
    constructor->Set(name, value, attrs);
    prototype->Set(name, value, attrs);
  }

  auto result = scope.Escape(constructor);
  return to_ffi(v8::Global<v8::FunctionTemplate>(isolate, result));
}

// FunctionTemplate
Local function_template_get_function(Isolate* isolate, const Global& tmpl) {
  auto& global_tmpl = global_as_ref_from_ffi<v8::FunctionTemplate>(tmpl);
  auto local_tmpl = v8::Local<v8::FunctionTemplate>::New(isolate, global_tmpl);
  auto function = ::workerd::jsg::check(local_tmpl->GetFunction(isolate->GetCurrentContext()));
  return to_ffi(kj::mv(function));
}

// Realm
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

void isolate_throw_internal_error(Isolate* isolate, ::rust::Str internalMessage) {
  // Mirrors makeInternalError() from util.c++: generates a unique error ID,
  // logs the internal message (with ID) to KJ_LOG(ERROR) for Sentry, and
  // throws a generic "internal error; reference = <id>" JS Error to the caller.
  // kj::heapString(data, size) copies exactly `size` bytes and appends a NUL,
  // avoiding the out-of-bounds read that kj::StringPtr(data, size) would cause
  // on non-NUL-terminated Rust &str data.
  auto message = kj::heapString(internalMessage.data(), internalMessage.size());
  isolate->ThrowException(::workerd::jsg::makeInternalError(isolate, message));
}

void isolate_terminate_execution(Isolate* isolate) {
  ::workerd::jsg::IsolateBase::from(isolate).terminateExecution();
}

bool isolate_is_locked(Isolate* isolate) {
  return v8::Locker::IsLocked(isolate);
}

}  // namespace workerd::rust::jsg
