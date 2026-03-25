// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/async.h>
#include <kj/refcount.h>
#include <kj/string.h>

namespace workerd {

// Per-request Cloudflare Access authentication information.
//
// This is the I/O-side carrier for Access auth data. It is created by the embedding application
// (e.g. the production runtime) before invoking the worker, plumbed through `newWorkerEntrypoint()`
// into the `IoContext::IncomingRequest`, and surfaced to JavaScript by the concrete
// `api::AccessContext` wrapper as `ctx.access`.
//
// In standalone workerd this is never constructed; `ctx.access` evaluates to `undefined`.
//
// This type intentionally lives in `io/` rather than `api/` because:
// - It is the polymorphism boundary between embedders (workerd vs. production), not the
//   JS-facing type.
// - It carries per-request data that flows through `newWorkerEntrypoint` → `IncomingRequest`,
//   not through `Worker::Api` (which is per-isolate) or `IoChannelFactory`.
class AccessInfo: public kj::Refcounted {
 public:
  virtual ~AccessInfo() noexcept(false) = default;

  // The audience claim from the Access JWT. Stable for the lifetime of the request.
  virtual kj::StringPtr getAudience() = 0;

  // Fetches the full identity information for the authenticated user, equivalent to calling
  // /cdn-cgi/access/get-identity. The returned string is a JSON document; `kj::none` indicates
  // no identity is available (e.g. service-token authentication).
  virtual kj::Promise<kj::Maybe<kj::String>> getIdentity() = 0;
};

}  // namespace workerd
