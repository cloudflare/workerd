#pragma once

#include <kj-rs/kj-rs.h>
#include <rust/cxx.h>
#include <v8.h>

#include <kj/function.h>

// Forward declarations needed by v8.rs.h
namespace workerd::rust::jsg {
using Isolate = v8::Isolate;
using FunctionCallbackInfo = v8::FunctionCallbackInfo<v8::Value>;
struct ModuleRegistry;
struct Local;
struct Global;
using ModuleCallback = ::rust::Fn<Local(Isolate*)>;
}  // namespace workerd::rust::jsg

// Include after forward declarations
#include <workerd/rust/jsg/v8.rs.h>

namespace workerd::rust::jsg {

// Local<T>
static_assert(sizeof(v8::Local<v8::Value>) == 8, "Size should match");
static_assert(alignof(v8::Local<v8::Value>) == 8, "Alignment should match");

template <typename T>
inline Local to_ffi(v8::Local<T>&& value) {
  size_t result;
  auto ptr_void = reinterpret_cast<void*>(&result);
  new (ptr_void) v8::Local<T>(kj::mv(value));
  return Local{result};
}

template <typename T>
inline v8::Local<T> local_from_ffi(Local value) {
  auto ptr_void = reinterpret_cast<void*>(&value.ptr);
  return *reinterpret_cast<v8::Local<T>*>(ptr_void);
}

void local_drop(Local value);
Local local_clone(const Local& value);
Global local_to_global(Isolate* isolate, Local value);
Local local_new_number(Isolate* isolate, double value);
Local local_new_string(Isolate* isolate, ::rust::Str value);
Local local_new_object(Isolate* isolate);
bool local_eq(const Local& lhs, const Local& rhs);

// Local<Object>
void local_object_set_property(Isolate* isolate, Local& object, ::rust::Str key, Local value);

// Global<T>
static_assert(sizeof(v8::Global<v8::Value>) == sizeof(Global), "Size should match");
static_assert(alignof(v8::Global<v8::Value>) == alignof(Global), "Alignment should match");

void global_drop(Global value);
Global global_clone(const Global& value);
Local global_to_local(Isolate* isolate, const Global& value);

template <typename T>
inline Global to_ffi(v8::Global<T>&& value) {
  size_t result;
  auto ptr_void = reinterpret_cast<void*>(&result);
  new (ptr_void) v8::Global<T>(kj::mv(value));
  return Global{result};
}

template <typename T>
inline v8::Global<T> global_from_ffi(Global value) {
  auto ptr_void = reinterpret_cast<void*>(&value.ptr);
  return kj::mv(*reinterpret_cast<v8::Global<T>*>(ptr_void));
}

// Wrappers
Local wrap_resource(Isolate* isolate, size_t resource, const Global& tmpl);

// Unwrappers
::rust::String unwrap_string(Isolate* isolate, Local value);
size_t unwrap_resource(Isolate* isolate, Local value);

// FunctionCallbackInfo
v8::Isolate* fci_get_isolate(FunctionCallbackInfo* args);
Local fci_get_this(FunctionCallbackInfo* args);
size_t fci_get_length(FunctionCallbackInfo* args);
Local fci_get_arg(FunctionCallbackInfo* args, size_t index);
void fci_set_return_value(FunctionCallbackInfo* args, Local value);

struct ModuleRegistry {
  virtual ~ModuleRegistry() = default;
  virtual void addBuiltinModule(::rust::Str specifier, ModuleCallback moduleCallback) = 0;
};

inline void register_add_builtin_module(
    ModuleRegistry& registry, ::rust::Str specifier, ModuleCallback callback) {
  registry.addBuiltinModule(specifier, kj::mv(callback));
}

struct ResourceDescriptor;
Global create_resource_template(v8::Isolate* isolate, const ResourceDescriptor& descriptor);

}  // namespace workerd::rust::jsg
