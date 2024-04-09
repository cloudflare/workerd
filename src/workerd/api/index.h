// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/api/actor.h>
#include <workerd/api/actor-state.h>
#include <workerd/api/analytics-engine.h>
#include <workerd/api/cache.h>
#include <workerd/api/crypto-impl.h>
#include <workerd/api/encoding.h>
#include <workerd/api/global-scope.h>
#include <workerd/api/html-rewriter.h>
#include <workerd/api/hyperdrive.h>
#include <workerd/api/kv.h>
#include <workerd/api/pyodide/pyodide.h>
#include <workerd/api/queue.h>
#include <workerd/api/scheduled.h>
#include <workerd/api/sockets.h>
#include <workerd/api/streams/standard.h>
#include <workerd/api/sql.h>
#include <workerd/api/r2.h>
#include <workerd/api/r2-admin.h>
#include <workerd/api/trace.h>
#include <workerd/api/unsafe.h>
#include <workerd/api/urlpattern.h>
#include <workerd/api/memory-cache.h>
#include <workerd/api/worker-rpc.h>
#include <workerd/api/node/node.h>

#ifdef WORKERD_EXPERIMENTAL_ENABLE_WEBGPU
#include <workerd/api/gpu/gpu.h>
#else
#define EW_WEBGPU_ISOLATE_TYPES
#endif

// Declares the listing of host object types and structs that the jsg
// automatic type mapping will understand. Each of the various
// NNNN_ISOLATE_TYPES macros are defined in different header files
// (e.g. GLOBAL_SCOPE_ISOLATE_TYPES is defined in api/global-scope.h).
//
// Global scope types are defined first just by convention, the rest
// of the list is in alphabetical order for easier readability (the
// actual order of the items is unimportant), followed by additional
// types defined in worker.c++ or as part of jsg.
#define EW_TYPE_GROUP_FOR_EACH(F)                                              \
  F("global-scope", EW_GLOBAL_SCOPE_ISOLATE_TYPES)                             \
  F("durable-objects", EW_ACTOR_ISOLATE_TYPES)                                 \
  F("durable-objects-state", EW_ACTOR_STATE_ISOLATE_TYPES)                     \
  F("analytics-engine", EW_ANALYTICS_ENGINE_ISOLATE_TYPES)                     \
  F("basics", EW_BASICS_ISOLATE_TYPES)                                         \
  F("blob", EW_BLOB_ISOLATE_TYPES)                                             \
  F("cache", EW_CACHE_ISOLATE_TYPES)                                           \
  F("crypto", EW_CRYPTO_ISOLATE_TYPES)                                         \
  F("encoding", EW_ENCODING_ISOLATE_TYPES)                                     \
  F("form-data", EW_FORMDATA_ISOLATE_TYPES)                                    \
  F("html-rewriter", EW_HTML_REWRITER_ISOLATE_TYPES)                           \
  F("http", EW_HTTP_ISOLATE_TYPES)                                             \
  F("sockets", EW_SOCKETS_ISOLATE_TYPES)                                       \
  F("kv", EW_KV_ISOLATE_TYPES)                                                 \
  F("pyodide", EW_PYODIDE_ISOLATE_TYPES)                                       \
  F("queue", EW_QUEUE_ISOLATE_TYPES)                                           \
  F("r2-admin", EW_R2_PUBLIC_BETA_ADMIN_ISOLATE_TYPES)                         \
  F("r2", EW_R2_PUBLIC_BETA_ISOLATE_TYPES)                                     \
  F("worker-rpc", EW_WORKER_RPC_ISOLATE_TYPES)                                 \
  F("scheduled", EW_SCHEDULED_ISOLATE_TYPES)                                   \
  F("streams", EW_STREAMS_ISOLATE_TYPES)                                       \
  F("trace", EW_TRACE_ISOLATE_TYPES)                                           \
  F("unsafe", EW_UNSAFE_ISOLATE_TYPES)                                         \
  F("memory-cache", EW_MEMORY_CACHE_ISOLATE_TYPES)                             \
  F("url", EW_URL_ISOLATE_TYPES)                                               \
  F("url-standard", EW_URL_STANDARD_ISOLATE_TYPES)                             \
  F("url-pattern", EW_URLPATTERN_ISOLATE_TYPES)                                \
  F("websocket", EW_WEBSOCKET_ISOLATE_TYPES)                                   \
  F("sql", EW_SQL_ISOLATE_TYPES)                                               \
  F("node", EW_NODE_ISOLATE_TYPES)                                             \
  F("hyperdrive", EW_HYPERDRIVE_ISOLATE_TYPES)                                 \
  F("webgpu", EW_WEBGPU_ISOLATE_TYPES)
// Intentionally omitting `EW_RTTI_ISOLATE_TYPES` as we can't compile those without the
// `cloudflare` and `node` bundles, but building those requires us to export type definitions from
// these types for type checking.

namespace workerd::api {

template <class Registry>
void registerModules(Registry& registry, auto featureFlags) {
  // Note we don't register module bundles here. These are registered in `index-bundles-rtti.h`.
  // This file declares types to include in RTTI passed to the TypeScript types generator for
  // building "internal" types. These "internal" types are used to type check module bundles.
  // Registering module bundles separately avoids a circular dependency on types.
  // See the `//types:types_internal` target for details.

  node::registerNodeJsCompatModules(registry, featureFlags);
  pyodide::registerPyodideModules(registry, featureFlags);
  registerUnsafeModules(registry, featureFlags);
  if (featureFlags.getUnsafeModule()) {
    registerUnsafeModule(registry);
  }
  registerSocketsModule(registry, featureFlags);
  registerRpcModules(registry, featureFlags);
}

}  // namespace workerd::api
