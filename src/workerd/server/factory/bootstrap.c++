// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "bootstrap.h"

#include "per-process.h"
#include "worker-factory.h"

#include <workerd/io/compatibility-date.capnp.h>
#include <workerd/io/compatibility-date.h>
#include <workerd/io/release-version.embed.h>
#include <workerd/server/json-logger.h>
#include <workerd/server/server/bridge.rs.h>
#include <workerd/server/workerd.capnp.h>
#include <workerd/util/entropy.h>

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
#include <fcntl.h>
#include <unistd.h>
#endif

#if _WIN32
#include <windows.h>

#include <kj/win32-api-version.h>
#include <kj/windows-sanity.h>
#endif

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

bool structuredLogging(const ::rust::Vec<uint64_t>& configWords) {
  capnp::FlatArrayMessageReader reader(asWords(kj::from<Rust>(configWords)), CONFIG_READER_OPTIONS);
  auto config = reader.getRoot<config::Config>();
  return config.hasLogging() ? config.getLogging().getStructuredLogging()
                             : config.getStructuredLogging();
}

// The I/O of one factory over the Rust-owned loop, attached to the factory. Members are declared
// in construction order, which is the reverse of destruction.
class LoopIo {
 public:
  explicit LoopIo(kj_rs_tokio::TokioAsyncIoContext& loop)
      : timer(loop.getTimer()),
        monotonicClock(kj::systemPreciseMonotonicClock()),
        provider(loop.getTimer()) {}
  KJ_DISALLOW_COPY_AND_MOVE(LoopIo);

  FactoryIo get() {
    return FactoryIo{
      .timer = timer,
      .monotonicClock = monotonicClock,
      .network = provider.getNetwork(),
      .entropySource = entropySource,
      .fs = *fs,
    };
  }

  kj::Filesystem& getFilesystem() {
    return *fs;
  }

 private:
  kj::Own<kj::Filesystem> fs = kj::newDiskFilesystem();
  EntropySourceImpl entropySource;
  kj::Timer& timer;
  // The loop's monotonic clock, consistent with `timer`.
  const kj::MonotonicClock& monotonicClock;
  // The tokio-backed kj::Network over the Runtime's loop.
  kj_rs_io::TokioAsyncIoProvider provider;
};

// The `serve` / `test` options that apply to the process.
void setUpServeOrTestProcess(
    ProcessState::Options& process, const ServeOrTestOptions& serveOrTest) {
  KJ_IF_SOME(path, serveOrTest.perfetto_trace_path) {
    process.perfettoTracePath = kj::str(path);
  }
  KJ_IF_SOME(categories, serveOrTest.perfetto_trace_categories) {
    process.perfettoTraceCategories = kj::str(categories);
  }
  KJ_IF_SOME(path, serveOrTest.async_trace_path) {
    process.asyncTracePath = kj::str(path);
  }
}

// The `test` options that apply to the process.
void setUpTestProcess(ProcessState::Options& process, const TestOptions& testOptions) {
  // Always turn on info logging when running tests so that uncaught exceptions are displayed.
  // TODO(beta): This can be removed once we improve our error logging story.
  process.infoLogging = !testOptions.no_verbose;
  process.predictable = testOptions.predictable;
  process.gcStress = testOptions.gc_stress;
  process.allAutogates = testOptions.all_autogates;
}

// The `bench` options that apply to the process. Bench mode is on before any worker compiles.
void setUpBenchProcess(ProcessState::Options& process, const BenchOptions& benchOptions) {
  process.benchMode = true;
  // As for tests, so that uncaught exceptions are displayed.
  process.infoLogging = !benchOptions.no_verbose;
  process.allAutogates = benchOptions.all_autogates;
}

void applyServeOrTestOptions(
    WorkerFactory::Options& options, const ServeOrTestOptions& serveOrTest, kj::Filesystem& fs) {
  KJ_IF_SOME(frames, serveOrTest.async_trace_stacks) {
    options.asyncTraceStackDepth = frames;
  }
  options.asyncTracePromises = serveOrTest.async_trace_promises;
  options.experimental = serveOrTest.experimental;
  auto& python = options.pythonConfig;
  KJ_IF_SOME(path, serveOrTest.pyodide_bundle_disk_cache_dir) {
    // Resolved as kj resolves paths, so `..` is taken lexically (`file/..` names the file's
    // directory).
    auto dir = fs.getCurrentPath().eval(kj::str(path)).toNativeString(true);
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
    python.snapshotDirectory =
        fs.getRoot().tryOpenSubdir(fs.getCurrentPath().eval(kj::str(path)), kj::WriteMode::MODIFY);
  }
}

// The `test` options that apply to a factory.
void applyTestOptions(WorkerFactory::Options& options, const TestOptions& testOptions) {
  KJ_IF_SOME(compatDate, testOptions.compat_date) {
    options.testCompatibilityDateOverride = kj::str(compatDate);
  }
  options.syncStorage = false;
}

// The `bench` options that apply to a factory.
void applyBenchOptions(WorkerFactory::Options& options, const BenchOptions& benchOptions) {
  KJ_IF_SOME(compatDate, benchOptions.compat_date) {
    options.testCompatibilityDateOverride = kj::str(compatDate);
  }
  KJ_IF_SOME(compatDate, benchOptions.default_compat_date) {
    options.defaultCompatibilityDate = kj::str(compatDate);
  }
  options.tailWorkers = benchOptions.trace;
}

kj::Arc<ProcessState> startProcess(const ::rust::Vec<uint64_t>& config,
    const ServeOrTestOptions& serveOrTest,
    kj::Maybe<const TestOptions&> test,
    kj::Maybe<const BenchOptions&> bench) {
  capnp::FlatArrayMessageReader reader(asWords(kj::from<Rust>(config)), CONFIG_READER_OPTIONS);
  auto root = reader.getRoot<config::Config>();
  ProcessState::Options options{
    .autogates = KJ_MAP(gate, root.getAutogates()) { return kj::str(gate); },
    .v8Flags = KJ_MAP(flag, root.getV8Flags()) { return kj::str(flag); },
  };
  setUpServeOrTestProcess(options, serveOrTest);
  KJ_IF_SOME(t, test) {
    setUpTestProcess(options, t);
  }
  KJ_IF_SOME(b, bench) {
    setUpBenchProcess(options, b);
  }
  return ProcessState::start(options);
}

kj::Own<WorkerFactory> newFactory(const ProcessState& process,
    kj_rs_tokio::TokioAsyncIoContext& loop,
    ::rust::Vec<uint64_t> config,
    const ServeOrTestOptions& serveOrTest,
    kj::Maybe<const TestOptions&> test,
    kj::Maybe<const BenchOptions&> bench) {
  auto io = kj::heap<LoopIo>(loop);
  auto options = kj::heap<WorkerFactory::Options>();
  options->loggingOptions = Worker::LoggingOptions(Worker::ConsoleMode::STDOUT);
  applyServeOrTestOptions(*options, serveOrTest, io->getFilesystem());
  KJ_IF_SOME(t, test) {
    applyTestOptions(*options, t);
  }
  KJ_IF_SOME(b, bench) {
    applyBenchOptions(*options, b);
  }
  // Destroyed in this order: the factory, its reference to the process, then its I/O.
  return newWorkerFactory(process, io->get(), kj::mv(options), kj::mv(config)).attach(kj::mv(io));
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
  bool structured = structuredLogging(config);
  if (structured) {
    jsonLogger.emplace();
  }
  return run_pending_command(kj::mv(command), kj::mv(config), structured);
}

kj::Own<WorkerFactory> bootstrap(kj_rs_tokio::TokioAsyncIoContext& loop,
    ::rust::Vec<uint64_t> config,
    const ServeOrTestOptions& options,
    kj::Maybe<const TestOptions&> test,
    kj::Maybe<const BenchOptions&> bench) {
  auto process = startProcess(config, options, test, bench);
  // The factory holds the process, so the process goes with it.
  return newFactory(*process, loop, kj::mv(config), options, test, bench);
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

kj::Own<ThreadLogger> thread_logger() {
  return kj::heap<ThreadLogger>(kj::getExceptionCallback().getThreadInitializer());
}

void run_with_logger(kj::Own<ThreadLogger> logger, ::rust::Box<ThreadBody> body) {
  logger->initializer([&body]() { thread_body_run(kj::mv(body)); });
}

void json_log_to_stderr(uint8_t severity, ::rust::Str file, uint32_t line, ::rust::Str message) {
  auto json = buildJsonLogMessage(static_cast<kj::LogSeverity>(severity), kj::str(file).cStr(),
      static_cast<int>(line), 0, kj::str(message));
  kj::FdOutputStream(STDERR_FILENO).write({json.asBytes(), "\n"_kj.asBytes()});
}

void write_stderr_nonblocking(::rust::Str text) {
#if !_WIN32
  int flags = fcntl(STDERR_FILENO, F_GETFL);
  if (flags < 0 || fcntl(STDERR_FILENO, F_SETFL, flags | O_NONBLOCK) < 0) return;
  [[maybe_unused]] auto written = write(STDERR_FILENO, text.data(), text.size());
  // The open file description may be shared with other processes, such as the supervisor's
  // terminal or pipe, which expect it to block.
  fcntl(STDERR_FILENO, F_SETFL, flags);
#endif
}

}  // namespace workerd::server::cli
