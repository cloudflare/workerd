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

struct ResourceDescriptor;

// Function declarations
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
void global_drop(Global value);
Global global_clone(const Global& value);
Local global_to_local(Isolate* isolate, const Global& value);

// Wrappers
Local wrap_resource(Isolate* isolate, size_t resource, const Global& tmpl, size_t drop_callback);

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

Global create_resource_template(v8::Isolate* isolate, const ResourceDescriptor& descriptor);

}  // namespace workerd::rust::jsg
