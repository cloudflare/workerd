// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/io/compatibility-date.capnp.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/modules.capnp.h>

#include <kj/map.h>

namespace workerd {

// Bootstrap scripts contributed by the embedder: a bundle built with wd_ts_script_bundle() and an
// `embedder_prefix`, so that every script in it is named under that prefix (e.g.
// "embedder/main"). Its entry point, `<prefix>main`, runs after workerd's own `main`, as part of
// the same bootstrap: the embedder's scripts share the require() cache and the pseudo-globals,
// and can require() workerd's bootstrap scripts. workerd's scripts cannot require() the
// embedder's.
//
// The bundle is a compile-time constant, so construct one instance per process and return it from
// Worker::Api::getEmbedderBootstrap(). The constructor checks the bundle, throwing if a script is
// named outside the prefix, if the prefix overlaps workerd's script names, or if the entry point
// is missing.
class EmbedderBootstrap {
 public:
  // `prefix` must be non-empty and end with '/'.
  EmbedderBootstrap(jsg::Bundle::Reader bundle, kj::StringPtr prefix);
  KJ_DISALLOW_COPY_AND_MOVE(EmbedderBootstrap);

  kj::StringPtr getPrefix() const {
    return prefix;
  }
  kj::StringPtr getEntryPoint() const {
    return entryPoint;
  }

  // Whether `specifier`, after normalization, names a script in the embedder's namespace, whether
  // or not the bundle has it.
  bool isInNamespace(kj::StringPtr specifier) const {
    return specifier.startsWith(prefix);
  }

  kj::Maybe<jsg::Module::Reader> tryGetScript(kj::StringPtr specifier) const;

 private:
  kj::String prefix;
  kj::String entryPoint;
  // Keys and values point into the bundle.
  kj::HashMap<kj::StringPtr, jsg::Module::Reader> scripts;
};

// Runs the per-isolate bootstrap scripts from the compiled-in PER_ISOLATE_BUNDLE, followed by the
// embedder's, if any.
//
// Must be called within a V8 context scope, after JSG global setup is complete
// but before any user code evaluation. The entry point script named "main" is
// loaded via a minimal synchronous require() mechanism.
//
// The require() function and compatFlags object are passed to each script as
// parameters of the function V8's CompileFunction wraps it in, not via globalThis. Scripts
// can install properties on globalThis; those become visible to user code.
//
// The bootstrap state (require cache, compat flags) is heap-allocated and stored
// in the context's BOOTSTRAP_STATE embedder data slot, so that require() remains
// usable for lazy evaluation after runPerIsolateBootstrap() returns. The state
// refers to `embedder`, which must outlive the context.
//
// Throws kj::Exception on failure (compilation errors, missing scripts, cycles).
// This is fatal for isolate creation since these are runtime-owned scripts.
void runPerIsolateBootstrap(jsg::Lock& js,
    CompatibilityFlags::Reader flags,
    kj::Maybe<const EmbedderBootstrap&> embedder = kj::none);

// Cleans up the per-isolate bootstrap state stored in the context's embedder data.
// Must be called before the context is destroyed (e.g., from disposeContext()).
void cleanupPerIsolateBootstrap(jsg::Lock& js, v8::Local<v8::Context> context);

// Returns a Maybe<JsValue> with the named bootstrap module export if it exists, or
// kj::none if it does not. The module must have already been instantiated/required
// as part of the per-isolate bootstrap process. This is used by the worker runtime
// to access the bootstrap modules from C++ code. Typically, the export should be
// an object, but there's always the possibility it could be any JS value type.
// The caller is responsible for checking the type. Embedder scripts are looked up
// by their full prefixed specifier.
kj::Maybe<jsg::JsValue> tryGetBootstrapExport(jsg::Lock& js, kj::StringPtr specifier);

// Returns the function property `name` of the named bootstrap module export, which
// must be a frozen object, or kj::none if the module was not required or the property
// is not a function. The function is cached per context, so later lookups of the same
// name skip the property get.
kj::Maybe<jsg::JsFunction> tryGetBootstrapFunction(
    jsg::Lock& js, kj::StringPtr specifier, kj::StringPtr name);

}  // namespace workerd
