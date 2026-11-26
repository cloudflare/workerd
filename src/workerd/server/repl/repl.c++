// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "repl.h"

#include <workerd/server/cli/repl/bridge.rs.h>
#include <workerd/server/factory/per-process.h>
#include <workerd/util/autogate.h>
#include <workerd/util/entropy.h>

#include <kj-rs-io/async-io.h>
#include <kj-rs-tokio/tokio-event-port.h>
#include <kj-rs/kj-rs.h>

namespace workerd::server::repl {

namespace {

class EntropySourceImpl final: public kj::EntropySource {
 public:
  void generate(kj::ArrayPtr<kj::byte> buffer) override {
    getEntropy(buffer);
  }
};

kj::Array<kj::String> toStrings(const ::rust::Vec<::rust::String>& strings) {
  return KJ_MAP(string, strings) { return kj::str(kj::from<kj_rs::Rust>(string)); };
}

bool isAutogate(kj::StringPtr name) {
  for (auto key: util::getAutogateKeys()) {
    if (kj::str(key) == name) return true;
  }
  return false;
}

// The process's autogates, as `ProcessState::Options` takes them, for `names`, each a gate's
// kebab-case name. Throws if one names no gate, which the process would ignore.
kj::Array<kj::String> autogateNames(kj::ArrayPtr<const kj::String> names) {
  return KJ_MAP(name, names) {
    if (!isAutogate(name)) {
      auto known = KJ_MAP(key, util::getAutogateKeys()) { return kj::str(key); };
      kj::throwFatalException(KJ_EXCEPTION(FAILED,
          kj::str("No such autogate: ", name, ". The autogates are: ", kj::strArray(known, ", "))));
    }
    return kj::str("workerd-autogate-", name);
  };
}

bool sameStrings(kj::ArrayPtr<const kj::String> strings, const ::rust::Vec<::rust::String>& rust) {
  if (strings.size() != rust.size()) return false;
  for (auto i: kj::indices(strings)) {
    if (strings[i] != kj::str(kj::from<kj_rs::Rust>(rust[i]))) return false;
  }
  return true;
}

// The process's state: its autogates and V8 (factory/per-process.h). V8 cannot start again once
// torn down, so the first session starts the process for the rest of it, and a later one, as in a
// test that opens many, must ask for the same autogates and V8 flags. The process exits with it.
class ReplProcess {
 public:
  ReplProcess(kj::Array<kj::String> autogatesParam, kj::Array<kj::String> v8FlagsParam)
      : autogates(kj::mv(autogatesParam)),
        v8Flags(kj::mv(v8FlagsParam)),
        state(ProcessState::start(ProcessState::Options{
          .perfettoTracePath = kj::none,
          .perfettoTraceCategories = kj::String(),
          .asyncTracePath = kj::none,
          .autogates = autogateNames(autogates),
          .v8Flags = KJ_MAP(flag, v8Flags) { return kj::str(flag); },
        })) {}

  const ProcessState& get(const ReplProcessOptions& options) {
    KJ_REQUIRE(sameStrings(autogates, options.autogates) && sameStrings(v8Flags, options.v8_flags),
        "the autogates and V8 flags are the process's, which its first REPL session set");
    return *state;
  }

 private:
  kj::Array<kj::String> autogates;
  kj::Array<kj::String> v8Flags;
  kj::Arc<ProcessState> state;
};

const ProcessState& replProcess(const ReplProcessOptions& options) {
  // Never destroyed. A failure to start, such as an unknown autogate, leaves it to the next
  // session to try again.
  static ReplProcess* process =
      new ReplProcess(toStrings(options.autogates), toStrings(options.v8_flags));
  return process->get(options);
}

}  // namespace

// What a session sets up, in construction order.
class ReplSession::Impl {
 public:
  Impl(kj_rs_tokio::TokioAsyncIoContext& loop, const ReplProcessOptions& options)
      : timer(loop.getTimer()),
        monotonicClock(kj::systemPreciseMonotonicClock()),
        provider(loop.getTimer()),
        resources(kj::rc<ReplResources>(ReplServerResources{
          .process = replProcess(options),
          .timer = timer,
          .monotonicClock = monotonicClock,
          .network = provider.getNetwork(),
          .entropySource = entropySource,
          .fs = *fs,
        })) {}

  kj::Rc<ReplResources> getResources() {
    return resources.addRef();
  }

 private:
  kj::Own<kj::Filesystem> fs = kj::newDiskFilesystem();
  EntropySourceImpl entropySource;
  kj::Timer& timer;
  const kj::MonotonicClock& monotonicClock;
  kj_rs_io::TokioAsyncIoProvider provider;
  kj::Rc<ReplResources> resources;
};

ReplSession::ReplSession(kj::Own<Impl> impl): impl(kj::mv(impl)) {}
ReplSession::~ReplSession() noexcept(false) {}

kj::Own<ReplSession> repl_session_new(
    kj_rs_tokio::TokioAsyncIoContext& loop, const ReplProcessOptions& options) {
  return kj::heap<ReplSession>(kj::heap<ReplSession::Impl>(loop, options));
}

kj::Rc<ReplResources> repl_session_resources(ReplSession& session) {
  return session.getImpl().getResources();
}

}  // namespace workerd::server::repl
