// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "cli-main.h"

#include "server.h"

#include <workerd/io/compatibility-date.capnp.h>
#include <workerd/io/compatibility-date.h>
#include <workerd/io/release-version.embed.h>
#include <workerd/jsg/setup.h>
#include <workerd/server/cli/bridge.rs.h>
#include <workerd/server/json-logger.h>
#include <workerd/server/v8-platform-impl.h>
#include <workerd/server/workerd.capnp.h>
#include <workerd/util/autogate.h>
#include <workerd/util/entropy.h>

#include <kj-rs-io/async-io.h>
#include <kj-rs/kj-rs.h>

#include <capnp/message.h>
#include <capnp/serialize.h>
#include <kj/filesystem.h>

#include <csignal>
#include <cstdio>

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

constexpr capnp::ReaderOptions CONFIG_READER_OPTIONS = {
  .traversalLimitInWords = kj::maxValue
  // Configs can legitimately be very large and are not malicious, so use an effectively-infinite
  // traversal limit.
};

// =======================================================================================

class CliMain {
 public:
  CliMain(StructuredLoggingProcessContext& context, ::rust::Box<Process> processParam)
      : context(context),
        process(kj::mv(processParam)),
        server(kj::heap<Server>(*fs,
            io.provider->getTimer(),
            kj::systemPreciseMonotonicClock(),
            io.provider->getNetwork(),
            entropySource,
            Worker::LoggingOptions(Worker::ConsoleMode::STDOUT),
            [&](kj::String error) {
              if (!process->is_watching()) {
                // TODO(someday): Don't just fail on the first error, keep going in order to report
                //   additional errors. The tricky part is we don't currently have any signal of when
                //   the server has completely finished loading, and also we probably don't want to
                //   accept any connections on any of the sockets if the server is partially broken.
                context.exitError(error);
              } else {
                // In --watch mode, we don't want to exit from errors, we want to wait until things
                // change. It's OK if we try to serve requests despite brokenness since this is a
                // development server.
                hadErrors = true;
                context.error(error);
              }
            },
            [&](kj::String warning) { context.warning(warning); })) {}

  // `message` is an encoded config (segment table, then segments); this object owns it for the
  // run.
  void loadConfig(::rust::Vec<uint64_t> message) {
    configMessage = kj::mv(message);
    auto words = kj::arrayPtr(
        reinterpret_cast<const capnp::word*>(configMessage.data()), configMessage.size());
    configReader = kj::heap<capnp::FlatArrayMessageReader>(words, CONFIG_READER_OPTIONS);
    config = configReader->getRoot<config::Config>();
    util::Autogate::initAutogate(config.getAutogates());
  }

  void applyServeOrTestOptions(const ServeOrTestOptions& options) {
    for (auto& o: options.directory_overrides) {
      server->overrideDirectory(kj::str(o.name), kj::str(o.value));
    }
    for (auto& o: options.external_overrides) {
      server->overrideExternal(kj::str(o.name), kj::str(o.value));
    }
    KJ_IF_SOME(addr, options.inspector_addr) {
      server->enableInspector(kj::str(addr));
    }
    KJ_IF_SOME(path, options.perfetto_trace_path) {
#ifdef WORKERD_USE_PERFETTO
      perfettoTraceDestination = kj::str(path);
      perfettoTraceCategories = options.perfetto_trace_categories.map(
          [](const ::rust::String& categories) { return kj::str(categories); });
#else
      KJ_UNIMPLEMENTED("perfetto tracing is not supported by this build", kj::str(path));
#endif
    }
    if (options.experimental) {
      server->allowExperimental();
    }
    KJ_IF_SOME(path, options.pyodide_package_disk_cache_dir) {
      // The command line checked that the directory exists.
      auto dir = KJ_REQUIRE_NONNULL(
          openWritableDirectory(kj::str(path)), "package disk cache dir must exist");
      server->setPackageDiskCacheRoot(kj::mv(dir));
    }
    KJ_IF_SOME(path, options.pyodide_bundle_disk_cache_dir) {
      server->setPyodideDiskCacheRoot(openWritableDirectory(kj::str(path)));
    }
    if (options.python_save_snapshot) {
      server->setPythonCreateSnapshot();
    }
    if (options.python_save_baseline_snapshot) {
      server->setPythonCreateBaselineSnapshot();
    }
    KJ_IF_SOME(path, options.python_load_snapshot) {
      server->setPythonLoadSnapshot(kj::str(path));
    }
    KJ_IF_SOME(path, options.python_snapshot_dir) {
      server->setPythonSnapshotDirectory(openWritableDirectory(kj::str(path)));
    }
  }

  void applyServeOptions(const ServeOptions& options) {
    for (auto& o: options.socket_addr_overrides) {
      server->overrideSocket(kj::str(o.name), kj::str(o.value));
    }
    for (auto& o: options.socket_fd_overrides) {
      server->overrideSocket(kj::str(o.name),
          io.lowLevelProvider->wrapListenSocketFd(
              static_cast<kj::LowLevelAsyncIoProvider::Fd>(o.fd),
              kj::LowLevelAsyncIoProvider::TAKE_OWNERSHIP));
    }
    KJ_IF_SOME(fd, options.control_fd) {
      server->enableControl(fd);
    }
    KJ_IF_SOME(addr, options.debug_port) {
      server->enableDebugPort(kj::str(addr));
    }
  }

  void serve() {
    serveImpl([&](jsg::V8System& v8System) {
#if _WIN32
      return server->run(v8System, config);
#else
      // Gracefully drain when SIGTERM is received.
      return server->run(v8System, config, kj_rs_io::onSignal(SIGTERM));
#endif
    });
  }

  void test(const TestOptions& options) {
    if (!options.no_verbose) {
      // Always turn on info logging when running tests so that uncaught exceptions are displayed.
      // TODO(beta): This can be removed once we improve our error logging story.
      kj::_::Debug::setLogLevel(kj::LogSeverity::INFO);
    }
    if (options.predictable) {
      setPredictableModeForTest();
    }
    if (options.gc_stress) {
      setGcStressModeForTest();
    }
    if (options.all_autogates) {
      util::Autogate::initAllAutogates();
    }

    KJ_IF_SOME(compatDate, options.compat_date) {
      server->setTestCompatibilityDateOverride(kj::str(compatDate));
    }

    // Enable loopback sockets in tests only.
    kj::downcast<kj_rs_io::TokioNetwork>(io.provider->getNetwork()).enableLoopback();

    auto servicePattern = kj::str("*");
    KJ_IF_SOME(pattern, options.service_pattern) {
      servicePattern = kj::str(pattern);
    }
    auto entrypointPattern = kj::str("*");
    KJ_IF_SOME(pattern, options.entrypoint_pattern) {
      entrypointPattern = kj::str(pattern);
    }

    serveImpl([&](jsg::V8System& v8System) {
      return server->test(v8System, config, servicePattern, entrypointPattern)
          .then([this](bool result) -> kj::Promise<void> {
        if (!result) {
          context.error("Tests failed!");
        }

        if (!process->is_watching()) {
          return kj::READY_NOW;
        } else {
          // Pause forever waiting for watcher.
          return kj::NEVER_DONE;
        }
      });
    });
  }

 private:
  StructuredLoggingProcessContext& context;
  ::rust::Box<Process> process;

  kj::Own<kj::Filesystem> fs = kj::newDiskFilesystem();
  kj::AsyncIoContext io = kj::setupAsyncIo();
  EntropySourceImpl entropySource;

  // The reader borrows the message, so the message is declared first.
  ::rust::Vec<uint64_t> configMessage;
  kj::Own<capnp::FlatArrayMessageReader> configReader;
  config::Config::Reader config;

#ifdef WORKERD_USE_PERFETTO
  kj::Maybe<kj::String> perfettoTraceDestination;
  kj::Maybe<kj::String> perfettoTraceCategories;
#endif

  kj::Own<Server> server;

  // Set by the Server's error callback under --watch, where errors don't exit.
  bool hadErrors = false;

  kj::Maybe<kj::Own<const kj::Directory>> openWritableDirectory(kj::StringPtr pathStr) {
    return fs->getRoot().tryOpenSubdir(fs->getCurrentPath().eval(pathStr), kj::WriteMode::MODIFY);
  }

  template <typename Func>
  void serveImpl(Func&& func) noexcept {
#ifdef WORKERD_USE_PERFETTO
    kj::Maybe<PerfettoSession> maybePerfettoSession;
    KJ_IF_SOME(dest, perfettoTraceDestination) {
      maybePerfettoSession =
          PerfettoSession(dest, kj::mv(perfettoTraceCategories).orDefault(kj::String()));
    }
#endif
    TRACE_EVENT("workerd", "serveImpl()");

    // Configure structured logging in the process context
    if (config.hasLogging() ? config.getLogging().getStructuredLogging()
                            : config.getStructuredLogging()) {
      context.enableStructuredLogging();
    }

    auto platform = jsg::defaultPlatform(0);
    WorkerdPlatform v8Platform(*platform);
    jsg::V8System v8System(v8Platform,
        KJ_MAP(flag, config.getV8Flags()) -> kj::StringPtr { return flag; }, platform.get());
    auto promise = func(v8System);
    if (process->is_watching()) {
      promise =
          promise.exclusiveJoin(wait_for_changes(*process).then([this]() { process->reload(); }));
    }
    promise.wait(io.waitScope);
#ifdef WORKERD_USE_PERFETTO
    KJ_IF_SOME(perfettoSession, maybePerfettoSession) {
      auto dropMe = kj::mv(perfettoSession);
      maybePerfettoSession = kj::none;
    }
#endif

    if (getenv("KJ_CLEAN_SHUTDOWN") == nullptr) {
      context.exit();
    }

    // Server maintains a reference to the v8 platform. Clean up before destroying the platform.
    server = nullptr;
  }
};

// Runs `body` as the process's main function, with KJ's top-level exception handling and exit-code
// semantics. The command line was already parsed; runMainAndExit() only gets the program name.
template <typename Func>
int32_t run(const CommonOptions& common, ::rust::Box<Process> process, Func&& body) {
  auto programName = kj::str(common.program_name);
  StructuredLoggingProcessContext context(programName);
  CliMain main(context, kj::mv(process));

#if defined(WORKERD_FUZZILLI) && defined(__linux__)
  initSignalHandlers();
#endif

  char* argv[] = {programName.begin(), nullptr};
  return kj::runMainAndExit(context, [&](kj::StringPtr, kj::ArrayPtr<const kj::StringPtr>) {
    if (common.verbose) {
      context.increaseLoggingVerbosity();
    }
    body(main);
  }, 1, argv);
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

::rust::String pyodide_lock() {
  capnp::MallocMessageBuilder message;
  // TODO(EW-8977): Implement option to specify python worker flags.
  auto features = message.getRoot<CompatibilityFlags>();
  features.setPythonWorkers(true);
  auto pythonRelease = KJ_REQUIRE_NONNULL(getPythonSnapshotRelease(features));
  auto lock = KJ_REQUIRE_NONNULL(api::pyodide::getPyodideLock(pythonRelease));
  return lock.as<RustCopyUncheckedUtf8>();
}

int32_t run_serve(const CommonOptions& common,
    ::rust::Vec<uint64_t> config,
    const ServeOrTestOptions& serveOrTest,
    const ServeOptions& serve,
    ::rust::Box<Process> process) {
  return run(common, kj::mv(process), [&](CliMain& main) {
    main.applyServeOrTestOptions(serveOrTest);
    main.applyServeOptions(serve);
    main.loadConfig(kj::mv(config));
    main.serve();
  });
}

int32_t run_test(const CommonOptions& common,
    ::rust::Vec<uint64_t> config,
    const ServeOrTestOptions& serveOrTest,
    const TestOptions& test,
    ::rust::Box<Process> process) {
  return run(common, kj::mv(process), [&](CliMain& main) {
    main.applyServeOrTestOptions(serveOrTest);
    main.loadConfig(kj::mv(config));
    main.test(test);
  });
}

}  // namespace workerd::server::cli
