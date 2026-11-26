#pragma once

#include "crypto.h"
#include "diagnostics-channel.h"
#include "zlib-util.h"

#include <workerd/api/node/async-hooks.h>
#include <workerd/api/node/buffer.h>
#include <workerd/api/node/module.h>
#include <workerd/api/node/process.h>
#include <workerd/api/node/timers.h>
#include <workerd/api/node/util.h>
#include <workerd/io/io-context.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/modules-new.h>
#include <workerd/jsg/url.h>
#include <workerd/rust/api/lib.rs.h>
#include <workerd/rust/jsg/jsg.h>

#include <node/node.capnp.h>

namespace workerd::api::node {

#define NODEJS_MODULES(V)                                                                          \
  V(AsyncHooksModule, "node-internal:async_hooks")                                                 \
  V(BufferUtil, "node-internal:buffer")                                                            \
  V(CryptoImpl, "node-internal:crypto")                                                            \
  V(ModuleUtil, "node-internal:module")                                                            \
  V(ProcessModule, "node-internal:process")                                                        \
  V(UtilModule, "node-internal:util")                                                              \
  V(DiagnosticsChannelModule, "node-internal:diagnostics_channel")                                 \
  V(ZlibUtil, "node-internal:zlib")                                                                \
  V(TimersUtil, "node-internal:timers")

// Add to the NODEJS_MODULES_EXPERIMENTAL list any currently in-development
// node.js compat C++ modules that should be guarded by the experimental compat
// flag. Once they are ready to ship, move them up to the NODEJS_MODULES list.
#define NODEJS_MODULES_EXPERIMENTAL(V)

bool isNodeJsCompatEnabled(auto featureFlags) {
  return featureFlags.getNodeJsCompat() || featureFlags.getNodeJsCompatV2();
}

constexpr bool isNodeJsCompatFsModule(kj::StringPtr name) {
  return name == "node:fs"_kj;
}

constexpr bool isNodeHttpModule(kj::StringPtr name) {
  return name == "node:http"_kj || name == "node:_http_common"_kj ||
      name == "node:_http_outgoing"_kj || name == "node:_http_client"_kj ||
      name == "node:_http_incoming"_kj || name == "node:_http_agent"_kj || name == "node:https"_kj;
}

constexpr bool isNodeHttpServerModule(kj::StringPtr name) {
  return name == "node:_http_server"_kj;
}

constexpr bool isNodeOsModule(kj::StringPtr name) {
  return name == "node:os"_kj;
}

constexpr bool isNodeHttp2Module(kj::StringPtr name) {
  return name == "node:http2"_kj;
}

constexpr bool isNodeConsoleModule(kj::StringPtr name) {
  return name == "node:console"_kj;
}

template <class Registry>
void registerNodeJsCompatModules(
    Registry& registry, auto featureFlags, const jsg::ModuleSource* moduleSource = nullptr) {
#define V(T, N)                                                                                    \
  registry.template addBuiltinModule<T>(N, workerd::jsg::ModuleRegistry::Type::INTERNAL);

  NODEJS_MODULES(V)

  if (featureFlags.getWorkerdExperimental()) {
    NODEJS_MODULES_EXPERIMENTAL(V)
  }

#undef V

  bool nodeJsCompatEnabled = isNodeJsCompatEnabled(featureFlags);

  // Value-captured subset of compat flags consulted when gating NODE_BUNDLE modules so the
  // predicate can be stored lazily on ModuleRegistryImpl without retaining featureFlags.
  struct NodeJsBundlePredicate {
    bool nodeJsCompatEnabled;
    bool nodeJsAls;
    bool fs;
    bool http;
    bool httpServer;
    bool os;
    bool http2;
    bool console;
    bool vm;
    bool perfHooks;
    bool domain;
    bool childProcess;
    bool v8;
    bool tty;
    bool punycode;
    bool cluster;
    bool workerThreads;
    bool streamWrap;
    bool wasi;
    bool dgram;
    bool inspector;
    bool traceEvents;
    bool readline;
    bool repl;
    bool sqlite;

    bool operator()(jsg::Module::Reader module) const {
      if (!nodeJsCompatEnabled) {
        // If the `nodejs_compat` flag isn't enabled, only register internal modules (needed for
        // `console.log()` locally), plus `node:async_hooks` when `nodejs_als` is on.
        if (module.getType() == jsg::ModuleType::INTERNAL) return true;
        return nodeJsAls && module.getName() == "node:async_hooks"_kj;
      }
      auto name = module.getName();
      if (isNodeJsCompatFsModule(name)) return fs;
      if (isNodeHttpModule(name)) return http;
      if (isNodeHttpServerModule(name)) return httpServer;
      if (isNodeOsModule(name)) return os;
      if (isNodeHttp2Module(name)) return http2;
      if (isNodeConsoleModule(name)) return console;
      if (name == "node:vm"_kj) return vm;
      if (name == "node:perf_hooks"_kj) return perfHooks;
      if (name == "node:domain"_kj) return domain;
      if (name == "node:child_process"_kj) return childProcess;
      if (name == "node:v8"_kj) return v8;
      if (name == "node:tty"_kj) return tty;
      if (name == "node:punycode"_kj) return punycode;
      if (name == "node:cluster"_kj) return cluster;
      if (name == "node:worker_threads"_kj) return workerThreads;
      if (name == "node:_stream_wrap"_kj) return streamWrap;
      if (name == "node:wasi"_kj) return wasi;
      if (name == "node:dgram"_kj) return dgram;
      if (name == "node:inspector"_kj || name == "node:inspector/promises"_kj) return inspector;
      if (name == "node:trace_events"_kj) return traceEvents;
      if (name == "node:readline"_kj || name == "node:readline/promises"_kj) return readline;
      if (name == "node:repl"_kj) return repl;
      if (name == "node:sqlite"_kj) return sqlite;
      return true;
    }
  };

  NodeJsBundlePredicate predicate{
    .nodeJsCompatEnabled = nodeJsCompatEnabled,
    .nodeJsAls = featureFlags.getNodeJsAls(),
    .fs = featureFlags.getEnableNodeJsFsModule(),
    .http = featureFlags.getEnableNodejsHttpModules(),
    .httpServer = featureFlags.getEnableNodejsHttpServerModules(),
    .os = featureFlags.getEnableNodeJsOsModule(),
    .http2 = featureFlags.getEnableNodeJsHttp2Module(),
    .console = featureFlags.getEnableNodeJsConsoleModule(),
    .vm = featureFlags.getEnableNodeJsVmModule(),
    .perfHooks = featureFlags.getEnableNodeJsPerfHooksModule(),
    .domain = featureFlags.getEnableNodeJsDomainModule(),
    .childProcess = featureFlags.getEnableNodeJsChildProcessModule(),
    .v8 = featureFlags.getEnableNodeJsV8Module(),
    .tty = featureFlags.getEnableNodeJsTtyModule(),
    .punycode = featureFlags.getEnableNodeJsPunycodeModule(),
    .cluster = featureFlags.getEnableNodeJsClusterModule(),
    .workerThreads = featureFlags.getEnableNodeJsWorkerThreadsModule(),
    .streamWrap = featureFlags.getEnableNodeJsStreamWrapModule(),
    .wasi = featureFlags.getEnableNodeJsWasiModule(),
    .dgram = featureFlags.getEnableNodeJsDgramModule(),
    .inspector = featureFlags.getEnableNodeJsInspectorModule(),
    .traceEvents = featureFlags.getEnableNodeJsTraceEventsModule(),
    .readline = featureFlags.getEnableNodeJsReadlineModule(),
    .repl = featureFlags.getEnableNodeJsReplModule(),
    .sqlite = featureFlags.getEnableNodeJsSqliteModule(),
  };

  static const jsg::BuiltinBundleIndex* const nodeIndex = new jsg::BuiltinBundleIndex(NODE_BUNDLE);
  registry.addLazyBuiltinBundle(*nodeIndex, kj::none, moduleSource,
      kj::Function<bool(jsg::Module::Reader)>(kj::mv(predicate)));

  ::workerd::rust::jsg::RustModuleRegistry<Registry, SuppressIoContextScope> r(registry);
  ::workerd::rust::api::register_nodejs_modules(r);
}

template <class TypeWrapper>
kj::Own<jsg::modules::ModuleBundle> getInternalNodeJsCompatModuleBundle(
    auto featureFlags, const jsg::ModuleSource* moduleSource = nullptr) {
  jsg::modules::ModuleBundle::BuiltinBuilder builder(
      jsg::modules::ModuleBundle::BuiltinBuilder::Type::BUILTIN_ONLY);
#define V(M, N)                                                                                    \
  static const auto k##M##Specifier = N##_url;                                                     \
  builder.addObject<M, TypeWrapper>(k##M##Specifier);
  NODEJS_MODULES(V)
  if (featureFlags.getWorkerdExperimental()) {
    NODEJS_MODULES_EXPERIMENTAL(V)
  }
#undef V

  jsg::modules::ModuleBundle::getBuiltInBundleFromCapnp(builder, NODE_BUNDLE, moduleSource);

  // Register Rust-implemented Node.js modules that declare Internal
  // visibility. The adapter registers only the modules whose declared type
  // matches this internal-only builder; Builtin-typed modules are picked up
  // by the same registration functions run in
  // getExternalNodeJsCompatModuleBundle().
  {
    ::workerd::rust::jsg::RustBuiltinModuleAdapter<SuppressIoContextScope> adapter(builder);
    ::workerd::rust::api::register_nodejs_modules(adapter);
  }

  return builder.finish();
}

kj::Own<jsg::modules::ModuleBundle> getExternalNodeJsCompatModuleBundle(
    auto featureFlags, const jsg::ModuleSource* moduleSource = nullptr) {
  jsg::modules::ModuleBundle::BuiltinBuilder builder(
      jsg::modules::ModuleBundle::BuiltinBuilder::Type::BUILTIN);
  if (isNodeJsCompatEnabled(featureFlags)) {
    auto filter = [&](jsg::Module::Reader module) -> bool {
      if (isNodeJsCompatFsModule(module.getName())) {
        return featureFlags.getEnableNodeJsFsModule();
      }
      if (isNodeHttpModule(module.getName())) {
        return featureFlags.getEnableNodejsHttpModules();
      }
      if (isNodeHttpServerModule(module.getName())) {
        return featureFlags.getEnableNodejsHttpServerModules();
      }
      if (isNodeOsModule(module.getName())) {
        return featureFlags.getEnableNodeJsOsModule();
      }
      if (isNodeHttp2Module(module.getName())) {
        return featureFlags.getEnableNodeJsHttp2Module();
      }
      if (isNodeConsoleModule(module.getName())) {
        return featureFlags.getEnableNodeJsConsoleModule();
      }
      if (module.getName() == "node:vm") {
        return featureFlags.getEnableNodeJsVmModule();
      }
      if (module.getName() == "node:perf_hooks") {
        return featureFlags.getEnableNodeJsPerfHooksModule();
      }
      if (module.getName() == "node:domain") {
        return featureFlags.getEnableNodeJsDomainModule();
      }
      if (module.getName() == "node:child_process") {
        return featureFlags.getEnableNodeJsChildProcessModule();
      }
      if (module.getName() == "node:v8") {
        return featureFlags.getEnableNodeJsV8Module();
      }
      if (module.getName() == "node:tty") {
        return featureFlags.getEnableNodeJsTtyModule();
      }
      if (module.getName() == "node:punycode") {
        return featureFlags.getEnableNodeJsPunycodeModule();
      }
      if (module.getName() == "node:cluster") {
        return featureFlags.getEnableNodeJsClusterModule();
      }
      if (module.getName() == "node:worker_threads") {
        return featureFlags.getEnableNodeJsWorkerThreadsModule();
      }
      if (module.getName() == "node:_stream_wrap") {
        return featureFlags.getEnableNodeJsStreamWrapModule();
      }
      if (module.getName() == "node:wasi") {
        return featureFlags.getEnableNodeJsWasiModule();
      }
      if (module.getName() == "node:dgram") {
        return featureFlags.getEnableNodeJsDgramModule();
      }
      if (module.getName() == "node:inspector" || module.getName() == "node:inspector/promises") {
        return featureFlags.getEnableNodeJsInspectorModule();
      }
      if (module.getName() == "node:trace_events") {
        return featureFlags.getEnableNodeJsTraceEventsModule();
      }
      if (module.getName() == "node:readline" || module.getName() == "node:readline/promises") {
        return featureFlags.getEnableNodeJsReadlineModule();
      }
      if (module.getName() == "node:repl") {
        return featureFlags.getEnableNodeJsReplModule();
      }
      if (module.getName() == "node:sqlite") {
        return featureFlags.getEnableNodeJsSqliteModule();
      }
      return true;
    };
    jsg::modules::ModuleBundle::getBuiltInBundleFromCapnp(
        builder, NODE_BUNDLE, kj::mv(filter), moduleSource);
  } else if (featureFlags.getNodeJsAls()) {
    // The AsyncLocalStorage API can be enabled independently of the rest
    // of the nodejs_compat layer.
    jsg::Bundle::Reader reader = NODE_BUNDLE;
    for (auto module: reader.getModules()) {
      auto specifier = module.getName();
      if (specifier == "node:async_hooks") {
        KJ_DASSERT(module.getType() == jsg::ModuleType::BUILTIN);
        KJ_DASSERT(module.which() == workerd::jsg::Module::SRC);
        auto specifier = KJ_ASSERT_NONNULL(jsg::Url::tryParse(module.getName()));
        if (moduleSource == nullptr) {
          builder.addEsm(specifier, module.getSrc().asChars());
        } else {
          builder.addEsm(specifier, moduleSource->get(module.getSrc().asChars()));
        }
      }
    }
  }

  // Register Rust-implemented Node.js modules that declare Builtin
  // (user-importable) visibility. The registration functions run once per
  // bundle and the adapter registers only the modules whose declared type
  // matches the builder it wraps, so the same functions run here and in
  // getInternalNodeJsCompatModuleBundle(). Currently every Rust module is
  // Internal, so this adds nothing; it exists so that a future Builtin-typed
  // Rust module gets the same visibility under both module registries (the
  // legacy path registers Rust modules unconditionally with their declared
  // type).
  {
    ::workerd::rust::jsg::RustBuiltinModuleAdapter<SuppressIoContextScope> adapter(builder);
    ::workerd::rust::api::register_nodejs_modules(adapter);
  }
  return builder.finish();
}

#undef NODEJS_MODULES
}  // namespace workerd::api::node

#define EW_NODE_ISOLATE_TYPES                                                                      \
  EW_NODE_BUFFER_ISOLATE_TYPES, EW_NODE_CRYPTO_ISOLATE_TYPES,                                      \
      EW_NODE_DIAGNOSTICCHANNEL_ISOLATE_TYPES, EW_NODE_ASYNCHOOKS_ISOLATE_TYPES,                   \
      EW_NODE_UTIL_ISOLATE_TYPES, EW_NODE_PROCESS_ISOLATE_TYPES, EW_NODE_ZLIB_ISOLATE_TYPES,       \
      EW_NODE_MODULE_ISOLATE_TYPES, EW_NODE_TIMERS_ISOLATE_TYPES
