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

#include <cstring>

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

// The files a config was loaded from, watched by the CLI's watcher (cli/watch.rs).
class ConfigWatch final: public ReplConfigWatch {
 public:
  explicit ConfigWatch(::rust::Box<ReplWatch> watch): watch(kj::mv(watch)) {}

  kj::Promise<void> whenChanged() override {
    return watch->changed();
  }

  ReplWatch& getRust() {
    return *watch;
  }

 private:
  ::rust::Box<ReplWatch> watch;
};

kj::Array<ReplEnvironment::Candidate> toCandidates(::rust::Vec<ReplCandidate> candidates) {
  return KJ_MAP(candidate, candidates) {
    return ReplEnvironment::Candidate{
      .source = kj::str(kj::from<kj_rs::Rust>(candidate.source)),
      .awaited = candidate.awaited,
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

// Loads configs as `workerd serve` does (cli/repl/session.rs).
class ConfigLoader final: public ReplConfigLoader {
 public:
  kj::Array<capnp::word> load(kj::StringPtr path,
      kj::Maybe<kj::StringPtr> constName,
      bool experimental,
      kj::Maybe<ReplConfigWatch&> watch) override {
    auto rustPath = path.as<kj_rs::RustUncheckedUtf8>();
    auto rustConstName = constName.orDefault(""_kj).as<kj_rs::RustUncheckedUtf8>();
    ::rust::Vec<uint64_t> words;
    KJ_IF_SOME(e, kj::runCatchingExceptions([&]() {
      KJ_IF_SOME(w, watch) {
        // Every watch this loader is given came from its newWatch().
        words = kj::downcast<ConfigWatch>(w).getRust().load_config(
            rustPath, rustConstName, experimental);
      } else {
        words = repl_load_config(rustPath, rustConstName, experimental);
      }
    })) {
      JSG_FAIL_REQUIRE(Error, e.getDescription());
    }
    auto config = kj::heapArray<capnp::word>(words.size());
    memcpy(config.begin(), words.data(), words.size() * sizeof(uint64_t));
    return config;
  }

  kj::Own<ReplConfigWatch> newWatch() override {
    return kj::heap<ConfigWatch>(repl_watch_new());
  }
};

}  // namespace

// The process-wide state a session sets up, in construction order: V8 is torn down after the
// environment and before the I/O it was given.
class ReplSession::Impl {
 public:
  Impl(kj_rs_tokio::TokioAsyncIoContext& loop, const ReplOptions& options)
      : timer(loop.getTimer()),
        monotonicClock(kj::systemPreciseMonotonicClock()),
        provider(loop.getTimer()),
        v8Flags(toStrings(options.v8_flags)),
        platform(initPlatform(toStrings(options.autogates))),
        v8Platform(*platform),
        v8System(v8Platform,
            KJ_MAP(flag, v8Flags) -> kj::StringPtr { return flag; }, platform.get()),
        environment(
            ReplServerResources{
              .v8System = v8System,
              .timer = timer,
              .monotonicClock = monotonicClock,
              .network = provider.getNetwork(),
              .entropySource = entropySource,
              .fs = *fs,
            },
            configLoader,
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
  kj::Array<kj::String> v8Flags;
  kj::Own<v8::Platform> platform;
  WorkerdPlatform v8Platform;
  jsg::V8System v8System;
  ConfigLoader configLoader;
  CandidateSource candidateSource;
  ReplEnvironment environment;

  // Autogates are process-wide and set before V8 starts.
  static kj::Own<v8::Platform> initPlatform(kj::Array<kj::String> autogates) {
    initAutogates(autogates);
    return jsg::defaultPlatform(0);
  }
};

ReplSession::ReplSession(kj::Own<Impl> impl): impl(kj::mv(impl)) {}
ReplSession::~ReplSession() noexcept(false) {}

kj::Own<ReplSession> repl_session_new(
    kj_rs_tokio::TokioAsyncIoContext& loop, const ReplOptions& options) {
  return kj::heap<ReplSession>(kj::heap<ReplSession::Impl>(loop, options));
}

kj::Promise<::rust::String> repl_evaluate(
    ReplSession& session, ::rust::Vec<ReplCandidate> candidates) {
  auto result =
      co_await session.getImpl().getEnvironment().evaluate(toCandidates(kj::mv(candidates)));
  // Rendered from JavaScript strings, which convert to valid UTF-8.
  co_return result.as<kj_rs::RustUncheckedUtf8>();
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

kj::Promise<void> repl_drain(ReplSession& session) {
  return session.getImpl().getEnvironment().drain();
}

}  // namespace workerd::server::repl
