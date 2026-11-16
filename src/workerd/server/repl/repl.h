// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// The C++ half of `workerd repl`: the functions the command line's REPL (cli/repl/) calls through
// its cxx bridge (cli/repl/bridge.rs). Rust reads and edits the input, splits it into inputs and
// decides how each may compile; C++ owns V8 and evaluates (repl-environment.h).
//
// Do not include the generated bridge header here: the bridge includes this header.

#include <rust/cxx.h>

#include <kj/async.h>
#include <kj/memory.h>

namespace kj_rs_tokio {
struct TokioAsyncIoContext;
}  // namespace kj_rs_tokio

namespace workerd::server::repl {

struct ReplOptions;
struct ReplCandidate;

// A REPL session: V8, the I/O the loaded configs use, and the environment input runs in. Created
// on the event loop's thread, and used only there.
class ReplSession {
 public:
  class Impl;

  explicit ReplSession(kj::Own<Impl> impl);
  ~ReplSession() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(ReplSession);

  Impl& getImpl() {
    return *impl;
  }

 private:
  kj::Own<Impl> impl;
};

// See the bridge (cli/repl/bridge.rs).
kj::Own<ReplSession> repl_session_new(
    kj_rs_tokio::TokioAsyncIoContext& loop, const ReplOptions& options);
kj::Promise<::rust::String> repl_evaluate(
    ReplSession& session, ::rust::Vec<ReplCandidate> candidates);
kj::Promise<::rust::Vec<::rust::String>> repl_property_names(
    ReplSession& session, ::rust::String base);
kj::Promise<void> repl_drain(ReplSession& session);

}  // namespace workerd::server::repl
