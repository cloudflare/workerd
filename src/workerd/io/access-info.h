// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/common.h>
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

  // Returns the subrequest channel index for the Access "binding worker", on which workerd invokes
  // the `getIdentity` JS-RPC method (via a `Fetcher`) to fetch the authenticated user's full
  // identity (equivalent to calling /cdn-cgi/access/get-identity).
  //
  // Returns `kj::none` when no identity service is available for this request (e.g. service-token
  // authentication, or the embedder has no channel configured), in which case
  // `ctx.access.getIdentity()` resolves to `undefined`.
  //
  // This boundary is deliberately narrow: the embedder is responsible only for *routing* a channel
  // to the Access binding worker (e.g. via a per-request channel token), while workerd owns the
  // JS-RPC dispatch and result handling.
  virtual kj::Maybe<kj::uint> getIdentityServiceChannel() = 0;
};

}  // namespace workerd
