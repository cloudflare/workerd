// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "bootstrap.h"

#include "worker-factory.h"

#include <workerd/io/compatibility-date.capnp.h>
#include <workerd/io/compatibility-date.h>
#include <workerd/io/release-version.embed.h>
#include <workerd/jsg/setup.h>
#include <workerd/server/json-logger.h>
#include <workerd/server/server/bridge.rs.h>
#include <workerd/server/v8-platform-impl.h>
#include <workerd/server/workerd.capnp.h>
#include <workerd/util/autogate.h>
#include <workerd/util/entropy.h>
#include <workerd/util/perfetto-tracing.h>
#include <workerd/util/thread-scopes.h>

#include <kj-rs-io/async-io.h>
#include <kj-rs/kj-rs.h>

#include <capnp/message.h>
#include <capnp/serialize.h>
#include <kj/filesystem.h>
#include <kj/io.h>
#include <kj/miniposix.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>

#if !_WIN32
#include <unistd.h>
#endif

#if _WIN32
#include <windows.h>

#include <kj/win32-api-version.h>
#include <kj/windows-sanity.h>
#endif

#include <workerd/util/use-perfetto-categories.h>

// since kj installs their global signal handlers
// and exits with 1 Fuzzilli doesn't realize that an application crashed due to the signo.
// Therefore, we install a handler before and just raise the signo
#ifdef WORKERD_FUZZILLI

void signalHandler(int signo, siginfo_t* info, void* context) noexcept {
  // inform reprl - remove debug output for clean testing
  struct sigaction sa = {};
  sa.sa_handler = SIG_DFL;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(signo, &sa, nullptr);
  raise(signo);
}

void initSignalHandlers() {
  struct sigaction action {};
  action.sa_flags = SA_SIGINFO;
  action.sa_sigaction = &signalHandler;

  for (auto signo: {SIGBUS, SIGFPE, SIGABRT, SIGILL, SIGTRAP, SIGSEGV}) {
    KJ_SYSCALL(sigaction(signo, &action, nullptr));
  }
}
#endif

using namespace kj_rs;

namespace workerd::server::cli {
namespace {

// =======================================================================================

// For ASan's leak sanitizer, suppress warnings about leaks with stacks that include "unknown
// modules". This suppression is adopted from the GN build and applies to addresses that LSan can't
// symbolize or even map to a binary – perhaps JIT or snapshot-generated code in V8's case?
// TODO(someday): Suppression is needed to get several python tests to pass under LSan. Investigate
// if this is an actual leak (perhaps a bug in V8 itself since it is suppressed there?) at a later
// time.
#if __has_feature(address_sanitizer)
extern "C" __attribute__((no_sanitize("address"))) __attribute__((visibility("default")))
__attribute__((used)) const char*
__lsan_default_suppressions() {
  return "leak:<unknown module>\n";
}
#endif

// =======================================================================================

class EntropySourceImpl: public kj::EntropySource {
 public:
  void generate(kj::ArrayPtr<kj::byte> buffer) override {
    getEntropy(buffer);
  }
};

// =======================================================================================

bool structuredLogging(const ::rust::Vec<uint64_t>& configWords) {
  capnp::FlatArrayMessageReader reader(asWords(kj::from<Rust>(configWords)), CONFIG_READER_OPTIONS);
  auto config = reader.getRoot<config::Config>();
  return config.hasLogging() ? config.getLogging().getStructuredLogging()
                             : config.getStructuredLogging();
}

// What the process needs around the Rust server (see the bridge), attached to the factory it
// builds. Members are declared in construction order, which is the reverse of destruction: V8 goes
// before the providers.
class Bootstrap {
 public:
  Bootstrap(kj_rs_tokio::TokioAsyncIoContext& loop,
      const ::rust::Vec<uint64_t>& configWords,
      const ServeOrTestOptions& serveOrTest,
      kj::Maybe<const TestOptions&> test)
      : timer(loop.getTimer()),
        monotonicClock(kj::systemPreciseMonotonicClock()),
        provider(loop.getTimer()) {
    KJ_IF_SOME(path, serveOrTest.perfetto_trace_path) {
#ifdef WORKERD_USE_PERFETTO
      perfettoSession = PerfettoSession(kj::str(path),
          serveOrTest.perfetto_trace_categories
              .map([](const ::rust::String& categories) {
        return kj::str(categories);
      }).orDefault(kj::String()));
#else
      KJ_UNIMPLEMENTED("perfetto tracing is not supported by this build", kj::str(path));
#endif
    }
    TRACE_EVENT("workerd", "Bootstrap()");

    // The process is set up for this config and these options: autogates, the test modes, and V8
    // with its flags (V8 reads the test modes as it starts).
    capnp::FlatArrayMessageReader reader(
        asWords(kj::from<Rust>(configWords)), CONFIG_READER_OPTIONS);
    auto config = reader.getRoot<config::Config>();
    util::Autogate::initAutogate(config.getAutogates());
    KJ_IF_SOME(t, test) {
      setUpTestProcess(t);
    }

    platform = jsg::defaultPlatform(0);
    v8Platform = kj::heap<WorkerdPlatform>(*platform);
    v8System = kj::heap<jsg::V8System>(*v8Platform,
        KJ_MAP(flag, config.getV8Flags()) -> kj::StringPtr { return flag; }, platform.get());
  }

  KJ_DISALLOW_COPY_AND_MOVE(Bootstrap);

  // A factory over `configWords`, which it owns for its run.
  kj::Own<WorkerFactory> makeFactory(::rust::Vec<uint64_t> configWords,
      const ServeOrTestOptions& serveOrTest,
      kj::Maybe<const TestOptions&> test) {
    auto options = kj::heap<WorkerFactory::Options>();
    options->loggingOptions = Worker::LoggingOptions(Worker::ConsoleMode::STDOUT);
    applyServeOrTestOptions(*options, serveOrTest);
    KJ_IF_SOME(t, test) {
      applyTestOptions(*options, t);
    }
    return kj::heap<WorkerFactory>(*v8System, timer, monotonicClock, provider.getNetwork(),
        entropySource, *fs, kj::mv(options), kj::mv(configWords));
  }

 private:
  kj::Own<kj::Filesystem> fs = kj::newDiskFilesystem();
  EntropySourceImpl entropySource;
  kj::Timer& timer;
  // The loop's monotonic clock, consistent with `timer`.
  const kj::MonotonicClock& monotonicClock;
  // The tokio-backed kj::Network over the Runtime's loop, for the factory.
  kj_rs_io::TokioAsyncIoProvider provider;

#ifdef WORKERD_USE_PERFETTO
  kj::Maybe<PerfettoSession> perfettoSession;
#endif

  kj::Own<v8::Platform> platform;
  kj::Own<WorkerdPlatform> v8Platform;
  kj::Own<jsg::V8System> v8System;

  kj::Maybe<kj::Own<const kj::Directory>> openWritableDirectory(kj::StringPtr pathStr) {
    return fs->getRoot().tryOpenSubdir(fs->getCurrentPath().eval(pathStr), kj::WriteMode::MODIFY);
  }

  void applyServeOrTestOptions(
      WorkerFactory::Options& options, const ServeOrTestOptions& serveOrTest) {
    options.experimental = serveOrTest.experimental;
    auto& python = options.pythonConfig;
    KJ_IF_SOME(path, serveOrTest.pyodide_package_disk_cache_dir) {
      // The command line checked that the directory exists.
      python.packageDiskCacheRoot = KJ_REQUIRE_NONNULL(
          openWritableDirectory(kj::str(path)), "package disk cache dir must exist");
    }
    KJ_IF_SOME(path, serveOrTest.pyodide_bundle_disk_cache_dir) {
      python.pyodideDiskCacheRoot = openWritableDirectory(kj::str(path));
    }
    python.createSnapshot = serveOrTest.python_save_snapshot;
    python.createBaselineSnapshot = serveOrTest.python_save_baseline_snapshot;
    KJ_IF_SOME(path, serveOrTest.python_load_snapshot) {
      python.loadSnapshotFromDisk = kj::str(path);
    }
    KJ_IF_SOME(path, serveOrTest.python_snapshot_dir) {
      python.snapshotDirectory = openWritableDirectory(kj::str(path));
    }
  }

  // The `test` options that apply to the process.
  static void setUpTestProcess(const TestOptions& testOptions) {
    if (!testOptions.no_verbose) {
      // Always turn on info logging when running tests so that uncaught exceptions are displayed.
      // TODO(beta): This can be removed once we improve our error logging story.
      kj::_::Debug::setLogLevel(kj::LogSeverity::INFO);
    }
    if (testOptions.predictable) {
      setPredictableModeForTest();
    }
    if (testOptions.gc_stress) {
      setGcStressModeForTest();
    }
    if (testOptions.all_autogates) {
      util::Autogate::initAllAutogates();
    }
  }

  // The `test` options that apply to a factory.
  static void applyTestOptions(WorkerFactory::Options& options, const TestOptions& testOptions) {
    KJ_IF_SOME(compatDate, testOptions.compat_date) {
      options.testCompatibilityDateOverride = kj::str(compatDate);
    }
  }
};

}  // namespace

::rust::Slice<const uint8_t> release_version() {
  return RELEASE_VERSION.asBytes().as<Rust>();
}

bool perfetto_supported() {
#ifdef WORKERD_USE_PERFETTO
  return true;
#else
  return false;
#endif
}

bool fuzzilli_supported() {
#if defined(WORKERD_FUZZILLI) && defined(__linux__)
  return true;
#else
  return false;
#endif
}

::rust::String pyodide_lock() {
  capnp::MallocMessageBuilder message;
  // TODO(EW-8977): Implement option to specify python worker flags.
  auto features = message.getRoot<CompatibilityFlags>();
  features.setPythonWorkers(true);
  auto pythonRelease = KJ_REQUIRE_NONNULL(getPythonSnapshotRelease(features));
  auto lock = KJ_REQUIRE_NONNULL(api::pyodide::getPyodideLock(pythonRelease));
  return lock.as<RustCopyUncheckedUtf8>();
}

int32_t with_process_context(
    bool verbose, ::rust::Vec<uint64_t> config, ::rust::Box<PendingCommand> command) {
  kj::printStackTraceOnCrash();
#if defined(WORKERD_FUZZILLI) && defined(__linux__)
  initSignalHandlers();
#endif
  if (verbose) {
    kj::_::Debug::setLogLevel(kj::LogSeverity::INFO);
  }
  kj::Maybe<JsonLogger> jsonLogger;
  bool structured = structuredLogging(config);
  if (structured) {
    jsonLogger.emplace();
  }
  return run_pending_command(kj::mv(command), kj::mv(config), structured);
}

kj::Own<WorkerFactory> bootstrap(kj_rs_tokio::TokioAsyncIoContext& loop,
    ::rust::Vec<uint64_t> config,
    const ServeOrTestOptions& options,
    kj::Maybe<const TestOptions&> test) {
  auto bootstrap = kj::heap<Bootstrap>(loop, config, options, test);
  auto factory = bootstrap->makeFactory(kj::mv(config), options, test);
  return factory.attach(kj::mv(bootstrap));
}

void cli_exit(int32_t code) {
  _exit(code);
}

void kj_log(uint8_t severity, ::rust::Str file, uint32_t line, ::rust::Str message) {
  auto kjSeverity = static_cast<kj::LogSeverity>(severity);
  if (!kj::_::Debug::shouldLog(kjSeverity)) return;
  // The file name is kept alive for the call only; KJ formats the line synchronously.
  auto fileStr = kj::str(file);
  kj::_::Debug::log(fileStr.cStr(), static_cast<int>(line), kjSeverity, "", kj::str(message));
}

void json_log_to_stderr(uint8_t severity, ::rust::Str file, uint32_t line, ::rust::Str message) {
  auto json = buildJsonLogMessage(static_cast<kj::LogSeverity>(severity), kj::str(file).cStr(),
      static_cast<int>(line), 0, kj::str(message));
  kj::FdOutputStream(STDERR_FILENO).write({json.asBytes(), "\n"_kj.asBytes()});
}

}  // namespace workerd::server::cli
