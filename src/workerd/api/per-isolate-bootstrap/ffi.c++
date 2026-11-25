// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "ffi.h"

#include <workerd/api/per-isolate-bootstrap/ffi.rs.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/ser.h>
#include <workerd/jsg/setup.h>
#include <workerd/rust/jsg/ffi-inl.h>

#if KJ_HAS_COMPILER_FEATURE(address_sanitizer) || defined(__SANITIZE_ADDRESS__)
#include <sanitizer/lsan_interface.h>
#endif

namespace workerd::api::per_isolate_bootstrap {

namespace rust_jsg = ::workerd::rust::jsg;

namespace {

// What the BOOTSTRAP_STATE slot of a context holds when the Rust bootstrap ran in it: the Rust
// state, behind the interface through which the deserializer finds the bootstrap's classes.
class RustBootstrapModules final: public jsg::BootstrapModules {
 public:
  explicit RustBootstrapModules(::rust::Box<State> state)
      : jsg::BootstrapModules(jsg::BootstrapLoader::RUST),
        state(kj::mv(state)) {}

  kj::Maybe<jsg::JsValue> tryGetLoadedExports(jsg::Lock& js, kj::StringPtr specifier) override {
    auto result = rust_jsg::maybe_local_from_ffi<v8::Value>(loaded_exports(js.v8Isolate, *state,
        ::rust::Slice<const uint8_t>(specifier.asBytes().begin(), specifier.size())));
    v8::Local<v8::Value> value;
    if (result.ToLocal(&value)) {
      return jsg::JsValue(value);
    }
    return kj::none;
  }

  const State& getState() const {
    return *state;
  }

 private:
  ::rust::Box<State> state;
};

kj::Maybe<RustBootstrapModules&> tryGetModules(v8::Local<v8::Context> context) {
  KJ_IF_SOME(modules,
      jsg::getAlignedPointerFromEmbedderData<jsg::BootstrapModules>(
          context, jsg::ContextPointerSlot::BOOTSTRAP_STATE)) {
    // The C++ loader's state, or a test's, is not ours.
    if (modules.getLoader() == jsg::BootstrapLoader::RUST) {
      return kj::downcast<RustBootstrapModules>(modules);
    }
  }
  return kj::none;
}

}  // namespace

::rust::Vec<::rust::String> bootstrap_function_params() {
  ::rust::Vec<::rust::String> names;
  for (const auto& name: *jsg::BOOTSTRAP_FUNCTION_PARAMS) {
    names.push_back(::rust::String(name.begin(), name.size()));
  }
  return names;
}

::rust::Slice<const uint8_t> script_source(const Script& script) {
  auto src = script.getSrc();
  return {src.begin(), src.size()};
}

::rust::Slice<const uint8_t> script_compile_cache(const Script& script) {
  auto cache = script.getCompileCache();
  if (cache.size() == 0 || cache.begin() == nullptr) {
    return {};
  }
  return {cache.begin(), cache.size()};
}

const State* install_state(Isolate* isolate, ::rust::Box<State> state) {
  auto context = isolate->GetCurrentContext();
  KJ_REQUIRE(jsg::getAlignedPointerFromEmbedderData<jsg::BootstrapModules>(
                 context, jsg::ContextPointerSlot::BOOTSTRAP_STATE) == kj::none,
      "the context already has bootstrap state");
  // The slot holds a raw pointer; cleanup() frees it.
  auto* modules = new RustBootstrapModules(kj::mv(state));
#if KJ_HAS_COMPILER_FEATURE(address_sanitizer) || defined(__SANITIZE_ADDRESS__)
  // LSAN can't trace the pointer through V8's embedder data.
  __lsan_ignore_object(modules);
#endif
  jsg::setAlignedPointerInEmbedderData(context, jsg::ContextPointerSlot::BOOTSTRAP_STATE,
      static_cast<jsg::BootstrapModules*>(modules));
  return &modules->getState();
}

const State* current_state(Isolate* isolate) {
  KJ_IF_SOME(modules, tryGetModules(isolate->GetCurrentContext())) {
    return &modules.getState();
  }
  return nullptr;
}

bool snapshot_state_index(Isolate* isolate, size_t& index) {
  KJ_IF_SOME(pinned, jsg::IsolateBase::from(isolate).finalizedSnapshotArtifact().bootstrapState) {
    // The C++ loader restores what it pinned; see chooseLoader() in api/per-isolate-bootstrap.c++.
    if (pinned.loader == jsg::BootstrapLoader::RUST) {
      index = pinned.index;
      return true;
    }
  }
  return false;
}

void set_snapshot_state_index(Isolate* isolate, size_t index) {
  jsg::IsolateBase::from(isolate).mutableSnapshotArtifact().bootstrapState =
      jsg::SnapshotArtifact::PinnedBootstrapState{
        .index = index,
        .loader = jsg::BootstrapLoader::RUST,
      };
}

const State& state_of(const jsg::BootstrapModules& modules) {
  KJ_REQUIRE(modules.getLoader() == jsg::BootstrapLoader::RUST);
  return kj::downcast<const RustBootstrapModules>(modules).getState();
}

void cleanup(v8::Local<v8::Context> context) {
  KJ_IF_SOME(modules, tryGetModules(context)) {
    delete &modules;
    jsg::setAlignedPointerInEmbedderData(
        context, jsg::ContextPointerSlot::BOOTSTRAP_STATE, nullptr);
  }
}

}  // namespace workerd::api::per_isolate_bootstrap
