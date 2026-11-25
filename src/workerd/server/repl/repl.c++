// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "repl.h"

#include "repl-environment.h"

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

ReplEnvironmentOptions toEnvironmentOptions(const ReplOptions& options) {
  return {
    .compatibilityDate = options.compat_date.empty()
        ? kj::Maybe<kj::String>(kj::none)
        : kj::str(kj::from<kj_rs::Rust>(options.compat_date)),
    .compatibilityFlags =
        KJ_MAP(flag, options.compat_flags) { return kj::str(kj::from<kj_rs::Rust>(flag)); },
    .experimental = options.experimental,
    .colors = options.colors,
  };
}

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

kj::Array<ReplEnvironment::Candidate> toCandidates(::rust::Vec<ReplCandidate> candidates) {
  return KJ_MAP(candidate, candidates) {
    return ReplEnvironment::Candidate{
      .source = kj::str(kj::from<kj_rs::Rust>(candidate.source)),
      .awaited = candidate.awaited,
      .parenthesized = candidate.parenthesized,
    };
  };
}

// Compiles input as the console does (cli/repl/input.rs).
class CandidateSource final: public ReplEnvironment::CandidateSource {
 public:
  kj::Array<ReplEnvironment::Candidate> candidates(kj::StringPtr input) override {
    return toCandidates(repl_candidates(input.as<kj_rs::RustUncheckedUtf8>()));
  }
};

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

  jsg::V8System& get(const ReplOptions& options) {
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

jsg::V8System& processV8(const ReplOptions& options) {
  // Never destroyed. A failure to set up, such as an unknown autogate, leaves it to the next
  // session to try again.
  static ProcessV8* v8 = new ProcessV8(toStrings(options.autogates), toStrings(options.v8_flags));
  return v8->get(options);
}

}  // namespace

// What a session sets up, in construction order: the environment is torn down before the I/O it
// was given.
class ReplSession::Impl {
 public:
  Impl(kj_rs_tokio::TokioAsyncIoContext& loop,
      const ReplOptions& options,
      ::rust::Box<ServerRegistry> registryParam)
      : timer(loop.getTimer()),
        monotonicClock(kj::systemPreciseMonotonicClock()),
        provider(loop.getTimer()),
        registry(kj::mv(registryParam)),
        environment(
            ReplServerResources{
              .v8System = processV8(options),
              .timer = timer,
              .monotonicClock = monotonicClock,
              .network = provider.getNetwork(),
              .entropySource = entropySource,
              .fs = *fs,
            },
            *registry,
            candidateSource,
            toEnvironmentOptions(options)) {}

  ReplEnvironment& getEnvironment() {
    return environment;
  }

 private:
  kj::Own<kj::Filesystem> fs = kj::newDiskFilesystem();
  EntropySourceImpl entropySource;
  kj::Timer& timer;
  const kj::MonotonicClock& monotonicClock;
  kj_rs_io::TokioAsyncIoProvider provider;
  ::rust::Box<ServerRegistry> registry;
  CandidateSource candidateSource;
  ReplEnvironment environment;
};

ReplSession::ReplSession(kj::Own<Impl> impl): impl(kj::mv(impl)) {}
ReplSession::~ReplSession() noexcept(false) {}

kj::Own<ReplSession> repl_session_new(kj_rs_tokio::TokioAsyncIoContext& loop,
    const ReplOptions& options,
    ::rust::Box<ServerRegistry> registry) {
  return kj::heap<ReplSession>(kj::heap<ReplSession::Impl>(loop, options, kj::mv(registry)));
}

kj::Promise<ReplEvaluation> repl_evaluate(
    ReplSession& session, ::rust::Vec<ReplCandidate> candidates) {
  auto evaluation =
      co_await session.getImpl().getEnvironment().evaluate(toCandidates(kj::mv(candidates)));
  co_return ReplEvaluation{
    // Rendered from JavaScript strings, which convert to valid UTF-8.
    .text = evaluation.text.as<kj_rs::RustUncheckedUtf8>(),
    .threw = evaluation.threw,
  };
}

kj::Promise<::rust::Vec<::rust::String>> repl_property_names(
    ReplSession& session, ::rust::String base) {
  auto names = co_await session.getImpl().getEnvironment().propertyNames(
      kj::str(kj::from<kj_rs::Rust>(base)));
  ::rust::Vec<::rust::String> result;
  result.reserve(names.size());
  for (auto& name: names) {
    result.push_back(name.as<kj_rs::RustUncheckedUtf8>());
  }
  co_return result;
}

kj::Promise<bool> repl_drain(ReplSession& session) {
  return session.getImpl().getEnvironment().drain();
}

}  // namespace workerd::server::repl
