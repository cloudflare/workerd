// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/server/fallback-client/ffi.rs.h>
#include <workerd/server/workerd.capnp.h>

#include <kj/map.h>
#include <kj/one-of.h>
#include <kj/string.h>
#include <kj/thread.h>

#include <atomic>

namespace workerd::fallback {

// The client of the module fallback service, a mechanism used only in workerd local development:
// a worker configured with a `moduleFallback` address (which requires `--experimental`) asks the
// HTTP service there for the modules its bundle does not contain. The protocol (V1 for the legacy
// module registry, V2 for the new one) and the client are the `fallback-client` Rust crate's; see
// lib.rs there.
//
// The service may answer with a module, a redirect to a different module specifier, or an error.
using ModuleOrRedirect =
    kj::Maybe<kj::OneOf<kj::String, kj::Own<server::config::Worker::Module::Reader>>>;

// A client with a background thread of its own and one long-lived HTTP connection, reused across
// requests; the caller blocks until the thread answers. The thread is a kj::Thread, so it logs
// through the creating thread's logger (the JSON logger under structured logging).
//
// IMPORTANT: This class supports only one caller at a time. tryResolve() will assert if called
// concurrently. Module resolution in workerd is single-threaded per isolate/registry so this is
// safe in practice.
class FallbackServiceClient {
 public:
  explicit FallbackServiceClient(kj::StringPtr address);
  ~FallbackServiceClient() noexcept(false);

  KJ_DISALLOW_COPY_AND_MOVE(FallbackServiceClient);

  // The module `specifier` resolves to, or the specifier to resolve instead; kj::none if the
  // service has no such module or could not be asked (the reason is logged). An empty
  // `rawSpecifier` is one that is not known. V1 sends no attributes. Throws once the background
  // thread has exited.
  ModuleOrRedirect tryResolve(Version version,
      ImportType type,
      kj::StringPtr specifier,
      kj::StringPtr rawSpecifier,
      kj::StringPtr referrer,
      const kj::HashMap<kj::StringPtr, kj::StringPtr>& attributes);

 private:
  explicit FallbackServiceClient(NewClient parts);

  // Declared before `client`, so that `client` is dropped first: that ends the worker's loop,
  // and the kj::Thread destructor then joins the thread.
  kj::Own<kj::Thread> thread;
  ::rust::Box<FallbackClient> client;
  std::atomic<bool> busy = false;
};

}  // namespace workerd::fallback
