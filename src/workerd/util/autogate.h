// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

#include <workerd/util/strong-bool.h>

#include <capnp/blob.h>
#include <capnp/list.h>
#include <kj/string.h>

#include <initializer_list>

namespace workerd::util {

// When YES, initAutogate() ignores the WORKERD_ALL_AUTOGATES environment variable and uses only
// the gates list that was explicitly passed in. This is used by the production server, which has
// its own autogate test infrastructure that manages the all-autogates behavior and builds
// selective gate configs (with forceOff / toggle support).
WD_STRONG_BOOL(IgnoreAllAutogatesEnv);

// Workerd-specific list of autogate keys (can also be used in internal repo).
enum class AutogateKey {
  TEST_WORKERD,
  V8_FAST_API,
  // Enables support for the streaming tail worker. Note that this is currently also guarded behind
  // an experimental compat flag.
  STREAMING_TAIL_WORKER,
  // Enable refactor used to consolidate the different tail worker stream implementations.
  TAIL_STREAM_REFACTOR,
  // Enable fast TextEncoder implementation using simdutf
  ENABLE_FAST_TEXTENCODER,
  // Apply an updated default autoAllocateChunkSize for ReadableStreams
  UPDATED_AUTO_ALLOCATE_CHUNK_SIZE,
  // When enabled, reject startTls calls that pass the expectedServerHostname option,
  // which is not currently supported. When disabled, log the usage instead.
  STARTTLS_REJECT_EXPECTED_SERVER_HOSTNAME,
  // Enable the HibernatableWebSocketAdapter-based implementation of HibernationManager and
  // related plumbing. Gates the refactor of hibernatable-WebSocket internals tracked by
  // EW-10817.
  HIBERNATABLE_WEBSOCKET_REFACTOR,
  // When enabled, turns on per-isolate TypeScript/JavaScript bootstrap
  PER_ISOLATE_JAVASCRIPT_BOOTSTRAP,
  // Gate for the Durable Object fetch-retries feature, scoped to DO `fetch()`. Enables the
  // retry-token claim machinery.
  DURABLE_OBJECT_RETRIES_FETCH,
  // When enabled, the native `node-internal:url` module is provided by the Rust
  // implementation (api::node UrlUtil ported to src/rust/api) instead of the
  // C++ implementation. The C++ implementation is retained for rollback.
  NODEJS_URL_RUST,
  NumOfKeys  // Reserved for iteration.
};

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
  static void initAutogateNamesForTest(std::initializer_list<kj::StringPtr> gateNames);

  // Initializes all autogates to true. Used for testing with the --all-autogates flag.
  static void initAllAutogates();

  // Destroys an initialized global Autogate instance. Used only for testing.
  static void deinitAutogate();

 private:
  bool gates[static_cast<unsigned long>(AutogateKey::NumOfKeys)] = {};

  Autogate() = default;
  Autogate(capnp::List<capnp::Text>::Reader autogates);
};

// Retrieves the name of the gate.
//
// When adding a new gate, add it into this method as well.
kj::StringPtr KJ_STRINGIFY(AutogateKey key);

}  // namespace workerd::util
