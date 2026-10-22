// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/io/compatibility-date.capnp.h>
#include <workerd/jsg/jsg.h>

namespace workerd {

// Runs the per-isolate bootstrap scripts from the compiled-in PER_ISOLATE_BUNDLE.
//
// Must be called within a V8 context scope, after JSG global setup is complete
// but before any user code evaluation.
//
// On a context built from scratch this runs the bootstrap scripts from the
// compiled-in PER_ISOLATE_BUNDLE: the entry point script named "main" is loaded
// via a minimal synchronous require() mechanism. The require() function and
// compatFlags object are injected into each script's scope via V8's
// CompileFunction context extensions, not via globalThis. Scripts can install
// properties on globalThis; those become visible to user code.
//
// On a context deserialized from a startup snapshot the scripts' effects are
// already in the context, and running them again would install a second set of
// classes beside the one the zygote's top-level code captured. Instead the
// bootstrap state is rebuilt from the state the zygote parked in the context's
// BOOTSTRAP_SNAPSHOT_STATE data slot and that slot is cleared.
//
// The bootstrap state (require cache, compat flags) is heap-allocated and stored
// in the context's BOOTSTRAP_STATE embedder data slot, so that require() remains
// usable for lazy evaluation after runPerIsolateBootstrap() returns.
//
// Throws kj::Exception on failure (compilation errors, missing scripts, cycles).
// This is fatal for isolate creation since these are runtime-owned scripts.
void runPerIsolateBootstrap(jsg::Lock& js, CompatibilityFlags::Reader flags);

// Cleans up the per-isolate bootstrap state stored in the context's embedder data.
// Must be called before the context is destroyed (e.g., from disposeContext()).
void cleanupPerIsolateBootstrap(jsg::Lock& js, v8::Local<v8::Context> context);

// Zygote only (preparing a startup snapshot): parks the bootstrap's JS-held state
// (require(), primordials, the compatFlags/autogates/utils objects, the require()
// cache) in the context's BOOTSTRAP_SNAPSHOT_STATE data slot, where the snapshot
// carries it, then releases the C++ bootstrap state exactly as
// cleanupPerIsolateBootstrap() does.
void stashPerIsolateBootstrapForSnapshot(jsg::Lock& js, v8::Local<v8::Context> context);

// Returns a Maybe<JsValue> with the named bootstrap module export if it exists, or
// kj::none if it does not. The module must have already been instantiated/required
// as part of the per-isolate bootstrap process. This is used by the worker runtime
// to access the bootstrap modules from C++ code. Typically, the export should be
// an object, but there's always the possibility it could be any JS value type.
// The caller is responsible for checking the type.
kj::Maybe<jsg::JsValue> tryGetBootstrapExport(jsg::Lock& js, kj::StringPtr specifier);

}  // namespace workerd
