#pragma once

#include <workerd/jsg/modules.capnp.h>

#include <kj-rs/kj-rs.h>
#include <rust/cxx.h>
#include <v8.h>

#include <kj/function.h>
#include <kj/memory.h>

// Forward declarations needed by v8.rs.h
namespace workerd::rust::jsg {
using Isolate = v8::Isolate;
using Context = v8::Local<v8::Context>;
using FunctionCallbackInfo = v8::FunctionCallbackInfo<v8::Value>;
using WeakCallbackInfo = v8::WeakCallbackInfo<void>;
struct ModuleRegistry;
struct Local;
struct Global;
struct Realm;
enum class ExceptionType : ::std::uint8_t;
using ModuleType = ::workerd::jsg::ModuleType;
using ModuleCallback = ::rust::Fn<Local(Isolate*)>;
using WeakCallback = ::rust::Fn<void(Isolate*, size_t)>;

struct ResourceDescriptor;

// Function declarations
void local_drop(Local value);
Local local_clone(const Local& value);
Global local_to_global(Isolate* isolate, Local value);
Local local_new_number(Isolate* isolate, double value);
Local local_new_string(Isolate* isolate, ::rust::Str value);
Local local_new_boolean(Isolate* isolate, bool value);
Local local_new_object(Isolate* isolate);
bool local_eq(const Local& lhs, const Local& rhs);
bool local_has_value(const Local& val);
bool local_is_string(const Local& val);
bool local_is_boolean(const Local& val);
bool local_is_number(const Local& val);
bool local_is_null(const Local& val);
bool local_is_undefined(const Local& val);
bool local_is_null_or_undefined(const Local& val);
bool local_is_object(const Local& val);
::rust::String local_type_of(Isolate* isolate, const Local& val);

// Local<Object>
void local_object_set_property(Isolate* isolate, Local& object, ::rust::Str key, Local value);
bool local_object_has_property(Isolate* isolate, const Local& object, ::rust::Str key);
kj::Maybe<Local> local_object_get_property(Isolate* isolate, const Local& object, ::rust::Str key);

// Global<T>
void global_drop(Global value);
Global global_clone(const Global& value);
Local global_to_local(Isolate* isolate, const Global& value);
void global_make_weak(
    Isolate* isolate, Global* value, size_t /* void* */ data, WeakCallback callback);

// Wrappers
Local wrap_resource(Isolate* isolate, size_t resource, const Global& tmpl, size_t drop_callback);

// Unwrappers
::rust::String unwrap_string(Isolate* isolate, Local value);
bool unwrap_boolean(Isolate* isolate, Local value);
double unwrap_number(Isolate* isolate, Local value);
size_t unwrap_resource(Isolate* isolate, Local value);

// FunctionCallbackInfo
Isolate* fci_get_isolate(FunctionCallbackInfo* args);
Local fci_get_this(FunctionCallbackInfo* args);
size_t fci_get_length(FunctionCallbackInfo* args);
Local fci_get_arg(FunctionCallbackInfo* args, size_t index);
void fci_set_return_value(FunctionCallbackInfo* args, Local value);

struct ModuleRegistry {
  virtual ~ModuleRegistry() = default;
  virtual void addBuiltinModule(
      ::rust::Str specifier, ModuleCallback moduleCallback, ModuleType moduleType) = 0;
};

inline void register_add_builtin_module(ModuleRegistry& registry,
    ::rust::Str specifier,
    ModuleCallback callback,
    ModuleType moduleType) {
  registry.addBuiltinModule(specifier, kj::mv(callback), moduleType);
}

Global create_resource_template(Isolate* isolate, const ResourceDescriptor& descriptor);

// Realm
Realm* realm_from_isolate(Isolate* isolate);

// Errors
Local exception_create(Isolate* isolate, ExceptionType exception_type, ::rust::Str message);

// Isolate
void isolate_throw_exception(Isolate* isolate, Local exception);
void isolate_throw_error(Isolate* isolate, ::rust::Str message);
bool isolate_is_locked(Isolate* isolate);

}  // namespace workerd::rust::jsg
