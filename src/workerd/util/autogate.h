// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

#include <workerd/util/strong-bool.h>

#include <capnp/blob.h>
#include <capnp/list.h>
#include <kj/common.h>
#include <kj/string.h>

#include <initializer_list>

namespace workerd::util {

// --------------------------------------------------------------------------------------
// Autogates enable gradual rollout of risky code changes independent of full
// binary releases. Unlike compatibility flags (see src/workerd/io/compatibility-date.capnp),
// autogates are not intended to be used for long-term feature gating. Instead, autogates
// are temporary gates that can be toggled on/off via internal tooling during rollout,
// then removed once the change is stable.
//
// Use an autogate when:
// - Rolling out a risky internal change gradually.
// - You need a kill switch during rollout because the change is risky.
//
// An autogate will never be used to gate a feature permanently. An autogate
// never becomes a compatibility flag.
//
// To add a new autogate:
//
// 1. Add the enum key to WORKERD_AUTOGATES.
//    Use the `SCREAMING_SNAKE_CASE` naming convention for the enum value.
//    The `kebab-case` string will be automatically derived from the enum value.
//    For example, `AutogateKey::MY_NEW_FEATURE` will be mapped to the string
//    `my-new-feature`. At runtime, the full autogate name will expand to
//    `workerd-autogate-my-new-feature`.
// 2. Guard your code with Autogate::isEnabled(AutogateKey::YOUR_GATE_KEY).
//
// Testing autogated code paths:
//
// 1. There is an automatic `@all-autogates` test variant that enables all gates.
// 2. In a C++ test file, you can enable specific gates by calling
//    Autogate::initAutogateNamesForTest() in the test's main() function.
//    You can disable using Autogate::deinitAutogate() in the test teardown.
// 3. In a wd-test test file, you can enable specific gates using the autogates
//    config option in the test's config file.
// 4. You can set the `WORKERD_ALL_AUTOGATES=1` environment variable to enable
//    all gates for a test run.
//
// Removing the gate after rollout:
//
// Once the change is verified to be stable, the autogate, and the old code path
// being guarded, should be removed.
//
// 1. Remove the enum value from WORKERD_AUTOGATES.
// 2. Remove the Autogate::isEnabled() checks, keeping only the new code path.
//    For example, if you have code like:
//    if (Autogate::isEnabled(AutogateKey::MY_NEW_FEATURE)) {
//      // new code path
//    } else {
//      // old code path
//    }
//    You should remove the if/else and keep only the new code path.

// --------------------------------------------------------------------------------------
// clang-format off
#define WORKERD_AUTOGATES(V)                                                                       \
  V(TEST_WORKERD)                                                                                  \
  V(V8_FAST_API)                                                                                   \
  /* Enables support for the streaming tail worker. Note that this is currently also guarded behind\
    an experimental compat flag. */                                                                \
  V(STREAMING_TAIL_WORKER)                                                                         \
  /* Enable refactor used to consolidate the different tail worker stream implementations. */      \
  V(TAIL_STREAM_REFACTOR)                                                                          \
  /* Enable fast TextEncoder implementation using simdutf */                                       \
  V(ENABLE_FAST_TEXTENCODER)                                                                       \
  V(UPDATED_AUTO_ALLOCATE_CHUNK_SIZE)                                                              \
  /* When enabled, reject startTls calls that pass the expectedServerHostname option,              \
    which is not currently supported. When disabled, log the usage instead. */                     \
  V(STARTTLS_REJECT_EXPECTED_SERVER_HOSTNAME)                                                      \
  /* Enable the HibernatableWebSocketAdapter-based implementation of HibernationManager and        \
     related plumbing. Gates the refactor of hibernatable-WebSocket internals tracked by           \
     EW-10817. */                                                                                  \
  V(HIBERNATABLE_WEBSOCKET_REFACTOR)                                                               \
  /* When enabled, turns on per-isolate TypeScript/JavaScript bootstrap */                         \
  V(PER_ISOLATE_JAVASCRIPT_BOOTSTRAP)                                                              \
  /* Gate for the Durable Object fetch-retries feature, scoped to DO `fetch()`. Enables           \
    observe-only retry-token claim machinery. */                                                   \
  V(DURABLE_OBJECT_RETRIES_FETCH)                                                                  \
  /* Enables Durable Object fetch retry requests and fail-closed receiver enforcement. The        \
     observe-only DURABLE_OBJECT_RETRIES_FETCH gate is a prerequisite. */                          \
  V(DURABLE_OBJECT_RETRIES_FETCH_RETRY_REQUESTS)                                                   \
  /* When enabled, the native `node-internal:url` module is provided by the Rust                   \
     implementation (api::node UrlUtil ported to src/rust/api) instead of the                      \
     C++ implementation. The C++ implementation is retained for rollback.*/                        \
  V(NODEJS_URL_RUST)                                                                               \
  /* When enabled, Node.js-style exceptions (api::node createNodeException /                       \
     createUVException) are created by the Rust implementation                                     \
     (src/rust/node-exceptions) instead of the C++ implementation. The C++                         \
     implementation is retained for rollback.*/                                                    \
  V(NODEJS_EXCEPTIONS_RUST)                                                                        \
  /* Reuse HTTP/1.1 tunnels opened by Container.getTcpPort().fetch(). */                           \
  V(CONTAINER_TUNNEL_REUSE)                                                                        \
  /* Allow a Socket to be transferred over JS RPC. When disabled, serializing a Socket fails as    \
     though the type were not serializable at all, and an incoming transferred Socket is           \
     rejected. */                                                                                  \
  V(SOCKET_RPC_TRANSFER)                                                                           \
  /* Materialize stream and socket externals of an incoming RPC value BEFORE the V8 value graph    \
     is deserialized (RpcDeserializerExternalHandler::prepare()), with deserialize() claiming the  \
     prebuilt objects. V8's deserializer forbids JS execution during the graph read, so this is    \
     the only phase in which TypeScript-implemented streams (whose construction runs JS) can be    \
     built; the mechanism itself is implementation-agnostic and runs for legacy streams too. When  \
     disabled, deserialization constructs legacy streams in place, exactly as before the gate      \
     existed; the typescript_implemented_streams compat flag requires this gate to receive         \
     streams over RPC (that combination is rejected, not degraded). */                             \
  V(RPC_EXTERNALS_HYDRATION)                                                                       \
  /* Back compression streams with memory-safe Rust implementations instead of the native C      \
     libraries. Currently covers zlib (zlib-rs) for node:zlib and web CompressionStream; the     \
     native implementations remain the default. */                                               \
  V(COMPRESSION_RS)                                                                                 \
  /* Enables per-call JSRPC tracing, trace-context propagation, and related Fetcher spans. */       \
  V(JSRPC_TRACING)
// clang-format on
// --------------------------------------------------------------------------------------

// When YES, initAutogate() ignores the WORKERD_ALL_AUTOGATES environment variable and uses only
// the gates list that was explicitly passed in. This is used by the production server, which has
// its own autogate test infrastructure that manages the all-autogates behavior and builds
// selective gate configs (with forceOff / toggle support).
WD_STRONG_BOOL(IgnoreAllAutogatesEnv);

// Workerd-specific list of autogate keys (can also be used in internal repo).
enum class AutogateKey {
#define V(key) key,
  WORKERD_AUTOGATES(V)
#undef V
      NumOfKeys  // Reserved for iteration.
};

// Convert an AutogateKey to an array index.
constexpr size_t autogateToIndex(AutogateKey key) {
  return static_cast<size_t>(key);
}

// Returns all AutogateKey values (excluding NumOfKeys) as an iterable range:
//
//     for (AutogateKey key: getAutogateKeys()) { ... }
constexpr kj::ArrayPtr<const AutogateKey> getAutogateKeys() {
  static constexpr AutogateKey keys[] = {
#define V(key) AutogateKey::key,
    WORKERD_AUTOGATES(V)
#undef V
  };
  return keys;
}
static_assert(getAutogateKeys().size() == autogateToIndex(AutogateKey::NumOfKeys));

// This class allows code changes to be rolled out independent of full binary releases. It enables
// specific code paths to be gradually rolled out via our internal tooling.
// See the equivalent file in our internal repo for more details.
//
// Workerd-specific gates can be added here.
//
// Usage:
//
//     #include <workerd/util/autogate.h>
//     Autogate::isEnabled(AutogateKey::YOUR_FEATURE_KEY)
//
// When making structural changes here, ensure you align them with autogate.h in the internal repo.
class Autogate {
 public:
  static bool isEnabled(AutogateKey key);

  // Creates a global Autogate and seeds it with gates that are specified in the config.
  //
  // This function is not thread safe, it should be called exactly once close to the start of the
  // process before any threads are created.
  static void initAutogate(capnp::List<capnp::Text>::Reader autogates,
      IgnoreAllAutogatesEnv ignoreEnv = IgnoreAllAutogatesEnv::NO);

  // Convenience method for bin-tests to invoke initAutogate() with an appropriate config.
  // Prefer the AutogateKey overload in hand-written tests for compile-time safety.
  static void initAutogateNamesForTest(std::initializer_list<kj::StringPtr> gateNames,
      IgnoreAllAutogatesEnv ignoreEnv = IgnoreAllAutogatesEnv::NO);
  static void initAutogateNamesForTest(kj::ArrayPtr<const kj::StringPtr> gateNames,
      IgnoreAllAutogatesEnv ignoreEnv = IgnoreAllAutogatesEnv::NO);

  // Type-safe overload: takes enum values directly, avoiding silent typo bugs.
  static void initAutogateForTest(std::initializer_list<AutogateKey> keys);

  // Initializes all autogates to true. Used for testing with the --all-autogates flag.
  static void initAllAutogates();

  // Destroys an initialized global Autogate instance. Used only for testing.
  static void deinitAutogate();

 private:
  bool gates[autogateToIndex(AutogateKey::NumOfKeys)] = {};

  Autogate() = default;
  Autogate(capnp::List<capnp::Text>::Reader autogates);
};

// Retrieves the name of the gate.
kj::StringPtr KJ_STRINGIFY(AutogateKey key);

}  // namespace workerd::util
