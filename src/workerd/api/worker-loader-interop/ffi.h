// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// The shims through which the Rust Worker Loader (api/worker-loader-api) builds a
// `DynamicWorkerSource`, loads it with `IoChannelFactory::loadIsolate()`, and gets entrypoints
// from the `WorkerStubChannel` it returns (ffi.rs). They are C++ API code, which uses C++ JSG as
// api/worker-loader.c++ does; see src/rust/jsg/README.md, "Safety rules", rule 3.
//
// Every function that takes an isolate takes the isolate of the caller's lock, locked by this
// thread with a context entered and a HandleScope open. It runs its C++ in jsg_cpp::callCpp() and
// returns false, with an exception pending, if it threw. A shim that takes an `ioContextId` finds
// the IoContext with io_context::currentWithId(), so it fails unless that IoContext is current.

#include <workerd/io/compatibility-date.h>
#include <workerd/io/frankenvalue.h>
#include <workerd/io/io-channels.h>
#include <workerd/io/io-own.h>
#include <workerd/io/worker-source.h>
#include <workerd/rust/jsg-cpp/jsg-cpp.h>

#include <rust/cxx.h>
#include <v8-isolate.h>
#include <v8-wasm.h>

#include <cstdint>

namespace workerd::api::worker_loader_interop {

namespace jsg_cpp = ::workerd::rust::jsg_cpp;

using SubrequestChannel = IoChannelFactory::SubrequestChannel;
using CompiledWasmModule = v8::CompiledWasmModule;

// Defined by the bridge (ffi.rs).
struct CodeCallback;
struct Limits;

// What a source's pointers point into: buffers that Rust made and C++ now owns, each a boxed
// `::rust::String` or `::rust::Vec`, so that neither the buffer nor the object holding it moves
// as the vectors grow. Text ends in a NUL that Rust appended, which is not part of the text, so
// that it can be viewed as a `kj::StringPtr`.
//
// A loaded Worker may be evicted and reloaded from the same source any number of times, and the
// content may be destroyed on another thread, so its refcount is atomic. Destroying a
// `::rust::String` or `::rust::Vec` frees Rust memory, which any thread may do.
struct SourceContent final: public kj::AtomicRefcounted {
  ::rust::String mainModule;
  kj::Vector<kj::Own<::rust::String>> texts;
  kj::Vector<kj::Own<::rust::Vec<uint8_t>>> bytes;
  kj::Own<CompatibilityFlags::Reader> compatFlags;
};

// A `DynamicWorkerSource` under construction, built from what Rust gives it.
class SourceBuilder {
 public:
  SourceBuilder(::rust::String mainModule);

  // `kind` is a `ModuleKind` of the bridge: a module whose body is text.
  void addText(uint8_t kind, ::rust::String name, ::rust::String body);
  // `kind` is `ModuleKind::Data` or `ModuleKind::Wasm`: a module whose body is bytes.
  void addBytes(uint8_t kind, ::rust::String name, ::rust::Vec<uint8_t> body);
  // A WASM module that shares already-compiled code. `esmBuiltins` and `sourceUrl` are the
  // isolate's settings it was compiled with.
  void addCompiledWasm(
      ::rust::String name, kj::Own<CompiledWasmModule> module, bool esmBuiltins, bool sourceUrl);

  // Compiles the code's compatibility date and flags with compileCompatibilityFlags(), as
  // worker-loader.c++ does, so both loaders get the same compiler. Throws the first error, as
  // worker-loader.c++ does.
  void compileCompatFlags(jsg::Lock& js,
      kj::StringPtr compatDate,
      kj::ArrayPtr<const kj::String> flags,
      bool allowExperimental,
      CompatibilityDateValidation dateValidation);
  void setLimits(const Limits& limits);
  void setEnv(kj::Own<Frankenvalue> env);
  void setGlobalOutbound(kj::Rc<SubrequestChannel> channel);
  void addTail(kj::Rc<SubrequestChannel> channel);
  void addStreamingTail(kj::Rc<SubrequestChannel> channel);

  // Builds the source. `ownContent` holds a reference to the content.
  DynamicWorkerSource build();

  // A new reference to the content, for each clone of the source that `build()` returns.
  kj::Own<SourceContent> addRefContent() {
    return kj::atomicAddRef(*content);
  }

 private:
  // Moves `text` (`bytes`) into the content, and returns a view of it.
  kj::StringPtr keepText(::rust::String text);
  kj::ArrayPtr<const byte> keepBytes(::rust::Vec<uint8_t> bytes);

  kj::Own<SourceContent> content;
  kj::Vector<WorkerSource::Module> modules;
  kj::Maybe<ResourceLimits> limits;
  Frankenvalue env;
  kj::Maybe<kj::Rc<SubrequestChannel>> globalOutbound;
  kj::Vector<kj::Rc<SubrequestChannel>> tails;
  kj::Vector<kj::Rc<SubrequestChannel>> streamingTails;
};

// A `WorkerStub`'s channel, owned by the IoContext that loaded it, as C++ `WorkerStub` holds it.
// The `IoOwn` may be destroyed on any thread: if it is not the IoContext's, the deletion is queued
// for the IoContext. It is dereferenced only by the shims below, which check that its IoContext is
// current.
struct WorkerStubHandle {
  IoOwn<WorkerStubChannel> channel;
};

kj::Own<SourceBuilder> source_builder_new(::rust::String mainModule);
void source_builder_add_text(
    SourceBuilder& builder, uint8_t kind, ::rust::String name, ::rust::String body);
void source_builder_add_bytes(
    SourceBuilder& builder, uint8_t kind, ::rust::String name, ::rust::Vec<uint8_t> body);
void source_builder_set_limits(SourceBuilder& builder, const Limits& limits);
void source_builder_set_env(SourceBuilder& builder, kj::Own<Frankenvalue> env);
void source_builder_set_global_outbound(SourceBuilder& builder, kj::Rc<SubrequestChannel> channel);
void source_builder_add_tail(SourceBuilder& builder, kj::Rc<SubrequestChannel> channel);
void source_builder_add_streaming_tail(SourceBuilder& builder, kj::Rc<SubrequestChannel> channel);

// builder.addCompiledWasm(), with the settings of the caller's isolate.
bool source_builder_add_compiled_wasm(v8::Isolate* isolate,
    SourceBuilder& builder,
    ::rust::String name,
    kj::Own<CompiledWasmModule> module);

// builder.compileCompatFlags().
bool source_builder_compile_compat_flags(v8::Isolate* isolate,
    SourceBuilder& builder,
    ::rust::Str compatDate,
    ::rust::Slice<const ::rust::String> flags,
    bool allowExperimental,
    CompatibilityDateValidation dateValidation);

// Has the loaded Worker inherit the calling Worker's global outbound:
// getSubrequestChannel(IoContext::NULL_CLIENT_CHANNEL).
bool source_builder_inherit_global_outbound(
    v8::Isolate* isolate, SourceBuilder& builder, uint64_t ioContextId);

// WorkerLoader.load(): loadIsolate(channel, kj::none, ...) with a callback that clones the source.
bool worker_loader_load(v8::Isolate* isolate,
    uint64_t ioContextId,
    uint32_t channel,
    kj::Own<SourceBuilder> builder,
    kj::Maybe<kj::Own<WorkerStubHandle>>& result);

// WorkerLoader.get(): loadIsolate(channel, name, ...) with a callback that calls `callback` for
// each source.
bool worker_loader_get(v8::Isolate* isolate,
    uint64_t ioContextId,
    uint32_t channel,
    bool hasName,
    ::rust::Str name,
    ::rust::Box<CodeCallback> callback,
    kj::Maybe<kj::Own<WorkerStubHandle>>& result);

// js.alloc<Fetcher>(ioctx.addObject(stub.channel->getEntrypoint(name, props, limits))).
bool worker_stub_get_entrypoint(v8::Isolate* isolate,
    const WorkerStubHandle& stub,
    uint64_t ioContextId,
    bool hasName,
    ::rust::Str name,
    kj::Own<Frankenvalue> props,
    const Limits& limits,
    kj::Maybe<kj::Own<jsg_cpp::CppRef>>& result);

// The same with getActorClass(), for a DurableObjectClass.
bool worker_stub_get_actor_class(v8::Isolate* isolate,
    const WorkerStubHandle& stub,
    uint64_t ioContextId,
    bool hasName,
    ::rust::Str name,
    kj::Own<Frankenvalue> props,
    const Limits& limits,
    kj::Maybe<kj::Own<jsg_cpp::CppRef>>& result);

}  // namespace workerd::api::worker_loader_interop
