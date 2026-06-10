// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/io/compatibility-date.capnp.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/modules.capnp.h>

namespace workerd {

// Runs the per-isolate bootstrap scripts from the given bundle.
//
// Must be called within a V8 context scope, after JSG global setup is complete
// but before any user code evaluation. The entry point script named "main" is
// loaded via a minimal synchronous require() mechanism.
//
// The require() function and compatFlags object are injected into each script's
// scope via V8's CompileFunction context extensions, not via globalThis. Scripts
// can install properties on globalThis; those become visible to user code.
//
// The bootstrap state (require cache, compat flags) is heap-allocated and stored
// in the context's BOOTSTRAP_STATE embedder data slot, so that require() remains
// usable for lazy evaluation after runPerIsolateBootstrap() returns.
//
// Throws kj::Exception on failure (compilation errors, missing scripts, cycles).
// This is fatal for isolate creation since these are runtime-owned scripts.
void runPerIsolateBootstrap(
    jsg::Lock& js, jsg::Bundle::Reader bundle, CompatibilityFlags::Reader flags);

// Cleans up the per-isolate bootstrap state stored in the context's embedder data.
// Must be called before the context is destroyed (e.g., from disposeContext()).
void cleanupPerIsolateBootstrap(jsg::Lock& js, v8::Local<v8::Context> context);

}  // namespace workerd
