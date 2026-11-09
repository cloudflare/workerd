// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/api/actor-state.h>
#include <workerd/api/actor.h>
#include <workerd/api/analytics-engine.h>
#include <workerd/api/base64.h>
#include <workerd/api/basics.h>
#include <workerd/api/blob.h>
#include <workerd/api/cache.h>
#include <workerd/api/capnp.h>
#include <workerd/api/commonjs.h>
#include <workerd/api/container.h>
#include <workerd/api/crypto/crypto.h>
#include <workerd/api/encoding.h>
#include <workerd/api/events.h>
#include <workerd/api/eventsource.h>
#include <workerd/api/export-loopback.h>
#include <workerd/api/filesystem.h>
#include <workerd/api/form-data.h>
#include <workerd/api/global-scope.h>
#include <workerd/api/html-rewriter.h>
#include <workerd/api/http.h>
#include <workerd/api/hyperdrive.h>
#include <workerd/api/kv.h>
#include <workerd/api/memory-cache.h>
#include <workerd/api/messagechannel.h>
#include <workerd/api/modules.h>
#include <workerd/api/node/node.h>
#include <workerd/api/performance.h>
#include <workerd/api/pyodide/pyodide.h>
#include <workerd/api/queue.h>
#include <workerd/api/r2.h>
#include <workerd/api/rtti.h>
#include <workerd/api/scheduled.h>
#include <workerd/api/snapshot.h>
#include <workerd/api/sockets.h>
#include <workerd/api/sql.h>
#include <workerd/api/streams.h>
#include <workerd/api/sync-kv.h>
#include <workerd/api/trace.h>
#include <workerd/api/tracing.h>
#include <workerd/api/unsafe.h>
#include <workerd/api/url-standard.h>
#include <workerd/api/url.h>
#include <workerd/api/urlpattern-standard.h>
#include <workerd/api/urlpattern.h>
#include <workerd/api/web-socket.h>
#include <workerd/api/worker-loader.h>
#include <workerd/api/worker-rpc.h>
#include <workerd/api/workers-module.h>
#include <workerd/api/wrapped-binding.h>
#include <workerd/io/compatibility-date.h>
#include <workerd/io/promise-wrapper.h>
#include <workerd/io/worker.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/setup.h>
#include <workerd/server/workerd-debug-port-client.h>

namespace workerd::server {

// Declares the listing of host object types and structs that the jsg
// automatic type mapping will understand. Each of the various
// NNNN_ISOLATE_TYPES macros are defined in different header files
// (e.g. GLOBAL_SCOPE_ISOLATE_TYPES is defined in api/global-scope.h).
//
// Global scope types are defined first just by convention, the rest
// of the list is in alphabetical order for easier readability (the
// actual order of the items is unimportant), followed by additional
// types defined in worker.c++ or as part of jsg.
//
// When adding a new NNNN_ISOLATE_TYPES macro, remember to add it to
// src/workerd/api/rtti.c++ too (and tools/api-encoder.c++ for the
// time being), so it gets included in the TypeScript types.
// clang-format off
#define JSG_WORKERD_ISOLATE_TYPES                       \
  EW_GLOBAL_SCOPE_ISOLATE_TYPES,                        \
  EW_ACTOR_ISOLATE_TYPES,                               \
  EW_ACTOR_STATE_ISOLATE_TYPES,                         \
  EW_ANALYTICS_ENGINE_ISOLATE_TYPES,                    \
  EW_BASE64_ISOLATE_TYPES,                              \
  EW_BASICS_ISOLATE_TYPES,                              \
  EW_BLOB_ISOLATE_TYPES,                                \
  EW_CACHE_ISOLATE_TYPES,                               \
  EW_CAPNP_TYPES,                                       \
  EW_CONTAINER_ISOLATE_TYPES,                           \
  EW_CJS_ISOLATE_TYPES,                                 \
  EW_CRYPTO_ISOLATE_TYPES,                              \
  EW_ENCODING_ISOLATE_TYPES,                            \
  EW_EVENTS_ISOLATE_TYPES,                              \
  EW_FORMDATA_ISOLATE_TYPES,                            \
  EW_HTML_REWRITER_ISOLATE_TYPES,                       \
  EW_HTTP_ISOLATE_TYPES,                                \
  EW_SOCKETS_ISOLATE_TYPES,                             \
  EW_KV_ISOLATE_TYPES,                                  \
  EW_PYODIDE_ISOLATE_TYPES,                             \
  EW_QUEUE_ISOLATE_TYPES,                               \
  EW_R2_PUBLIC_BETA_ISOLATE_TYPES,                      \
  EW_WORKER_RPC_ISOLATE_TYPES,                          \
  EW_SCHEDULED_ISOLATE_TYPES,                           \
  EW_STREAMS_ISOLATE_TYPES,                             \
  EW_TRACE_ISOLATE_TYPES,                               \
  EW_UNSAFE_ISOLATE_TYPES,                              \
  EW_MEMORY_CACHE_ISOLATE_TYPES,                        \
  EW_SNAPSHOT_ISOLATE_TYPES,                            \
  EW_URL_ISOLATE_TYPES,                                 \
  EW_URL_STANDARD_ISOLATE_TYPES,                        \
  EW_URLPATTERN_ISOLATE_TYPES,                          \
  EW_URLPATTERN_STANDARD_ISOLATE_TYPES,                 \
  EW_WEB_FILESYSTEM_ISOLATE_TYPE,                       \
  EW_FILESYSTEM_ISOLATE_TYPES,                          \
  EW_WEBSOCKET_ISOLATE_TYPES,                           \
  EW_SQL_ISOLATE_TYPES,                                 \
  EW_SYNC_KV_ISOLATE_TYPES,                             \
  EW_NODE_ISOLATE_TYPES,                                \
  EW_RTTI_ISOLATE_TYPES,                                \
  EW_HYPERDRIVE_ISOLATE_TYPES,                          \
  EW_EVENTSOURCE_ISOLATE_TYPES,                         \
  EW_WORKER_LOADER_ISOLATE_TYPES,                       \
  EW_MESSAGECHANNEL_ISOLATE_TYPES,                      \
  EW_WORKERS_MODULE_ISOLATE_TYPES,                      \
  EW_EXPORT_LOOPBACK_ISOLATE_TYPES,                     \
  EW_PERFORMANCE_ISOLATE_TYPES,                         \
  EW_TRACING_ISOLATE_TYPES,                             \
  EW_WORKERD_DEBUG_PORT_CLIENT_ISOLATE_TYPES,           \
  EW_WRAPPED_BINDING_ISOLATE_TYPES,                     \
  workerd::api::EnvModule,                              \
  jsg::TypeWrapperExtension<PromiseWrapper>,            \
  jsg::InjectConfiguration<CompatibilityFlags::Reader>, \
  Worker::Api::ErrorInterface
// clang-format on

#ifdef WORKERD_API_UNSPLIT_TEMPLATES
// On Windows, compiling the split translation units in parallel exhausted a CI runner's memory, so
// Windows builds compile all of the isolate's template builders in workerd-api.c++.
JSG_DECLARE_ISOLATE_TYPE(JsgWorkerdIsolate, JSG_WORKERD_ISOLATE_TYPES);
#else
// JsgWorkerdIsolate's template builders, most of its code, are split across 8 translation units,
// workerd-api-templates-<n>.c++, which compile, and are optimized at link time, in parallel.
JSG_DECLARE_SPLIT_ISOLATE_TYPE(JsgWorkerdIsolate, 8, JSG_WORKERD_ISOLATE_TYPES);
#endif

}  // namespace workerd::server
