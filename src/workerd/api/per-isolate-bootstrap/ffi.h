// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// The C++ side of the Rust per-isolate bootstrap (lib.rs). The bridge (ffi.rs) declares these.
//
// The loader that api/per-isolate-bootstrap.c++ calls under the RUST_PER_ISOLATE_BOOTSTRAP
// autogate lives in Rust. Two things stay in C++:
// - ScriptHost, implemented by api/per-isolate-bootstrap.c++, which gives Rust the scripts of the
//   compiled-in bundle and of the embedder's, and the natives of `utils` that forward to the C++
//   API layer;
// - the context's BOOTSTRAP_STATE slot, which holds the Rust state behind a
//   jsg::BootstrapModules for the deserializer, and the isolate's snapshot artifact, which
//   records where a zygote pinned the state (ffi.c++).
//
// Every function that takes an isolate takes the isolate of the caller's lock, locked by this
// thread with a context entered and a HandleScope open, and handles in that scope.

#include <workerd/jsg/modules.capnp.h>
#include <workerd/rust/jsg/ffi.h>

#include <rust/cxx.h>

#include <cstdint>

namespace workerd::jsg {
class BootstrapModules;
}  // namespace workerd::jsg

namespace workerd::api::per_isolate_bootstrap {

using Isolate = ::workerd::rust::jsg::Isolate;
using Local = ::workerd::rust::jsg::Local;
using MaybeLocal = ::workerd::rust::jsg::MaybeLocal;

// A script of a bootstrap bundle, by a name in this namespace, which the bridge declares it by.
using Script = ::workerd::jsg::Module::Reader;

// The Rust state of one context's bootstrap (lib.rs).
struct State;

// The scripts the bootstrap of a context runs: workerd's, from the compiled-in bundle, and the
// embedder's, if any. Implemented by api/per-isolate-bootstrap.c++, owned by the context's
// State, and so alive as long as the context's bootstrap is.
class ScriptHost {
 public:
  virtual ~ScriptHost() noexcept(false) = default;

  // Whether `specifier` names a script in the embedder's namespace, whether or not the embedder's
  // bundle has it. Always false without an embedder.
  virtual bool in_embedder_namespace(::rust::Str specifier) const = 0;

  // The script `specifier` of workerd's bundle, or of the embedder's, or null if that bundle has
  // none. The result lives as long as this host.
  virtual const Script* find_workerd_script(::rust::Str specifier) const = 0;
  virtual const Script* find_embedder_script(::rust::Str specifier) const = 0;

  // The specifier of the embedder's entry point, or empty without an embedder.
  virtual ::rust::Str embedder_entry_point() const = 0;

  // The source of `script` from the jsg::ModuleSource of the isolate, or empty without one, in
  // which case V8 reads script_source(). Doesn't run JavaScript.
  virtual MaybeLocal module_source(Isolate* isolate, const Script& script) const = 0;

  // A new object holding the natives of `utils` that forward to the C++ API layer, to which the
  // Rust bootstrap adds its own. Empty with an exception pending if making one failed.
  virtual MaybeLocal api_natives(Isolate* isolate) const = 0;
};

// jsg::BOOTSTRAP_FUNCTION_PARAMS: the pseudo-globals, in the order of the parameters of the
// function each script is compiled as.
::rust::Vec<::rust::String> bootstrap_function_params();

// The source of `script`, in the bundle's static bytes: workerd's bundle is compiled in, and an
// embedder's is a compile-time constant (see EmbedderBootstrap).
::rust::Slice<const uint8_t> script_source(const Script& script);

// The compile cache that the build made for `script`, or empty.
::rust::Slice<const uint8_t> script_compile_cache(const Script& script);

// Makes `state` the bootstrap state of the current context: stores it, behind the
// jsg::BootstrapModules through which the deserializer finds the bootstrap's classes, in the
// context's BOOTSTRAP_STATE slot, which must be empty. Returns the state, which lives until
// cleanup() frees it.
const State* install_state(Isolate* isolate, ::rust::Box<State> state);

// The bootstrap state of the current context, or null.
const State* current_state(Isolate* isolate);

// Where the zygote pinned the bootstrap state into the snapshot the isolate starts from, as
// jsg::SnapshotArtifact::bootstrapStateIndex. False if it pinned none.
bool snapshot_state_index(Isolate* isolate, size_t& index);

// Records where a zygote pinned the bootstrap state into its snapshot, as the Rust loader's.
void set_snapshot_state_index(Isolate* isolate, size_t index);

// Not part of the bridge: the Rust state behind `modules`, which the Rust loader made
// (getLoader() is BootstrapLoader::RUST).
const State& state_of(const jsg::BootstrapModules& modules);

// Frees the bootstrap state of `context`, if it has any, and clears its slot.
void cleanup(v8::Local<v8::Context> context);

}  // namespace workerd::api::per_isolate_bootstrap
