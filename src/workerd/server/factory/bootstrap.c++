// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "bootstrap.h"

#include "worker-factory.h"

#include <workerd/io/async-trace.h>
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
#include <v8-initialization.h>

#include <capnp/message.h>
#include <capnp/serialize.h>
#include <kj/encoding.h>
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
//
// glibc's resolver keeps per-thread state, including a 28-byte `sockaddr_in6` per name server
// that `__res_context_send` allocates, and frees it as the thread exits, after the thread's
// pthread key destructors. DNS lookups run on the blocking pool of a loop thread's tokio runtime,
// which kj-rs-tokio shuts down without joining (see `EnteredRuntime`). So the process can exit
// while a pool thread is exiting: ASan's key destructor has already removed it from the threads
// LSan scans, but glibc has not yet freed its resolver state, and LSan reports that as a leak.
#if __has_feature(address_sanitizer)
extern "C" __attribute__((no_sanitize("address"))) __attribute__((visibility("default")))
__attribute__((used)) const char*
__lsan_default_suppressions() {
  return "leak:<unknown module>\n"
         "leak:__res_context_send\n";
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

// How the binary was built, for `workerd bench`'s report: a sanitizer's instrumentation and a
// debug build's checks make code slower than in a release build.
kj::StringPtr buildMode() {
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) ||                         \
    __has_feature(memory_sanitizer) || __has_feature(undefined_behavior_sanitizer)
  return "sanitizer"_kj;
#elifdef NDEBUG
  return "release"_kj;
#else
  return "debug"_kj;
#endif
}

bool structuredLogging(kj::ArrayPtr<const uint64_t> configWords) {
  capnp::FlatArrayMessageReader reader(asWords(configWords), CONFIG_READER_OPTIONS);
  auto config = reader.getRoot<config::Config>();
  return config.hasLogging() ? config.getLogging().getStructuredLogging()
                             : config.getStructuredLogging();
}

// What the process needs around the Rust server (see the bridge), set up once and attached to the
// `SharedFactory` it builds, so that it outlives every loop's factory. Members are declared in
// construction order, which is the reverse of destruction.
class ProcessBootstrap {
 public:
  ProcessBootstrap(kj::ArrayPtr<const uint64_t> configWords,
      const ServeOrTestOptions& serveOrTest,
      kj::Maybe<const TestOptions&> test,
      kj::Maybe<const BenchOptions&> bench) {
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
    KJ_IF_SOME(path, serveOrTest.async_trace_path) {
      asyncTraceWriter = AsyncTraceWriter::open(kj::str(path), RELEASE_VERSION);
    }
    TRACE_EVENT("workerd", "Bootstrap()");

    // The process is set up for this config and these options: autogates, the test and bench
    // modes, and V8 with its flags (V8 reads the test modes as it starts).
    capnp::FlatArrayMessageReader reader(asWords(configWords), CONFIG_READER_OPTIONS);
    auto config = reader.getRoot<config::Config>();
    util::Autogate::initAutogate(config.getAutogates());
    KJ_IF_SOME(t, test) {
      setUpTestProcess(t);
    }
    KJ_IF_SOME(b, bench) {
      setUpBenchProcess(b);
    }

    platform = jsg::defaultPlatform(0);
    v8Platform = kj::heap<WorkerdPlatform>(*platform);
    v8System = kj::heap<jsg::V8System>(*v8Platform,
        KJ_MAP(flag, config.getV8Flags()) -> kj::StringPtr { return flag; }, platform.get());
  }

  // Ends the --async-trace file. Every factory is destroyed first, ending every context, so the
  // exit line follows their ends.
  ~ProcessBootstrap() noexcept(false) {
    KJ_IF_SOME(writer, asyncTraceWriter) {
      writer->finish();
    }
  }

  KJ_DISALLOW_COPY_AND_MOVE(ProcessBootstrap);

  // The shared factory over `configWords`, which it owns for the run, and over `self`.
  static kj::Arc<SharedFactory> makeShared(kj::Own<ProcessBootstrap> self,
      ::rust::Vec<uint64_t> configWords,
      const ServeOrTestOptions& serveOrTest,
      kj::Maybe<const TestOptions&> test,
      kj::Maybe<const BenchOptions&> bench) {
    auto options = kj::heap<WorkerFactory::Options>();
    options->loggingOptions = Worker::LoggingOptions(Worker::ConsoleMode::STDOUT);
    self->applyServeOrTestOptions(*options, serveOrTest);
    KJ_IF_SOME(t, test) {
      applyTestOptions(*options, t);
    }
    KJ_IF_SOME(b, bench) {
      applyBenchOptions(*options, b);
    }
    auto& v8System = *self->v8System;
    auto& entropySource = self->entropySource;
    auto& fs = *self->fs;
    return kj::arc<SharedFactory>(
        v8System, entropySource, fs, kj::mv(options), kj::mv(configWords), kj::mv(self));
  }

 private:
  kj::Own<kj::Filesystem> fs = kj::newDiskFilesystem();
  EntropySourceImpl entropySource;

#ifdef WORKERD_USE_PERFETTO
  kj::Maybe<PerfettoSession> perfettoSession;
#endif

  // Set by --async-trace. The factories' isolates write to it, so it outlives them.
  kj::Maybe<kj::Own<AsyncTraceWriter>> asyncTraceWriter;

  kj::Own<v8::Platform> platform;
  kj::Own<WorkerdPlatform> v8Platform;
  kj::Own<jsg::V8System> v8System;

  kj::Maybe<kj::Own<const kj::Directory>> openWritableDirectory(kj::StringPtr pathStr) {
    return fs->getRoot().tryOpenSubdir(fs->getCurrentPath().eval(pathStr), kj::WriteMode::MODIFY);
  }

  void applyServeOrTestOptions(
      WorkerFactory::Options& options, const ServeOrTestOptions& serveOrTest) {
    KJ_IF_SOME(writer, asyncTraceWriter) {
      options.asyncTraceWriter = *writer;
    }
    KJ_IF_SOME(frames, serveOrTest.async_trace_stacks) {
      options.asyncTraceStackDepth = frames;
    }
    options.asyncTracePromises = serveOrTest.async_trace_promises;
    options.experimental = serveOrTest.experimental;
    auto& python = options.pythonConfig;
    KJ_IF_SOME(path, serveOrTest.pyodide_package_disk_cache_dir) {
      // The command line checked that the directory exists.
      python.packageDiskCacheRoot = KJ_REQUIRE_NONNULL(
          openWritableDirectory(kj::str(path)), "package disk cache dir must exist");
    }
    KJ_IF_SOME(path, serveOrTest.pyodide_bundle_disk_cache_dir) {
      // Resolved as kj resolves paths, so `..` is taken lexically (`file/..` names the file's
      // directory).
      auto dir = fs->getCurrentPath().eval(kj::str(path)).toNativeString(true);
      // The server takes the directory as UTF-8, which the working directory need not be.
      KJ_REQUIRE(!kj::encodeUtf16(dir).hadErrors,
          "--pyodide-bundle-disk-cache-dir does not resolve to a UTF-8 path", dir);
      options.pyodideBundleDiskCacheDir = kj::mv(dir);
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
    options.syncStorage = false;
  }

  // The `bench` options that apply to the process. Bench mode is on before any worker compiles.
  static void setUpBenchProcess(const BenchOptions& benchOptions) {
    setBenchMode();
    if (!benchOptions.no_verbose) {
      // As for tests, so that uncaught exceptions are displayed.
      kj::_::Debug::setLogLevel(kj::LogSeverity::INFO);
    }
    if (benchOptions.all_autogates) {
      util::Autogate::initAllAutogates();
    }
  }

  // The `bench` options that apply to a factory.
  static void applyBenchOptions(WorkerFactory::Options& options, const BenchOptions& benchOptions) {
    KJ_IF_SOME(compatDate, benchOptions.compat_date) {
      options.testCompatibilityDateOverride = kj::str(compatDate);
    }
    KJ_IF_SOME(compatDate, benchOptions.default_compat_date) {
      options.defaultCompatibilityDate = kj::str(compatDate);
    }
    options.tailWorkers = benchOptions.trace;
  }
};

// What one loop thread's factory needs of its loop: the tokio-backed `kj::Network` over the
// loop's timer. Attached to the factory, so destroyed after it.
struct LoopBootstrap {
  explicit LoopBootstrap(kj_rs_tokio::TokioAsyncIoContext& loop): provider(loop.getTimer()) {}
  kj_rs_io::TokioAsyncIoProvider provider;
};

// A factory on `loop`, which is the current thread's.
kj::Own<WorkerFactory> newLoopFactory(
    kj::Arc<SharedFactory> shared, kj_rs_tokio::TokioAsyncIoContext& loop, PrimaryLoop primary) {
  auto loopBootstrap = kj::heap<LoopBootstrap>(loop);
  auto factory = kj::heap<WorkerFactory>(kj::mv(shared), loop.getTimer(),
      kj::systemPreciseMonotonicClock(), loopBootstrap->provider.getNetwork(), primary);
  return factory.attach(kj::mv(loopBootstrap));
}

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
  bool structured = structuredLogging(kj::from<Rust>(config));
  if (structured) {
    jsonLogger.emplace();
  }
  return run_pending_command(kj::mv(command), kj::mv(config), structured);
}

void with_loop_thread_context(const SharedFactory& shared, ::rust::Box<LoopThread> thread) {
  // An exception callback belongs to the thread whose stack it is on, so a loop thread gets a
  // JSON logger of its own.
  kj::Maybe<JsonLogger> jsonLogger;
  if (structuredLogging(shared.getConfigWords())) {
    jsonLogger.emplace();
  }
  run_loop_thread(kj::mv(thread));
}

kj::Arc<SharedFactory> bootstrap_process(::rust::Vec<uint64_t> config,
    const ServeOrTestOptions& options,
    kj::Maybe<const TestOptions&> test,
    kj::Maybe<const BenchOptions&> bench) {
  auto process = kj::heap<ProcessBootstrap>(kj::from<Rust>(config), options, test, bench);
  return ProcessBootstrap::makeShared(kj::mv(process), kj::mv(config), options, test, bench);
}

kj::Own<WorkerFactory> bootstrap_loop(
    const SharedFactory& shared, kj_rs_tokio::TokioAsyncIoContext& loop) {
  return newLoopFactory(shared.addRef(), loop, PrimaryLoop::NO);
}

kj::Own<WorkerFactory> bootstrap(kj_rs_tokio::TokioAsyncIoContext& loop,
    ::rust::Vec<uint64_t> config,
    const ServeOrTestOptions& options,
    kj::Maybe<const TestOptions&> test,
    kj::Maybe<const BenchOptions&> bench) {
  return newLoopFactory(
      bootstrap_process(kj::mv(config), options, test, bench), loop, PrimaryLoop::YES);
}

BuildInfo build_info() {
  return BuildInfo{
    .workerd_version = RELEASE_VERSION.as<RustCopyUncheckedUtf8>(),
    .build_mode = buildMode().as<RustCopyUncheckedUtf8>(),
    .v8_version = kj::StringPtr(v8::V8::GetVersion()).as<RustCopyUncheckedUtf8>(),
  };
}

::rust::String newest_compat_date() {
  return newestCompatDateStr().as<RustCopyUncheckedUtf8>();
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
