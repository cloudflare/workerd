#pragma once

#include <kj-rs/kj-rs.h>
#include <rust/cxx.h>
#include <v8.h>

#include <kj/function.h>

namespace workerd::rust::jsg {

using GlobalValue = size_t;
using LocalValue = size_t;
using LocalObject = size_t;
using Isolate = v8::Isolate;
using FunctionCallbackInfo = v8::FunctionCallbackInfo<v8::Value>;
using ModuleCallback = ::rust::Fn<LocalValue(v8::Isolate*)>;
using LocalFunctionTemplate = size_t;
using GlobalFunctionTemplate = size_t;

struct ModuleRegistry {
  virtual ~ModuleRegistry() = default;
  virtual void addBuiltinModule(::rust::Str specifier, ModuleCallback moduleCallback) = 0;
};

inline void register_add_builtin_module(
    ModuleRegistry& registry, ::rust::Str specifier, ModuleCallback callback) {
  registry.addBuiltinModule(specifier, kj::mv(callback));
}

static_assert(sizeof(v8::Local<v8::Value>) == 8, "Size should match");
static_assert(alignof(v8::Local<v8::Value>) == 8, "Alignment should match");

static_assert(sizeof(v8::Global<v8::Value>) == sizeof(GlobalFunctionTemplate), "Size should match");
static_assert(
    alignof(v8::Global<v8::Value>) == alignof(GlobalFunctionTemplate), "Alignment should match");

template <typename T>
inline GlobalValue to_ffi(v8::Global<T>&& value) {
  size_t result;
  auto ptr_void = reinterpret_cast<void*>(&result);
  new (ptr_void) v8::Global<T>(kj::mv(value));
  return result;
}

template <typename T>
inline LocalValue to_ffi(v8::Local<T>&& value) {
  size_t result;
  auto ptr_void = reinterpret_cast<void*>(&result);
  new (ptr_void) v8::Local<T>(kj::mv(value));
  return result;
}

template <typename T>
inline v8::Local<T> local_from_ffi(LocalValue value) {
  auto ptr_void = reinterpret_cast<void*>(&value);
  return *reinterpret_cast<v8::Local<T>*>(ptr_void);
}

template <typename T>
inline v8::Global<T> global_from_ffi(GlobalValue value) {
  auto ptr_void = reinterpret_cast<void*>(&value);
  return kj::mv(*reinterpret_cast<v8::Global<T>*>(ptr_void));
}

struct ResourceDescriptor;
size_t create_resource_template(v8::Isolate* isolate, const ResourceDescriptor& descriptor);

inline v8::Isolate* get_isolate(FunctionCallbackInfo* args) {
  return args->GetIsolate();
}

inline LocalObject get_this(FunctionCallbackInfo* args) {
  return to_ffi(args->This());
}

inline size_t get_length(FunctionCallbackInfo* args) {
  return args->Length();
}

inline LocalValue get_arg(FunctionCallbackInfo* args, size_t index) {
  return to_ffi((*args)[index]);
}

inline ::rust::String unwrap_string(Isolate* isolate, LocalValue value) {
  v8::Local<v8::String> v8Str;
  if (!local_from_ffi<v8::Value>(value)->ToString(isolate->GetCurrentContext()).ToLocal(&v8Str)) {
    KJ_UNIMPLEMENTED("wrong");
  }
  v8::String::ValueView view(isolate, v8Str);
  if (!view.is_one_byte()) {
    return ::rust::String(reinterpret_cast<const char16_t*>(view.data16()), view.length());
  }
  return ::rust::String::latin1(reinterpret_cast<const char*>(view.data8()), view.length());
}

inline LocalFunctionTemplate global_function_template_as_local(
    Isolate* isolate, GlobalFunctionTemplate tmpl) {
  v8::Global<v8::FunctionTemplate> glbl = global_from_ffi<v8::FunctionTemplate>(tmpl);
  auto local = v8::Local<v8::FunctionTemplate>::New(isolate, glbl);
  KJ_REQUIRE(tmpl == to_ffi(kj::mv(glbl)));  // We need to forget global and not to destroy it.
  return to_ffi(kj::mv(local));
}

LocalValue wrap_resource(Isolate* isolate, size_t resource, LocalFunctionTemplate tmpl);

size_t unwrap_resource(Isolate* isolate, LocalValue value);

LocalObject new_local_object(Isolate* isolate);

void set_local_object_property(
    Isolate* isolate, LocalObject object, const char* key, LocalValue value);

}  // namespace workerd::rust::jsg
