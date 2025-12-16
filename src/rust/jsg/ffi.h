#pragma once

#include <cppgc/allocation.h>
#include <cppgc/garbage-collected.h>
#include <cppgc/member.h>
#include <cppgc/name-provider.h>
#include <cppgc/persistent.h>
#include <cppgc/visitor.h>
#include <kj-rs/kj-rs.h>
#include <rust/cxx.h>
#include <v8-cppgc.h>
#include <v8.h>

#include <kj/function.h>
#include <kj/memory.h>

// Forward declarations needed by v8.rs.h
namespace workerd::rust::jsg {
using Isolate = v8::Isolate;
using FunctionCallbackInfo = v8::FunctionCallbackInfo<v8::Value>;
struct ModuleRegistry;
struct Local;
struct Global;
struct Realm;
struct TracedReference;
struct CppgcVisitor;
struct RustResourceData;
class RustResource;
enum class ExceptionType : ::std::uint8_t;
using ModuleCallback = ::rust::Fn<Local(Isolate*)>;
using CppgcPersistent = cppgc::Persistent<RustResource>;
using CppgcWeakPersistent = cppgc::WeakPersistent<RustResource>;
using CppgcMember = cppgc::Member<RustResource>;
using CppgcWeakMember = cppgc::WeakMember<RustResource>;

// CXX generates the ModuleType enum definition in v8.rs.h from the Rust shared enum.
// This forward declaration allows ffi.h to reference the type before that header is included.
enum class ModuleType : ::std::uint8_t;

struct ResourceDescriptor;

// Function declarations
void local_drop(Local value);
Local local_clone(const Local& value);
Global local_to_global(Isolate* isolate, Local value);
Local local_new_number(Isolate* isolate, double value);
Local local_new_string(Isolate* isolate, ::rust::Str value);
Local local_new_object(Isolate* isolate);
bool local_eq(const Local& lhs, const Local& rhs);
bool local_has_value(const Local& val);
bool local_is_string(const Local& val);

// Local<Object>
void local_object_set_property(Isolate* isolate, Local& object, ::rust::Str key, Local value);
bool local_object_has_property(Isolate* isolate, const Local& object, ::rust::Str key);
kj::Maybe<Local> local_object_get_property(Isolate* isolate, const Local& object, ::rust::Str key);

// Global<T>
void global_reset(Global* value);
Global global_clone(const Global& value);
Local global_to_local(Isolate* isolate, const Global& value);

// TracedReference (cppgc/Oilpan)
TracedReference traced_reference_from_local(Isolate* isolate, Local value);
Local traced_reference_to_local(Isolate* isolate, const TracedReference& value);
void traced_reference_reset(TracedReference* value);
bool traced_reference_is_empty(const TracedReference& value);

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
  virtual void addBuiltinModule(
      ::rust::Str specifier, ModuleCallback moduleCallback, ModuleType moduleType) = 0;
};

inline void register_add_builtin_module(
    ModuleRegistry& registry, ::rust::Str specifier, ModuleCallback callback, ModuleType type) {
  registry.addBuiltinModule(specifier, kj::mv(callback), type);
}

Global create_resource_template(v8::Isolate* isolate, const ResourceDescriptor& descriptor);

// Realm
Realm* realm_from_isolate(Isolate* isolate);

// Errors
Local exception_create(Isolate* isolate, ExceptionType exception_type, ::rust::Str message);

// Isolate
void isolate_throw_exception(Isolate* isolate, Local exception);
void isolate_throw_error(Isolate* isolate, ::rust::Str message);
bool isolate_is_locked(Isolate* isolate);

// cppgc
RustResource* cppgc_allocate(Isolate* isolate, RustResourceData data);
void cppgc_visitor_trace(CppgcVisitor* visitor, const TracedReference& handle);
void cppgc_visitor_trace_member(CppgcVisitor* visitor, const CppgcMember& member);
void cppgc_visitor_trace_weak_member(CppgcVisitor* visitor, const CppgcWeakMember& member);
kj::Own<CppgcPersistent> cppgc_persistent_new(RustResource* resource);
RustResource* cppgc_persistent_get(const CppgcPersistent& persistent);
kj::Own<CppgcWeakPersistent> cppgc_weak_persistent_new(RustResource* resource);
RustResource* cppgc_weak_persistent_get(const CppgcWeakPersistent& persistent);
kj::Own<CppgcMember> cppgc_member_new(RustResource* resource);
RustResource* cppgc_member_get(const CppgcMember& member);
void cppgc_member_set(CppgcMember& member, RustResource* resource);
kj::Own<CppgcWeakMember> cppgc_weak_member_new(RustResource* resource);
RustResource* cppgc_weak_member_get(const CppgcWeakMember& member);
void cppgc_weak_member_set(CppgcWeakMember& member, RustResource* resource);

}  // namespace workerd::rust::jsg
