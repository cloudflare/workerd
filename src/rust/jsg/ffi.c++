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
#include <kj/map.h>
#include <kj/mutex.h>
#include <kj/string.h>

#include <memory>

#if KJ_HAS_COMPILER_FEATURE(address_sanitizer) || defined(__SANITIZE_ADDRESS__)
#include <sanitizer/lsan_interface.h>
#endif

#if V8_MAJOR_VERSION >= 15
#define Utf8LengthV2 Utf8Length
#define WriteV2 Write
#define WriteOneByteV2 WriteOneByte
#define WriteUtf8V2 WriteUtf8
#endif

using namespace kj_rs;

namespace workerd::rust::jsg {

#define DEFINE_TYPED_ARRAY_NEW(name, v8_type, elem_type)                                           \
  Local local_new_##name(Isolate* isolate, const elem_type* data, size_t length) {                 \
    auto backingStore = v8::ArrayBuffer::NewBackingStore(isolate, length * sizeof(elem_type));     \
    /* A zero-length backing store has a null Data(), which memcpy must not receive. */            \
    if (length > 0) {                                                                              \
      memcpy(backingStore->Data(), data, length * sizeof(elem_type));                              \
    }                                                                                              \
    auto arrayBuffer = v8::ArrayBuffer::New(isolate, std::move(backingStore));                     \
    return to_ffi(v8::v8_type::New(arrayBuffer, 0, length));                                       \
  }

#define DEFINE_TYPED_ARRAY_UNWRAP(name, v8_type, elem_type)                                        \
  ::rust::Vec<elem_type> unwrap_##name(Isolate* isolate, Local value) {                            \
    auto v8Val = local_from_ffi<v8::Value>(kj::mv(value));                                         \
    KJ_REQUIRE(v8Val->Is##v8_type(), "Expected a " #v8_type);                                      \
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
  elem_type local_##name##_get(const Local& array, size_t index) {                                 \
    auto typed = local_as_ref_from_ffi<v8::v8_type>(array);                                        \
    KJ_REQUIRE(index < typed->Length(), "index out of bounds");                                    \
    auto data = reinterpret_cast<elem_type*>(                                                      \
        static_cast<uint8_t*>(typed->Buffer()->Data()) + typed->ByteOffset());                     \
    return data[index];                                                                            \
  }

// BackingStore — the size_t handle is the address of a heap-allocated
// std::shared_ptr<v8::BackingStore> allocated with `new`.

// Creates a V8 string from UTF-8. NewFromUtf8 fails only when the string is longer than
// v8::String::kMaxLength, and then leaves no JavaScript exception pending, so an overlong string
// throws a KJ exception instead. The length is checked before the call, which keeps a length
// above INT_MAX from reaching NewFromUtf8's int parameter, where a negative value means
// "NUL-terminated". kMaxLength counts UTF-16 code units and the check counts UTF-8 bytes, of
// which there are never fewer, so it may reject a long multi-byte string that V8 could hold, but
// never lets through one that V8 would reject.
static v8::Local<v8::String> newUtf8String(
    v8::Isolate* isolate, kj::ArrayPtr<const char> utf8, v8::NewStringType type) {
  KJ_REQUIRE(utf8.size() <= static_cast<size_t>(v8::String::kMaxLength),
      "string exceeds V8's string length limit", utf8.size());
  v8::Local<v8::String> result;
  if (!v8::String::NewFromUtf8(isolate, utf8.begin(), type, static_cast<int>(utf8.size()))
           .ToLocal(&result)) {
    KJ_FAIL_REQUIRE("V8 failed to create a string", utf8.size());
  }
  return result;
}

// Creates an interned V8 string from a Rust identifier name (rust::String or rust::Str). An
// overlong name is checked here too, so that the error names it.
template <typename Name>
static v8::Local<v8::String> makeInternedStr(v8::Isolate* isolate, const Name& name) {
  KJ_REQUIRE(name.size() <= static_cast<size_t>(v8::String::kMaxLength),
      "Rust identifier name exceeds V8 string length limit", name);
  return newUtf8String(
      isolate, kj::arrayPtr(name.data(), name.size()), v8::NewStringType::kInternalized);
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
Global local_to_global(Isolate* isolate, Local value) {
  v8::Global<v8::Value> global(isolate, local_from_ffi<v8::Value>(kj::mv(value)));
  return to_ffi(kj::mv(global));
}

Local local_new_number(Isolate* isolate, double value) {
  v8::Local<v8::Number> val = v8::Number::New(isolate, value);
  return to_ffi(kj::mv(val));
}

Local local_new_string(Isolate* isolate, ::rust::Str value) {
  auto val =
      newUtf8String(isolate, kj::arrayPtr(value.data(), value.size()), v8::NewStringType::kNormal);
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

bool local_is_compatible_with_isolate(Isolate* isolate, const Local& value) {
  return local_as_ref_from_ffi<v8::Data>(value)->IsCompatibleWithIsolate(isolate);
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

bool local_is_typed_array(const Local& val) {
  return local_as_ref_from_ffi<v8::Value>(val)->IsTypedArray();
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

bool local_is_big_int(const Local& val) {
  return local_as_ref_from_ffi<v8::Value>(val)->IsBigInt();
}

bool local_is_date(const Local& val) {
  return local_as_ref_from_ffi<v8::Value>(val)->IsDate();
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

MaybeLocal local_string_concat(Isolate* isolate, Local left, Local right) {
  // `Concat` returns an empty handle when the result would be too long.
  return maybe_local_to_ffi(v8::MaybeLocal<v8::String>(v8::String::Concat(isolate,
      local_from_ffi<v8::String>(kj::mv(left)), local_from_ffi<v8::String>(kj::mv(right)))));
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

// Local<BigInt>
Local big_int_new(Isolate* isolate, int64_t value) {
  return to_ffi(v8::BigInt::New(isolate, value));
}

Local big_int_new_from_unsigned(Isolate* isolate, uint64_t value) {
  return to_ffi(v8::BigInt::NewFromUnsigned(isolate, value));
}

int64_t local_big_int_int64_value(const Local& value, bool& lossless) {
  return local_as_ref_from_ffi<v8::BigInt>(value)->Int64Value(&lossless);
}

// Local<Date>
// Empty, with the exception pending, if creating the object throws.
MaybeLocal date_new(Isolate* isolate, double time) {
  return maybe_local_to_ffi(v8::Date::New(isolate->GetCurrentContext(), time));
}

double local_date_value_of(const Local& value) {
  return local_as_ref_from_ffi<v8::Date>(value)->ValueOf();
}

// Local<Function>
MaybeLocal local_function_call(
    Isolate* isolate, const Local& function, const Local& recv, ::rust::Slice<const Local> args) {
  auto context = isolate->GetCurrentContext();
  auto fn = local_as_ref_from_ffi<v8::Function>(function);
  auto receiver = local_as_ref_from_ffi<v8::Value>(recv);

  v8::LocalVector<v8::Value> v8Args(isolate, args.size());
  for (size_t i = 0; i < args.size(); i++) {
    v8Args[i] = local_as_ref_from_ffi<v8::Value>(args[i]);
  }

  // No TryCatch: what the function throws stays pending and propagates once the Rust caller
  // returns to JavaScript, as with C++ JSG. V8 then still reports it where it was thrown, and
  // predicts whether JavaScript catches it (which DevTools' "pause on uncaught exceptions" uses)
  // by walking the whole stack.
  return maybe_local_to_ffi(fn->Call(context, receiver, v8Args.size(), v8Args.data()));
}

MaybeLocal function_new(Isolate* isolate, size_t callback, kj::Rc<Wrappable> wrappable) {
  auto context = isolate->GetCurrentContext();
  // Like the functions C++ JSG wraps (FunctionWrapper::wrap() in jsg/function.h): the wrapper
  // keeps the Wrappable alive and traced for as long as the function is reachable.
  auto data = wrappable->attachOpaqueWrapper(context, true);
  return maybe_local_to_ffi(v8::Function::New(
      context, reinterpret_cast<v8::FunctionCallback>(reinterpret_cast<void*>(callback)), data));
}

// Local<Object>
void local_object_set_property(Isolate* isolate, Local& object, ::rust::Str key, Local value) {
  auto v8_obj = local_as_ref_from_ffi<v8::Object>(object);
  auto context = isolate->GetCurrentContext();
  auto v8_key = makeInternedStr(isolate, key);
  ::workerd::jsg::check(v8_obj->Set(context, v8_key, local_from_ffi<v8::Value>(kj::mv(value))));
}

// No TryCatch in the shims below that run JavaScript: what it throws stays pending, as in
// local_function_call().

bool local_object_has_property(Isolate* isolate, const Local& object, ::rust::Str key, bool& has) {
  auto v8_obj = local_as_ref_from_ffi<v8::Object>(object);
  auto context = isolate->GetCurrentContext();
  auto v8_key = makeInternedStr(isolate, key);
  return v8_obj->Has(context, v8_key).To(&has);
}

MaybeLocal local_object_get_property(Isolate* isolate, const Local& object, ::rust::Str key) {
  auto v8_obj = local_as_ref_from_ffi<v8::Object>(object);
  auto context = isolate->GetCurrentContext();
  auto v8_key = makeInternedStr(isolate, key);
  return maybe_local_to_ffi(v8_obj->Get(context, v8_key));
}

// Local<Array>
Local local_new_array(Isolate* isolate, size_t length) {
  return to_ffi(v8::Array::New(isolate, length));
}

uint32_t local_array_length(const Local& array) {
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

MaybeLocal array_buffer_new_with_mode(
    Isolate* isolate, size_t byte_length, BackingStoreInitializationMode mode) {
  return maybe_local_to_ffi(v8::ArrayBuffer::MaybeNew(
      isolate, byte_length, static_cast<v8::BackingStoreInitializationMode>(mode)));
}

Local array_buffer_from_backing_store(Isolate* isolate, size_t ptr) {
  return to_ffi(
      v8::ArrayBuffer::New(isolate, *reinterpret_cast<std::shared_ptr<v8::BackingStore>*>(ptr)));
}

size_t local_array_buffer_byte_length(const Local& buffer) {
  return local_as_ref_from_ffi<v8::ArrayBuffer>(buffer)->ByteLength();
}

uint8_t* local_array_buffer_data(const Local& buffer) {
  return static_cast<uint8_t*>(local_as_ref_from_ffi<v8::ArrayBuffer>(buffer)->Data());
}

size_t local_array_buffer_get_backing_store(const Local& buffer) {
  return reinterpret_cast<size_t>(new std::shared_ptr<v8::BackingStore>(
      local_as_ref_from_ffi<v8::ArrayBuffer>(buffer)->GetBackingStore()));
}

// Local<ArrayBufferView>
size_t local_array_buffer_view_byte_offset(const Local& view) {
  return local_as_ref_from_ffi<v8::ArrayBufferView>(view)->ByteOffset();
}

size_t local_array_buffer_view_byte_length(const Local& view) {
  return local_as_ref_from_ffi<v8::ArrayBufferView>(view)->ByteLength();
}

uint8_t* local_array_buffer_view_buffer_data(const Local& view) {
  return static_cast<uint8_t*>(local_as_ref_from_ffi<v8::ArrayBufferView>(view)->Buffer()->Data());
}

Local local_array_buffer_view_get_buffer(Isolate* isolate, const Local& view) {
  return to_ffi(local_as_ref_from_ffi<v8::ArrayBufferView>(view)->Buffer());
}

size_t local_array_buffer_view_element_size(const Local& view) {
  auto& v8Val = local_as_ref_from_ffi<v8::Value>(view);
  if (v8Val->IsUint8Array() || v8Val->IsInt8Array() || v8Val->IsUint8ClampedArray()) return 1;
  if (v8Val->IsUint16Array() || v8Val->IsInt16Array()) return 2;
  if (v8Val->IsUint32Array() || v8Val->IsInt32Array() || v8Val->IsFloat32Array()) return 4;
  if (v8Val->IsFloat64Array() || v8Val->IsBigInt64Array() || v8Val->IsBigUint64Array()) return 8;
  return 0;  // DataView — no fixed element size
}

bool local_array_buffer_view_is_integer_type(const Local& view) {
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

bool local_array_buffer_was_detached(const Local& buffer) {
  return local_as_ref_from_ffi<v8::ArrayBuffer>(buffer)->WasDetached();
}

bool local_array_buffer_is_detachable(const Local& buffer) {
  return local_as_ref_from_ffi<v8::ArrayBuffer>(buffer)->IsDetachable();
}

// ArrayBuffer is_shared (value-level check)
bool local_array_buffer_is_shared(const Local& value) {
  return local_as_ref_from_ffi<v8::Value>(value)->IsSharedArrayBuffer();
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
MaybeLocal wrap_resource(Isolate* isolate, kj::Rc<Wrappable> wrappable, const Global& tmpl) {
  if (wrappable->hasWrapper() && wrappable->getIsolate() != isolate) {
    return MaybeLocal{.ptr = 0};
  }

  // Check if already wrapped
  KJ_IF_SOME(handle, wrappable->tryGetHandle(isolate)) {
    return maybe_local_to_ffi(v8::MaybeLocal<v8::Value>(handle));
  }

  auto& global_tmpl = global_as_ref_from_ffi<v8::FunctionTemplate>(tmpl);
  auto local_tmpl = v8::Local<v8::FunctionTemplate>::New(isolate, global_tmpl);
  v8::Local<v8::Object> object = ::workerd::jsg::check(
      local_tmpl->InstanceTemplate()->NewInstance(isolate->GetCurrentContext()));

  // attachWrapper sets up CppgcShim, TracedReference, internal fields, etc.
  wrappable->attachWrapper(isolate, object, true,
      static_cast<v8::CppHeapPointerTag>(::workerd::jsg::kNonResourceWrappableTag));

  // Override tag to identify as Rust object for unwrapping
  auto tagAddress = const_cast<uint16_t*>(&::workerd::jsg::Wrappable::WORKERD_RUST_WRAPPABLE_TAG);
  object->SetAlignedPointerInInternalField(::workerd::jsg::Wrappable::WRAPPABLE_TAG_FIELD_INDEX,
      tagAddress,
      static_cast<v8::EmbedderDataTypeTag>(::workerd::jsg::Wrappable::WRAPPABLE_TAG_FIELD_INDEX));

  return maybe_local_to_ffi(v8::MaybeLocal<v8::Value>(object));
}

void wrappable_attach_wrapper(kj::Rc<Wrappable> wrappable, FunctionCallbackInfo& args) {
  auto* isolate = args.GetIsolate();
  auto object = args.This();

  // attachWrapper sets up CppgcShim, TracedReference, internal fields, etc.
  wrappable->attachWrapper(isolate, object, true,
      static_cast<v8::CppHeapPointerTag>(::workerd::jsg::kNonResourceWrappableTag));

  // Override tag to identify as Rust object for unwrapping
  auto tagAddress = const_cast<uint16_t*>(&::workerd::jsg::Wrappable::WORKERD_RUST_WRAPPABLE_TAG);
  object->SetAlignedPointerInInternalField(::workerd::jsg::Wrappable::WRAPPABLE_TAG_FIELD_INDEX,
      tagAddress,
      static_cast<v8::EmbedderDataTypeTag>(::workerd::jsg::Wrappable::WRAPPABLE_TAG_FIELD_INDEX));
}

// Unwrappers

bool unwrap_string(Isolate* isolate, Local value, ::rust::String& result) {
  auto v8Value = local_from_ffi<v8::Value>(kj::mv(value));
  // Fast path: a string primitive needs no coercion and can't throw. (String wrapper objects are
  // not IsString.)
  v8::Local<v8::String> v8Str;
  if (v8Value->IsString()) {
    v8Str = v8Value.As<v8::String>();
  } else if (!v8Value->ToString(isolate->GetCurrentContext()).ToLocal(&v8Str)) {
    return false;
  }
  v8::String::ValueView view(isolate, v8Str);
  if (!view.is_one_byte()) {
    // lossy(): lone surrogates become U+FFFD instead of throwing (which aborts).
    result = ::rust::String::lossy(reinterpret_cast<const char16_t*>(view.data16()), view.length());
  } else {
    result = ::rust::String::latin1(reinterpret_cast<const char*>(view.data8()), view.length());
  }
  return true;
}

bool unwrap_boolean(Isolate* isolate, Local value) {
  return local_from_ffi<v8::Value>(kj::mv(value))->ToBoolean(isolate)->Value();
}

bool unwrap_number(Isolate* isolate, Local value, double& result) {
  auto v8Value = local_from_ffi<v8::Value>(kj::mv(value));
  // Fast path: a number primitive needs no coercion and can't throw.
  if (v8Value->IsNumber()) {
    result = v8Value.As<v8::Number>()->Value();
    return true;
  }
  return v8Value->NumberValue(isolate->GetCurrentContext()).To(&result);
}

namespace {
// Creates no handles, so fast callbacks can use it without a HandleScope.
kj::Maybe<Wrappable&> tryUnwrapRustWrappable(Isolate* isolate, v8::Local<v8::Value> v8_val) {
  // Non-object values (numbers, strings, booleans, etc.) are never wrapped resources.
  if (!v8_val->IsObject()) return kj::none;
  auto v8_obj = v8_val.As<v8::Object>();
  // Plain JS objects have no internal fields; check before reading to avoid V8 fatal error.
  if (v8_obj->InternalFieldCount() < ::workerd::jsg::Wrappable::INTERNAL_FIELD_COUNT ||
      v8_obj->GetAlignedPointerFromInternalField(
          ::workerd::jsg::Wrappable::WRAPPABLE_TAG_FIELD_INDEX,
          static_cast<v8::EmbedderDataTypeTag>(
              ::workerd::jsg::Wrappable::WRAPPABLE_TAG_FIELD_INDEX)) !=
          const_cast<uint16_t*>(&::workerd::jsg::Wrappable::WORKERD_RUST_WRAPPABLE_TAG)) {
    return kj::none;
  }
  // The WORKERD_RUST_WRAPPABLE_TAG marker check above established that this is one of our Rust
  // wrappables, so the pointer can be taken directly through the shim.
  auto* wrappable = ::workerd::jsg::Wrappable::unwrapFromShimAnyType(isolate, v8_obj);
  if (wrappable == nullptr) {
    // unwrapFromShimAnyType() should never return null for a marked object.
    KJ_LOG(FATAL, "wrapper type mismatch: marked object's CppHeap handle resolves to no wrappable");
    abort();
  }
  return *static_cast<Wrappable*>(reinterpret_cast<::workerd::jsg::Wrappable*>(wrappable));
}
}  // namespace

kj::Maybe<kj::Rc<Wrappable>> unwrap_resource(Isolate* isolate, Local value) {
  KJ_IF_SOME(wrappable, tryUnwrapRustWrappable(isolate, local_from_ffi<v8::Value>(kj::mv(value)))) {
    return wrappable.toRc();
  }
  return kj::none;
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
bool local_array_iterate(Isolate* isolate, Local value, ::rust::Vec<Global>& result) {
  auto context = isolate->GetCurrentContext();
  auto v8Val = local_from_ffi<v8::Value>(kj::mv(value));

  KJ_REQUIRE(v8Val->IsArray(), "Value must be an array");
  auto arr = v8Val.As<v8::Array>();

  struct Data {
    Isolate* isolate;
    ::rust::Vec<Global>* result;
  };
  Data data{isolate, &result};

  auto iterateResult = arr->Iterate(context,
      [](uint32_t index, v8::Local<v8::Value> element,
          void* userData) -> v8::Array::CallbackResult {
    auto* d = static_cast<Data*>(userData);
    d->result->push_back(to_ffi(v8::Global<v8::Value>(d->isolate, element)));
    return v8::Array::CallbackResult::kContinue;
  },
      &data);
  if (iterateResult.IsNothing()) {
    // A getter or Proxy trap on an element threw. ffi::Global has no destructor in Rust, so the
    // handles collected so far would stay strong roots until the isolate is disposed.
    for (auto& global: result) {
      global_reset(global);
    }
    result.clear();
    return false;
  }
  return true;
}

// Local<TypedArray>
Local uint8_array_from_buffer(
    Isolate* isolate, const Local& buffer, size_t byte_offset, size_t length) {
  auto arrayBuffer = local_as_ref_from_ffi<v8::ArrayBuffer>(buffer);
  KJ_REQUIRE(
      byte_offset <= arrayBuffer->ByteLength() && length <= arrayBuffer->ByteLength() - byte_offset,
      "Uint8Array view is out of bounds of its ArrayBuffer");
  return to_ffi(v8::Uint8Array::New(arrayBuffer, byte_offset, length));
}

size_t local_typed_array_length(const Local& array) {
  return local_as_ref_from_ffi<v8::TypedArray>(array)->Length();
}

uintptr_t local_typed_array_buffer_data(const Local& array) {
  return reinterpret_cast<uintptr_t>(
      local_as_ref_from_ffi<v8::TypedArray>(array)->Buffer()->Data());
}

size_t local_typed_array_byte_offset(const Local& array) {
  return local_as_ref_from_ffi<v8::TypedArray>(array)->ByteOffset();
}

size_t local_typed_array_byte_length(const Local& array) {
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

Isolate* gc_visitor_v8_isolate(GcVisitor* visitor) {
  return gc_visitor_from_ffi(visitor)->v8Isolate();
}

void global_clear_weak(uintptr_t* global) {
  auto& handle = *reinterpret_cast<v8::Global<v8::Value>*>(global);
  if (!handle.IsEmpty()) {
    handle.ClearWeak<void>();
  }
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

bool fci_is_construct_call(FunctionCallbackInfo* args) {
  return args->IsConstructCall();
}

Local fci_get_arg(FunctionCallbackInfo* args, size_t index) {
  return to_ffi((*args)[index]);
}

void fci_set_return_value(FunctionCallbackInfo* args, Local value) {
  args->GetReturnValue().Set(local_from_ffi<v8::Value>(kj::mv(value)));
}

kj::Maybe<kj::Rc<Wrappable>> fci_get_data_wrappable(FunctionCallbackInfo* args) {
  auto data = args->DataV2().As<v8::Value>();
  KJ_IF_SOME(wrappable, ::workerd::jsg::Wrappable::tryUnwrapOpaque(args->GetIsolate(), data)) {
    // C++ JSG's wrapped functions also have an opaque wrapper as their data, so check the type,
    // as ::workerd::jsg::downcastWrappable() does.
    const auto& actualType = typeid(wrappable);
    if (&actualType == &typeid(Wrappable) || actualType == typeid(Wrappable)) {
      return kj::downcast<Wrappable>(wrappable).toRc();
    }
  }
  return kj::none;
}

Isolate* pci_get_isolate(PropertyCallbackInfo* info) {
  return info->GetIsolate();
}

Local pci_get_this(PropertyCallbackInfo* info) {
  return to_ffi(info->Holder());
}

void pci_set_return_value(PropertyCallbackInfo* info, Local value) {
  info->GetReturnValue().Set(local_from_ffi<v8::Value>(kj::mv(value)));
}

Isolate* psci_get_isolate(PropertySetterCallbackInfo* info) {
  return info->GetIsolate();
}

Local psci_get_this(PropertySetterCallbackInfo* info) {
  return to_ffi(info->Holder());
}

Isolate* pqci_get_isolate(PropertyQueryCallbackInfo* info) {
  return info->GetIsolate();
}

Local pqci_get_this(PropertyQueryCallbackInfo* info) {
  return to_ffi(info->Holder());
}

// FastApiCallbackOptions
TraitObjectPtr fast_api_unwrap_receiver(FastApiCallbackOptions* options, Local receiver) {
  KJ_IF_SOME(wrappable,
      tryUnwrapRustWrappable(options->isolate, local_from_ffi<v8::Value>(kj::mv(receiver)))) {
    return wrappable.trait_object;
  }
  return {};
}

void fast_api_throw_exception(
    FastApiCallbackOptions* options, ExceptionType exceptionType, ::rust::Str message) {
  v8::HandleScope scope(options->isolate);
  isolate_throw_exception(options->isolate,
      exception_create_from_bytes(options->isolate, exceptionType,
          reinterpret_cast<const uint8_t*>(message.data()), static_cast<int32_t>(message.size())));
}

void fast_api_throw_internal_error(FastApiCallbackOptions* options, ::rust::Str internalMessage) {
  v8::HandleScope scope(options->isolate);
  isolate_throw_internal_error(options->isolate, internalMessage);
}

void fast_api_terminate_execution(FastApiCallbackOptions* options) {
  isolate_terminate_execution(options->isolate);
}

namespace {
v8::CTypeInfo cTypeOf(FastApiType type) {
  switch (type) {
    case FastApiType::Void:
      return v8::CTypeInfo(v8::CTypeInfo::Type::kVoid);
    case FastApiType::Bool:
      return v8::CTypeInfo(v8::CTypeInfo::Type::kBool);
    case FastApiType::Float64:
      return v8::CTypeInfo(v8::CTypeInfo::Type::kFloat64);
  }
  KJ_UNREACHABLE;
}

// V8 keeps a pointer to the CFunction, so it lives for the rest of the process.
struct FastCFunction {
  FastCFunction(const FastApiDescriptor& descriptor)
      : returnType(cTypeOf(descriptor.return_type)),
        argTypes(buildArgTypes(descriptor)),
        info(returnType, argTypes.size(), argTypes.begin()),
        function(reinterpret_cast<const void*>(descriptor.callback), &info) {}

  static kj::Array<v8::CTypeInfo> buildArgTypes(const FastApiDescriptor& descriptor) {
    auto builder = kj::heapArrayBuilder<v8::CTypeInfo>(descriptor.arg_types.size() + 2);
    builder.add(v8::CTypeInfo(v8::CTypeInfo::Type::kV8Value));
    for (auto type: descriptor.arg_types) {
      builder.add(cTypeOf(type));
    }
    builder.add(v8::CTypeInfo(v8::CTypeInfo::kCallbackOptionsType));
    return builder.finish();
  }

  v8::CTypeInfo returnType;
  kj::Array<v8::CTypeInfo> argTypes;
  v8::CFunctionInfo info;
  v8::CFunction function;
};

const v8::CFunction& getFastCFunction(const FastApiDescriptor& descriptor) {
  using Map = kj::HashMap<size_t, kj::Own<FastCFunction>>;
  static kj::MutexGuarded<Map> functions;
  auto lock = functions.lockExclusive();
  return lock
      ->findOrCreate(descriptor.callback, [&]() -> Map::Entry {
    return {descriptor.callback, kj::heap<FastCFunction>(descriptor)};
  })->function;
}

// The descriptor callback of a wildcard property, as in C++ JSG's WildcardPropertyCallbacks
// (resource.h): a wildcard property is never an own property, so `hasOwnProperty()` is false
// and `Object.getOwnPropertyDescriptor()` is undefined for it.
v8::Intercepted wildcardPropertyDescriptor(
    v8::Local<v8::Name> name, const v8::PropertyCallbackInfo<v8::Value>& info) {
  return v8::Intercepted::kNo;
}

// The Rust callbacks of a wildcard property return the `v8::Intercepted` as a `u32`, and take
// the `v8::Local<v8::Name>` as a `usize` and the `PropertyCallbackInfo` by pointer.
static_assert(std::is_same_v<std::underlying_type_t<v8::Intercepted>, uint32_t>);
static_assert(static_cast<uint32_t>(v8::Intercepted::kYes) == 0);
static_assert(static_cast<uint32_t>(v8::Intercepted::kNo) == 1);
static_assert(std::is_trivially_copyable_v<v8::Local<v8::Name>>);
static_assert(sizeof(v8::Local<v8::Name>) == sizeof(size_t));

// Installs the wildcard property on `instance`, with the flags of C++ JSG's
// WildcardPropertyCallbacks: properties of the object or its prototypes take precedence
// (`kNonMasking`), symbols are not intercepted (`kOnlyInterceptStrings`), and the debugger may
// call the callbacks while evaluating without side effects (`kHasNoSideEffect`).
template <typename RegisterExternalRef>
void setWildcardProperty(v8::Local<v8::ObjectTemplate> instance,
    const WildcardPropertyDescriptor& wildcard,
    RegisterExternalRef& registerExternalRef) {
  registerExternalRef(wildcard.getter_callback);
  registerExternalRef(wildcard.query_callback);
  registerExternalRef(reinterpret_cast<size_t>(&wildcardPropertyDescriptor));
  instance->SetHandler(v8::NamedPropertyHandlerConfiguration(
      reinterpret_cast<v8::NamedPropertyGetterCallback>(wildcard.getter_callback), nullptr,
      reinterpret_cast<v8::NamedPropertyQueryCallback>(wildcard.query_callback), nullptr, nullptr,
      nullptr, &wildcardPropertyDescriptor, v8::Local<v8::Value>(),
      static_cast<v8::PropertyHandlerFlags>(
          static_cast<int>(v8::PropertyHandlerFlags::kNonMasking) |
          static_cast<int>(v8::PropertyHandlerFlags::kHasNoSideEffect) |
          static_cast<int>(v8::PropertyHandlerFlags::kOnlyInterceptStrings))));
}

// The constructor of a resource without a #[jsg_constructor], with the error that C++ JSG's
// throwIllegalConstructor() throws.
void throwIllegalConstructor(const v8::FunctionCallbackInfo<v8::Value>& args) {
  auto* isolate = args.GetIsolate();
  if (isolate->IsExecutionTerminating()) return;
  isolate->ThrowException(v8::Exception::TypeError(v8::String::NewFromUtf8Literal(
      isolate, "Illegal constructor", v8::NewStringType::kInternalized)));
}
}  // namespace

Global create_resource_template(Isolate* isolate, const ResourceDescriptor& descriptor) {
  // Construct lazily.
  v8::EscapableHandleScope scope(isolate);

  // Every callback address baked into the templates below must be registered as a V8
  // external reference so that templates (and functions instantiated from them) can be
  // serialized into a startup snapshot. No-op unless the isolate is in a snapshot mode.
  auto registerExternalRef = [&](size_t callback) {
    workerd::jsg::isolateRegisterExternalReference(isolate, static_cast<intptr_t>(callback));
  };

  v8::Local<v8::FunctionTemplate> constructor;
  KJ_IF_SOME(descriptor, descriptor.constructor) {
    constructor = v8::FunctionTemplate::New(isolate,
        reinterpret_cast<v8::FunctionCallback>(reinterpret_cast<void*>(descriptor.callback)));
    registerExternalRef(descriptor.callback);
  } else {
    constructor = v8::FunctionTemplate::New(isolate, &throwIllegalConstructor);
    registerExternalRef(reinterpret_cast<size_t>(&throwIllegalConstructor));
  }

  auto prototype = constructor->PrototypeTemplate();

  // Signatures protect our methods from being invoked with the wrong `this`.
  auto signature = v8::Signature::New(isolate, constructor);

  auto instance = constructor->InstanceTemplate();

  instance->SetInternalFieldCount(workerd::jsg::Wrappable::INTERNAL_FIELD_COUNT);

  auto classname = newUtf8String(isolate,
      kj::arrayPtr(descriptor.name.data(), descriptor.name.size()), v8::NewStringType::kNormal);

  if (workerd::jsg::getShouldSetToStringTag(isolate)) {
    prototype->Set(v8::Symbol::GetToStringTag(isolate), classname, v8::PropertyAttribute::DontEnum);
  }

  auto internalMarker =
      v8::Symbol::For(isolate, ::workerd::jsg::v8StrIntern(isolate, "cloudflare:internal-class"));
  prototype->Set(internalMarker, internalMarker,
      static_cast<v8::PropertyAttribute>(v8::PropertyAttribute::DontEnum |
          v8::PropertyAttribute::DontDelete | v8::PropertyAttribute::ReadOnly));

  if (workerd::jsg::getShouldSetImmutablePrototype(isolate)) {
    constructor->ReadOnlyPrototype();
  }

  constructor->SetClassName(classname);

  const bool fastApiEnabled = ::workerd::jsg::IsolateBase::from(isolate).isFastApiEnabled();

  auto makeMethod = [&](const MethodDescriptor& method, v8::Local<v8::Signature> methodSignature) {
    registerExternalRef(method.callback);
    auto callback =
        reinterpret_cast<v8::FunctionCallback>(reinterpret_cast<void*>(method.callback));
    if (fastApiEnabled) {
      KJ_IF_SOME(fast, method.fast) {
        const auto& cFunction = getFastCFunction(fast);
        registerExternalRef(reinterpret_cast<size_t>(&cFunction));
        return v8::FunctionTemplate::NewWithCFunctionOverloads(isolate, callback,
            v8::Local<v8::Value>(), methodSignature, 0, v8::ConstructorBehavior::kThrow,
            v8::SideEffectType::kHasSideEffect, {&cFunction, 1});
      }
    }
    return v8::FunctionTemplate::New(isolate, callback, v8::Local<v8::Value>(), methodSignature, 0,
        v8::ConstructorBehavior::kThrow);
  };

  for (const auto& method: descriptor.static_methods) {
    auto functionTemplate = makeMethod(method, v8::Local<v8::Signature>());
    functionTemplate->RemovePrototype();
    constructor->Set(makeInternedStr(isolate, method.name), functionTemplate);
  }

  for (const auto& method: descriptor.methods) {
    auto functionTemplate = makeMethod(method, signature);
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
      registerExternalRef(callback);
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
        // Mirrors registerInstanceProperty / registerReadonlyInstanceProperty in resource.h: a
        // native data property, which reads as a plain data property from JS.
        // The Rust callbacks take each v8::Local as a usize. That matches the C++ signature on
        // every ABI we target because v8::Local is one trivially copyable pointer.
        static_assert(std::is_trivially_copyable_v<v8::Local<v8::Value>>);
        static_assert(sizeof(v8::Local<v8::Value>) == sizeof(size_t));
        registerExternalRef(prop.getter_callback);
        auto getter = reinterpret_cast<v8::AccessorNameGetterCallback>(prop.getter_callback);
        KJ_IF_SOME(setterCb, prop.setter_callback) {
          registerExternalRef(setterCb);
          instance->SetNativeDataProperty(v8Name, getter,
              reinterpret_cast<v8::AccessorNameSetterCallbackV2>(setterCb), v8::Local<v8::Value>(),
              v8::PropertyAttribute::None);
        } else {
          instance->SetNativeDataProperty(
              v8Name, getter, nullptr, v8::Local<v8::Value>(), v8::PropertyAttribute::ReadOnly);
        }
        break;
      }
      case PropertyKind::LazyInstance:
      case PropertyKind::LazyReadonlyInstance: {
        // Mirrors registerLazyInstanceProperty in resource.h: V8 runs the getter on first access
        // and replaces the property with a data property holding its result. The getter is an
        // AccessorNameGetterCallback, like the Instance kind's (see the static_asserts there).
        registerExternalRef(prop.getter_callback);
        auto getter = reinterpret_cast<v8::AccessorNameGetterCallback>(prop.getter_callback);
        auto attributes = prop.kind == PropertyKind::LazyReadonlyInstance
            ? v8::PropertyAttribute::ReadOnly
            : v8::PropertyAttribute::None;
        instance->SetLazyDataProperty(v8Name, getter, v8::Local<v8::Value>(), attributes);
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

  KJ_IF_SOME(wildcard, descriptor.wildcard_property) {
    setWildcardProperty(instance, wildcard, registerExternalRef);
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

// Mirrors registerNestedType() (resource.h): the nested type's constructor is a property of the
// prototype, so that it is inherited along with the prototype.
void function_template_set_nested_type(
    Isolate* isolate, const Global& tmpl, ::rust::Str name, const Global& nested) {
  v8::HandleScope scope(isolate);
  auto local_tmpl = v8::Local<v8::FunctionTemplate>::New(
      isolate, global_as_ref_from_ffi<v8::FunctionTemplate>(tmpl));
  auto local_nested = v8::Local<v8::FunctionTemplate>::New(
      isolate, global_as_ref_from_ffi<v8::FunctionTemplate>(nested));
  local_tmpl->PrototypeTemplate()->Set(makeInternedStr(isolate, name), local_nested);
}

// Realm
Realm* realm_from_isolate(Isolate* isolate) {
  auto* realm =
      static_cast<Realm*>(isolate->GetData(::workerd::jsg::SetDataIndex::SET_DATA_RUST_REALM));
  KJ_ASSERT(realm != nullptr, "Rust Realm not set on isolate");
  return realm;
}

namespace {
// The IsolateBase deferred action hook of an isolate with a Rust Realm.
void resetGlobalsAwaitingLock(v8::Isolate* isolate) {
  realm_reset_globals_awaiting_lock(isolate);
}

// The IsolateBase dispose hook that a Realm sets when it is dropped, with the reference to its
// IsolateState that it hands over as data.
void isolateDisposed(void* data) {
  isolate_disposed(static_cast<const IsolateState*>(data));
}
}  // namespace

bool realm_can_attach(Isolate* isolate) {
  return isolate->GetData(::workerd::jsg::SetDataIndex::SET_DATA_RUST_REALM) == nullptr &&
      !::workerd::jsg::IsolateBase::from(isolate).hasDisposeHook();
}

void realm_attach(Isolate* isolate, Realm& realm) {
  isolate->SetData(::workerd::jsg::SetDataIndex::SET_DATA_RUST_REALM, &realm);
  ::workerd::jsg::IsolateBase::from(isolate).setDeferredActionHook(&resetGlobalsAwaitingLock);
}

void realm_detach(Isolate* isolate, const IsolateState* state) {
  auto& isolateBase = ::workerd::jsg::IsolateBase::from(isolate);
  isolateBase.setDeferredActionHook(nullptr);
  isolateBase.setDisposeHook(&isolateDisposed, const_cast<IsolateState*>(state));
  isolate->SetData(::workerd::jsg::SetDataIndex::SET_DATA_RUST_REALM, nullptr);
}

// Errors
namespace {
Local exceptionCreateFromMessage(ExceptionType exception_type, v8::Local<v8::String> message) {
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
}  // namespace

Local exception_create_from_bytes(
    Isolate* isolate, ExceptionType exception_type, const uint8_t* data, int32_t length) {
  // kNormal (not kInternalized): error messages are arbitrary, potentially
  // non-UTF-8 byte strings (e.g. filesystem paths) and should not pollute the
  // internalized string table (which is never GC'd). NewFromUtf8 replaces
  // invalid sequences with U+FFFD, matching the legacy jsg::Lock::error() /
  // js.str() behavior.
  auto message = newUtf8String(isolate,
      kj::arrayPtr(reinterpret_cast<const char*>(data), static_cast<size_t>(length)),
      v8::NewStringType::kNormal);
  return exceptionCreateFromMessage(exception_type, message);
}

// TryCatch
bool catch_exception(v8::TryCatch& tryCatch, MaybeLocal& exception) {
  // The rule of jsg::Lock::tryCatch(): nothing caught means a termination, which V8 may already
  // have cleared once all JavaScript frames unwound.
  if (!tryCatch.CanContinue() || !tryCatch.HasCaught() || tryCatch.Exception().IsEmpty()) {
    tryCatch.ReThrow();
    return false;
  }
  exception = maybe_local_to_ffi(v8::MaybeLocal<v8::Value>(tryCatch.Exception()));
  return true;
}

void isolate_try_catch(
    Isolate* isolate, size_t data, ::rust::Fn<bool(size_t)> callback, MaybeLocal& exception) {
  v8::TryCatch tryCatch(isolate);
  if (callback(data)) {
    catch_exception(tryCatch, exception);
  }
}

// Isolate
void isolate_throw_exception(Isolate* isolate, Local exception) {
  if (isolate->IsExecutionTerminating()) return;
  isolate->ThrowException(local_from_ffi<v8::Value>(kj::mv(exception)));
}

void isolate_throw_error(Isolate* isolate, ::rust::Str description) {
  // kNormal (not kInternalized): error messages are arbitrary caller-supplied
  // strings and should not pollute the internalized string table (which is never
  // GC'd), matching C++ jsg::Lock::v8Error() / v8Str() behavior.
  if (isolate->IsExecutionTerminating()) return;
  auto message = newUtf8String(
      isolate, kj::arrayPtr(description.data(), description.size()), v8::NewStringType::kNormal);
  isolate->ThrowError(message);
}

void isolate_throw_internal_error(Isolate* isolate, ::rust::Str internalMessage) {
  // kj::heapString(data, size) copies exactly `size` bytes and appends a NUL,
  // avoiding the out-of-bounds read that kj::StringPtr(data, size) would cause
  // on non-NUL-terminated Rust &str data.
  auto message = kj::heapString(internalMessage.data(), internalMessage.size());
  if (isolate->IsExecutionTerminating()) {
    // Logs the message, but creates no error: creating one while execution is terminating fails
    // a V8 debug check, and it would not be thrown.
    KJ_LOG(ERROR, message);
    return;
  }
  // makeInternalError() generates a unique error ID, logs the internal message (with ID) to
  // KJ_LOG(ERROR) for Sentry, and returns a generic "internal error; reference = <id>" JS Error.
  isolate->ThrowException(::workerd::jsg::makeInternalError(isolate, message));
}

void isolate_terminate_execution(Isolate* isolate) {
  ::workerd::jsg::IsolateBase::from(isolate).terminateExecution();
}

bool isolate_is_execution_terminating(Isolate* isolate) {
  return isolate->IsExecutionTerminating();
}

bool isolate_has_pending_exception(Isolate* isolate) {
  return isolate->HasPendingException();
}

void ignore_leak(size_t ptr) {
#if KJ_HAS_COMPILER_FEATURE(address_sanitizer) || defined(__SANITIZE_ADDRESS__)
  __lsan_ignore_object(reinterpret_cast<const void*>(ptr));
#endif
}

bool isolate_is_locked(Isolate* isolate) {
  return v8::Locker::IsLocked(isolate);
}

bool isolate_in_context(Isolate* isolate) {
  return isolate->InContext();
}

}  // namespace workerd::rust::jsg
