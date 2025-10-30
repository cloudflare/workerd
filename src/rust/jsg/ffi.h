#pragma once

#include <kj-rs/kj-rs.h>
#include <rust/cxx.h>
#include <v8.h>

#include <kj/function.h>

namespace workerd::rust::jsg {

using LocalValue = size_t;
using LocalObject = size_t;
using Isolate = v8::Isolate;
using FunctionCallbackInfo = v8::FunctionCallbackInfo<v8::Value>;
using ModuleCallback = ::rust::Fn<LocalValue(v8::Isolate*)>;

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

inline LocalValue to_repr(v8::Local<v8::Value> value) {
  size_t result;
  auto ptr_void = reinterpret_cast<void*>(&result);
  new (ptr_void) v8::Local<v8::Value>(value);
  return result;
}

inline v8::Local<v8::Value> from_repr(LocalValue value) {
  auto ptr_void = reinterpret_cast<void*>(&value);
  return *reinterpret_cast<v8::Local<v8::Value>*>(ptr_void);
}

struct ResourceDescriptor;
uint64_t instantiate_resource(v8::Isolate* isolate, const ResourceDescriptor& descriptor);

inline v8::Isolate* get_isolate(FunctionCallbackInfo* args) {
  return args->GetIsolate();
}

inline LocalObject get_this(FunctionCallbackInfo* args) {
  return to_repr(args->This());
}

inline size_t get_length(FunctionCallbackInfo* args) {
  return args->Length();
}

inline LocalValue get_arg(FunctionCallbackInfo* args, size_t index) {
  return to_repr((*args)[index]);
}

inline ::rust::String unwrap_string(Isolate* isolate, LocalValue value) {
  v8::Local<v8::String> v8Str;
  if (!from_repr(value)->ToString(isolate->GetCurrentContext()).ToLocal(&v8Str)) {
    KJ_UNIMPLEMENTED("wrong");
  }
  v8::String::ValueView view(isolate, v8Str);
  if (!view.is_one_byte()) {
    return ::rust::String(reinterpret_cast<const char16_t*>(view.data16()), view.length());
  }
  return ::rust::String::latin1(reinterpret_cast<const char*>(view.data8()), view.length());
}

}  // namespace workerd::rust::jsg
