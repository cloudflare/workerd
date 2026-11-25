// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "repl.h"

#include <workerd/jsg/exception.h>
#include <workerd/jsg/setup.h>
#include <workerd/server/cli/repl/bridge.rs.h>
#include <workerd/server/v8-platform-impl.h>
#include <workerd/util/autogate.h>
#include <workerd/util/entropy.h>

#include <kj-rs-io/async-io.h>
#include <kj-rs-tokio/tokio-event-port.h>
#include <kj-rs/kj-rs.h>

#include <capnp/message.h>

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

// Sets the process's autogates to `names`, each a gate's kebab-case name. Throws if one names no
// gate, which a config's `autogates` would ignore.
void initAutogates(kj::ArrayPtr<const kj::String> names) {
  capnp::MallocMessageBuilder message;
  auto gates = message.getOrphanage().newOrphan<capnp::List<capnp::Text>>(names.size());
  for (auto i: kj::indices(names)) {
    if (!isAutogate(names[i])) {
      auto known = KJ_MAP(key, util::getAutogateKeys()) { return kj::str(key); };
      kj::throwFatalException(KJ_EXCEPTION(FAILED,
          kj::str(
              "No such autogate: ", names[i], ". The autogates are: ", kj::strArray(known, ", "))));
    }
    gates.get().set(i, kj::str("workerd-autogate-", names[i]));
  }
  util::Autogate::initAutogate(gates.getReader());
}

bool sameStrings(kj::ArrayPtr<const kj::String> strings, const ::rust::Vec<::rust::String>& rust) {
  if (strings.size() != rust.size()) return false;
  for (auto i: kj::indices(strings)) {
    if (strings[i] != kj::str(kj::from<kj_rs::Rust>(rust[i]))) return false;
  }
  return true;
}

// V8 and the autogates, which are the process's. V8 cannot start again once torn down, so the
// first session sets both up for the rest of the process, and a later one, as in a test that
// opens many, must ask for the same. The process exits with them.
class ProcessV8 {
 public:
  ProcessV8(kj::Array<kj::String> autogatesParam, kj::Array<kj::String> v8FlagsParam)
      : autogates(kj::mv(autogatesParam)),
        v8Flags(kj::mv(v8FlagsParam)),
        platform(initPlatform(autogates)),
        v8Platform(*platform),
        v8System(
            v8Platform, KJ_MAP(flag, v8Flags) -> kj::StringPtr { return flag; }, platform.get()) {}

  jsg::V8System& get(const ReplProcessOptions& options) {
    KJ_REQUIRE(sameStrings(autogates, options.autogates) && sameStrings(v8Flags, options.v8_flags),
        "the autogates and V8 flags are the process's, which its first REPL session set");
    return v8System;
  }

 private:
  kj::Array<kj::String> autogates;
  kj::Array<kj::String> v8Flags;
  kj::Own<v8::Platform> platform;
  WorkerdPlatform v8Platform;
  jsg::V8System v8System;

  // Autogates are set before V8 starts.
  static kj::Own<v8::Platform> initPlatform(kj::ArrayPtr<const kj::String> autogates) {
    initAutogates(autogates);
    return jsg::defaultPlatform(0);
  }
};

jsg::V8System& processV8(const ReplProcessOptions& options) {
  // Never destroyed. A failure to set up, such as an unknown autogate, leaves it to the next
  // session to try again.
  static ProcessV8* v8 = new ProcessV8(toStrings(options.autogates), toStrings(options.v8_flags));
  return v8->get(options);
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
          .v8System = processV8(options),
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
