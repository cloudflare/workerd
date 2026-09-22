// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "schema-file.h"
#include "server.h"

#include <workerd/api/unsafe.h>
#include <workerd/io/compatibility-date.capnp.h>
#include <workerd/io/compatibility-date.h>
#include <workerd/io/release-version.embed.h>
#include <workerd/jsg/setup.h>
#include <workerd/server/json-logger.h>
#include <workerd/server/v8-platform-impl.h>
#include <workerd/server/workerd.capnp.h>
#include <workerd/util/autogate.h>
#include <workerd/util/entropy.h>

#include <errno.h>
#include <fcntl.h>
#include <kj-rs-io/async-io.h>
#ifdef __linux__
#include <sys/mman.h>
#include <sys/stat.h>
#endif

#include <capnp/dynamic.h>
#include <capnp/message.h>
#include <capnp/schema-parser.h>
#include <capnp/serialize.h>
#include <kj/encoding.h>
#include <kj/filesystem.h>
#include <kj/main.h>

#if _WIN32
#include <windows.h>
#include <winsock2.h>

#include <kj/win32-api-version.h>
#include <kj/windows-sanity.h>

#include <iostream>
#else
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#ifdef __GLIBC__
#include <sys/auxv.h>
#endif

#ifdef __APPLE__
#include <crt_externs.h>
#include <libproc.h>
#define environ (*_NSGetEnviron())
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

namespace workerd::server {
namespace {

static kj::StringPtr getVersionString() {
  static const kj::String result = kj::str("workerd ", RELEASE_VERSION);
  return result;
}

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
// Some generic CLI helpers so that we can throw exceptions rather than return
// kj::MainBuilder::Validity. Honestly I do not know how people put up with patterns like
// Result<T, E>, it seems like such a slog.

class CliError {
 public:
  CliError(kj::String description): description(kj::mv(description)) {}
  kj::String description;
};

template <typename Func>
auto cliMethod(Func&& func) {
  return [func = kj::fwd<Func>(func)](auto&&... params) mutable -> kj::MainBuilder::Validity {
    try {
      func(kj::fwd<decltype(params)>(params)...);
      return true;
    } catch (CliError& e) {
      return kj::mv(e.description);
    }
  };
}

// Pass to MainBuilder when a function returning kj::MainBuilder::Validity is needed, implemented
// by a method of this class.
#define CLI_METHOD(name) cliMethod(KJ_BIND_METHOD(*this, name))

// Throws an exception that is caught and reported as a usage error.
#define CLI_ERROR(...) throw CliError(kj::str(__VA_ARGS__))

constexpr capnp::ReaderOptions CONFIG_READER_OPTIONS = {
  .traversalLimitInWords = kj::maxValue
  // Configs can legitimately be very large and are not malicious, so use an effectively-infinite
  // traversal limit.
};

// =======================================================================================

// =======================================================================================

class CliMain final: public SchemaFileImpl::ErrorReporter {
 public:
  CliMain(StructuredLoggingProcessContext& context, char** argv)
      : context(context),
        argv(argv),
        server(kj::heap<Server>(*fs,
            io.provider->getTimer(),
            kj::systemPreciseMonotonicClock(),
            io.provider->getNetwork(),
            entropySource,
            Worker::LoggingOptions(Worker::ConsoleMode::STDOUT),
            [&](kj::String error) {
              if (watcher == kj::none) {
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
            [&](kj::String warning) { context.warning(warning); })) {
    KJ_IF_SOME(e, exeInfo) {
      auto& exe = *e.file;
      auto size = exe.stat().size;
      KJ_ASSERT(size > sizeof(COMPILED_MAGIC_SUFFIX) + sizeof(uint64_t));
      kj::byte magic[sizeof(COMPILED_MAGIC_SUFFIX)]{};
      exe.read(size - sizeof(COMPILED_MAGIC_SUFFIX), magic);
      if (kj::arrayPtr(magic) == kj::asBytes(COMPILED_MAGIC_SUFFIX)) {
        // Oh! It appears we are running a compiled binary, it has a config appended to the end.
        uint64_t configSize;
        exe.read(size - sizeof(COMPILED_MAGIC_SUFFIX) - sizeof(uint64_t), kj::asBytes(configSize));
        KJ_ASSERT(size - sizeof(COMPILED_MAGIC_SUFFIX) - sizeof(uint64_t) >
            configSize * sizeof(capnp::word));
        size_t offset = size - sizeof(COMPILED_MAGIC_SUFFIX) - sizeof(uint64_t) -
            configSize * sizeof(capnp::word);

        auto mapping = exe.mmap(offset, configSize * sizeof(capnp::word));
        KJ_ASSERT(reinterpret_cast<uintptr_t>(mapping.begin()) % sizeof(capnp::word) == 0,
            "compiled-in config is not aligned correctly?");

        config = capnp::readMessageUnchecked<config::Config>(
            reinterpret_cast<const capnp::word*>(mapping.begin()));
        configOwner = kj::heap(kj::mv(mapping));
      }
    } else {
      context.warning(
          "Unable to find and open the program executable, so unable to determine if there is a "
          "compiled-in config file. Proceeding on the assumption that there is not.");
    }

    // We don't want to force people to specify top-level file IDs in `workerd` config files, as
    // those IDs would be totally irrelevant.
    schemaParser.setFileIdsRequired(false);
  }

  kj::MainFunc getMain() {
    if (config == kj::none) {
      return kj::MainBuilder(
          context, getVersionString(), "Runs the Workers JavaScript/Wasm runtime.")
          .addSubCommand("serve", KJ_BIND_METHOD(*this, getServe), "run the server")
          .addSubCommand(
              "compile", KJ_BIND_METHOD(*this, getCompile), "create a self-contained binary")
#ifdef WORKERD_FUZZILLI
          .addSubCommand("fuzzilli", KJ_BIND_METHOD(*this, getFuzz), "run reprl for fuzzing")
#endif
          .addSubCommand("test", KJ_BIND_METHOD(*this, getTest), "run unit tests")
          .addSubCommand("pyodide-lock", KJ_BIND_METHOD(*this, getPyodideLock),
              "outputs the package lock file used by Pyodide")
          .addSubCommand("make-pyodide-baseline-snapshot",
              KJ_BIND_METHOD(*this, getMakePyodideBaselineSnapshot),
              "Make a Pyodide baseline memory snapshot")
          .build();
      // TODO(someday):
      // "validate": Loads the config and parses all the code to report errors, but then exits
      //   without serving anything.
      // "explain": Produces human-friendly description of the config.
    } else {
      // We already have a config, meaning this must be a compiled binary.
      auto builder = kj::MainBuilder(context, getVersionString(),
          "Serve requests based on the compiled config.",
          "This binary has an embedded configuration.");
      return addServeOptions(builder);
    }
  }

  kj::MainBuilder& addConfigParsingOptionsNoConstName(kj::MainBuilder& builder) {
    return builder
        .addOptionWithArg({'I', "import-path"}, CLI_METHOD(addImportPath), "<dir>",
            "Add <dir> to the list of directories searched for non-relative "
            "imports in the config file (ones that start with a '/').")
        .addOption({'b', "binary"},
            [this]() {
      binaryConfig = true;
      return true;
    },
            "Specifies that the configuration file is an encoded binary Cap'n Proto "
            "message, rather than the usual text format. This is particularly useful when "
            "driving the server from higher-level tooling that automatically generates a "
            "config.")
        .expectArg("<config-file>", CLI_METHOD(parseConfigFile));
  }

  kj::MainBuilder& addConfigParsingOptions(kj::MainBuilder& builder) {
    return addConfigParsingOptionsNoConstName(builder).expectOptionalArg(
        "<const-name>", CLI_METHOD(setConstName));
  }

  kj::MainBuilder& addServeOrTestOptions(kj::MainBuilder& builder) {
    return builder
        .addOptionWithArg({'d', "directory-path"}, CLI_METHOD(overrideDirectory), "<name>=<path>",
            "Override the directory named <name> to point to <path> instead of the "
            "path specified in the config file.")
        .addOptionWithArg({'e', "external-addr"}, CLI_METHOD(overrideExternal), "<name>=<addr>",
            "Override the external service named <name> to connect to the address "
            "<addr> instead of the address specified in the config file.")
        .addOptionWithArg({'i', "inspector-addr"}, CLI_METHOD(enableInspector), "<addr>",
            "Enable the inspector protocol to connect to the address <addr>.")
#ifdef WORKERD_USE_PERFETTO
        // TODO(later): In the future, we might want to enable providing a perfetto
        // TraceConfig structure here rather than just the categories.
        .addOptionWithArg({"p", "perfetto-trace"}, CLI_METHOD(enablePerfetto),
            "<path>=<categories>", "Enable perfetto tracing output to the specified file.")
#endif
        .addOption({'w', "watch"}, CLI_METHOD(watch),
            "Watch configuration files (and server binary) and reload if they change. "
            "Useful for development, but not recommended in production.")
        .addOption({"experimental"},
            [this]() {
      server->allowExperimental();
      return true;
    },
            "Permit the use of experimental features which may break backwards "
            "compatibility in a future release.")
        .addOptionWithArg({"pyodide-package-disk-cache-dir"}, CLI_METHOD(setPackageDiskCacheDir),
            "<path>",
            "Use <path> as a disk cache to avoid repeatedly fetching packages from the internet. ")
        .addOptionWithArg({"pyodide-bundle-disk-cache-dir"}, CLI_METHOD(setPyodideDiskCacheDir),
            "<path>",
            "Use <path> as a disk cache to avoid repeatedly fetching Pyodide bundles from the internet. ")
        .addOption({"python-save-snapshot"},
            [this]() {
      server->setPythonCreateSnapshot();
      return true;
    }, "Save a dedicated snapshot to the disk cache")
        .addOption({"python-save-baseline-snapshot"},
            [this]() {
      server->setPythonCreateBaselineSnapshot();
      return true;
    }, "Save a baseline snapshot to the disk cache")
        .addOptionWithArg({"python-load-snapshot"}, CLI_METHOD(setPythonLoadSnapshot), "<path>",
            "Load a snapshot from the python snapshot directory.")
        .addOptionWithArg({"python-snapshot-dir"}, CLI_METHOD(setPythonSnapshotDirectory), "<path>",
            "Set the snapshot snapshot directory.");
  }

  kj::MainFunc addServeOptions(kj::MainBuilder& builder) {
    return addServeOrTestOptions(builder)
        .addOptionWithArg({'s', "socket-addr"}, CLI_METHOD(overrideSocketAddr), "<name>=<addr>",
            "Override the socket named <name> to bind to the address <addr> instead "
            "of the address specified in the config file.")
        .addOptionWithArg({'S', "socket-fd"}, CLI_METHOD(overrideSocketFd), "<name>=<fd>",
            "Override the socket named <name> to listen on the already-open socket "
            "descriptor <fd> instead of the address specified in the config file.")
        .addOptionWithArg({"control-fd"}, CLI_METHOD(enableControl), "<fd>",
            "Enable sending of control messages on descriptor <fd>. Currently this "
            "only reports the port each socket is listening on when ready.")
        .addOptionWithArg({"debug-port"}, CLI_METHOD(enableDebugPort), "<addr>",
            "Listen on the specified address for debug RPC connections. This exposes "
            "a privileged interface that allows access to all services in the process. "
            "For use by miniflare and local development only.")
        .callAfterParsing(CLI_METHOD(serve))
        .build();
  }

  kj::MainFunc getServe() {
    auto builder = kj::MainBuilder(context, getVersionString(), "Serve requests based on a config.",
        "Serves requests based on the configuration specified in <config-file>.");
    return addServeOptions(addConfigParsingOptions(builder));
  }

  kj::MainFunc getPyodideLock() {
    auto builder = kj::MainBuilder(
        context, getVersionString(), "Outputs the package lock file used by Pyodide.");
    return builder
        .callAfterParsing([]() -> kj::MainBuilder::Validity {
      static const PythonConfig config{
        .packageDiskCacheRoot = kj::none,
        .pyodideDiskCacheRoot = kj::none,
        .createSnapshot = false,
        .createBaselineSnapshot = false,
      };

      capnp::MallocMessageBuilder message;
      // TODO(EW-8977): Implement option to specify python worker flags.
      auto features = message.getRoot<CompatibilityFlags>();
      features.setPythonWorkers(true);
      auto pythonRelease = KJ_ASSERT_NONNULL(getPythonSnapshotRelease(features));

      auto lock = KJ_ASSERT_NONNULL(api::pyodide::getPyodideLock(pythonRelease));

      printf("%s\n", lock.cStr());
      fflush(stdout);
      return true;
    }).build();
  }

  kj::MainFunc getTest() {
    auto builder = kj::MainBuilder(context, getVersionString(), "Runs tests based on a config.",
        "Runs tests for services defined in <config-file>. <filter>, if given, specifies "
        "exactly which tests to run. It has one of the following formats:\n"
        "    <service-pattern>\n"
        "    <service-pattern>:<entrypoint-pattern>\n"
        "    <const-name>:<service-pattern>:<entrypoint-pattern>\n"
        "<service-pattern> is a glob pattern matching names of services which should be tested. "
        "If not specified, '*' is assumed (which matches all services). <entrypoint-pattern> "
        "is a glob pattern matching entrypoints within each service which should be tested; "
        "again, the default is '*'. <const-name> has the same meaning as for the `serve` "
        "command (this is rarely used).\n"
        "\n"
        "Tests can be defined by exporting a function called `test` instead of (or in addition "
        "to) `fetch`. Example:\n"
        "    export default {\n"
        "      async test(ctrl, env, ctx) {\n"
        "        if (1 + 1 != 2) {\n"
        "          throw new Error('math is broken!');\n"
        "        }\n"
        "      }\n"
        "    }\n"
        "The test passes if the test function completes without throwing. Multiple tests can "
        "be exported under different entrypoint names:\n"
        "    export let test1 = {\n"
        "      async test(ctrl, env, ctx) {\n"
        "        ...\n"
        "      }\n"
        "    }\n"
        "    export let test2 = {\n"
        "      async test(ctrl, env, ctx) {\n"
        "        ...\n"
        "      }\n"
        "    }\n");
    return addServeOrTestOptions(addConfigParsingOptionsNoConstName(builder))
        .addOption({"no-verbose"},
            [this]() {
      noVerbose = true;
      return true;
    },
            "Disable INFO-level logging for this test. Otherwise, INFO logging is enabled by "
            "default for tests in order to show uncaught exceptions, but it can be noisey.")
        .addOption({"predictable"},
            [this]() {
      predictable = true;
      return true;
    },
            "Enable predictable mode. This makes workerd behave more deterministically by using "
            "pre-set values instead of random data or timestamps to facilitate testing.")
        .addOption({"gc-stress"},
            [this]() {
      gcStress = true;
      return true;
    },
            "Force a full V8 GC at each awaitIo continuation. "
            "Detects KJ async objects on the JS heap without IoOwn wrapping. Very slow.")
        .addOption({"all-autogates"},
            [this]() {
      allAutogates = true;
      return true;
    },
            "Enable all autogates. This is useful for testing code paths that are guarded by "
            "autogates.")
        .addOptionWithArg({"compat-date"}, CLI_METHOD(setTestCompatDate), "<date>",
            "Set the compatibility date for all workers. When specified, workers must NOT "
            "specify compatibilityDate in the config. Use '0000-00-00' for oldest behavior "
            "or '9999-12-31' for newest behavior.")
        .expectOptionalArg("<filter>", CLI_METHOD(setTestFilter))
        .callAfterParsing(CLI_METHOD(test))
        .build();
  }

  kj::MainFunc getFuzz() {
    auto builder = kj::MainBuilder(context, getVersionString(),
        "Creates a custom signal handler and depending on the config leverages Stdin.reprl() to communicate with fuzzilli.");

    return addServeOrTestOptions(addConfigParsingOptionsNoConstName(builder))
        .callAfterParsing(CLI_METHOD(test))
        .build();
  }

  kj::MainFunc getCompile() {
    auto builder = kj::MainBuilder(context, getVersionString(),
        "Builds a self-contained binary from a config.",
        "This parses a config file in the same manner as the \"serve\" command, but instead "
        "of then running it, it outputs a new binary to stdout that embeds the config and all "
        "associated Worker code and data as one self-contained unit. This binary may then "
        "be executed on another system to run the config -- without any other files being "
        "present on that system.");
    return addConfigParsingOptions(builder)
        .addOption({"config-only"},
            [this]() {
      configOnly = true;
      return true;
    },
            "Only write the encoded binary config to stdout. Do not attach it to an executable. "
            "The encoded config can be used as input to the \"serve\" command, without the need "
            "for any other files to be present.")
        .callAfterParsing(CLI_METHOD(compile))
        .build();
  }

  kj::MainFunc getMakePyodideBaselineSnapshot() {
    server->allowExperimental();
    server->setPythonCreateBaselineSnapshot();
    auto builder =
        kj::MainBuilder(context, getVersionString(), "Make a Pyodide baseline memory snapshot", "");
    setPyodideDiskCacheDir(".");
    return builder.expectArg("<python-version>", CLI_METHOD(parsePythonCompatFlag))
        .expectArg("<output-directory>", CLI_METHOD(setPackageDiskCacheDir))
        .callAfterParsing(CLI_METHOD(test))
        .build();
  }

  void addImportPath(kj::StringPtr pathStr) {
    auto path = fs->getCurrentPath().evalNative(pathStr);
    if (fs->getRoot().tryOpenSubdir(path) != kj::none) {
      importPath.add(kj::mv(path));
    } else {
      CLI_ERROR("No such directory.");
    }
  }

  struct Override {
    kj::String name;
    kj::StringPtr value;
  };
  Override parseOverride(kj::StringPtr str) {
    auto equalPos = KJ_UNWRAP_OR(str.findFirst('='), CLI_ERROR("Expected <name>=<value>"));
    return {kj::str(str.first(equalPos)), str.slice(equalPos + 1)};
  }

  void overrideSocketAddr(kj::StringPtr param) {
    auto [name, value] = parseOverride(param);
    server->overrideSocket(kj::mv(name), kj::str(value));
  }

#if _WIN32
  void validateSocketFd(uint fd, kj::StringPtr label) {
    int acceptcon = 0;
    int optlen = sizeof(acceptcon);
    int result = getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, (char*)&acceptcon, &optlen);
    if (result == SOCKET_ERROR) {
      // https://learn.microsoft.com/en-us/windows/win32/api/winsock/nf-winsock-getsockopt#return-value
      switch (int error = WSAGetLastError()) {
        case WSAENOTSOCK:
          CLI_ERROR("File descriptor is not a socket.");
        case WSAENOPROTOOPT:
          // Some operating systems don't support SO_ACCEPTCONN; in that case just move on and
          // assume it is listening.
          break;
        default:
          KJ_FAIL_SYSCALL("getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN)", error);
      }
    } else if (!acceptcon) {
      CLI_ERROR("Socket for ", label, " is not listening.");
    }
  }
#else
  void validateSocketFd(uint fd, kj::StringPtr label) {
    int acceptcon = 0;
    socklen_t optlen = sizeof(acceptcon);
    KJ_SYSCALL_HANDLE_ERRORS(getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &acceptcon, &optlen)) {
      case EBADF:
        CLI_ERROR("File descriptor is not open.");
      case ENOTSOCK:
        CLI_ERROR("File descriptor is not a socket.");
      case ENOPROTOOPT:
        // Some operating systems don't support SO_ACCEPTCONN; in that case just move on and
        // assume it is listening.
        break;
      default:
        KJ_FAIL_SYSCALL("getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN)", error);
    }
    else {
      if (!acceptcon) {
        CLI_ERROR("Socket for ", label, " is not listening.");
      }
    }
  }
#endif

  void overrideSocketFd(kj::StringPtr param) {
    auto [name, value] = parseOverride(param);

    int fd = KJ_UNWRAP_OR(value.tryParseAs<uint>(),
        CLI_ERROR("Socket value must be a file descriptor (non-negative integer)."));

    validateSocketFd(fd, name);

    inheritedFds.add(fd);
    server->overrideSocket(kj::mv(name),
        io.lowLevelProvider->wrapListenSocketFd(fd, kj::LowLevelAsyncIoProvider::TAKE_OWNERSHIP));
  }

  void overrideDirectory(kj::StringPtr param) {
    auto [name, value] = parseOverride(param);
    server->overrideDirectory(kj::mv(name), kj::str(value));
  }

  void overrideExternal(kj::StringPtr param) {
    auto [name, value] = parseOverride(param);
    server->overrideExternal(kj::mv(name), kj::str(value));
  }

#ifdef WORKERD_USE_PERFETTO
  void enablePerfetto(kj::StringPtr param) {
    auto [name, value] = parseOverride(param);
    perfettoTraceDestination = kj::str(name);
    perfettoTraceCategories = kj::str(value);
  }
#endif

  void enableInspector(kj::StringPtr param) {
    server->enableInspector(kj::str(param));
  }

  void enableControl(kj::StringPtr param) {
    int fd = KJ_UNWRAP_OR(param.tryParseAs<uint>(),
        CLI_ERROR("Output value must be a file descriptor (non-negative integer)."));
    server->enableControl(fd);
  }

  void enableDebugPort(kj::StringPtr param) {
    server->enableDebugPort(kj::str(param));
  }

  void setPackageDiskCacheDir(kj::StringPtr pathStr) {
    kj::Path path = fs->getCurrentPath().eval(pathStr);
    kj::Maybe<kj::Own<const kj::Directory>> dir =
        fs->getRoot().tryOpenSubdir(path, kj::WriteMode::MODIFY);
    server->setPackageDiskCacheRoot(
        kj::mv(KJ_UNWRAP_OR(dir, CLI_ERROR("package disk cache dir must exist"))));
  }

  void setPyodideDiskCacheDir(kj::StringPtr pathStr) {
    kj::Path path = fs->getCurrentPath().eval(pathStr);
    kj::Maybe<kj::Own<const kj::Directory>> dir =
        fs->getRoot().tryOpenSubdir(path, kj::WriteMode::MODIFY);
    server->setPyodideDiskCacheRoot(kj::mv(dir));
  }

  void setPythonLoadSnapshot(kj::StringPtr pathStr) {
    server->setPythonLoadSnapshot(kj::str(pathStr));
  }
  void setPythonSnapshotDirectory(kj::StringPtr pathStr) {
    kj::Path path = fs->getCurrentPath().eval(pathStr);
    kj::Maybe<kj::Own<const kj::Directory>> dir =
        fs->getRoot().tryOpenSubdir(path, kj::WriteMode::MODIFY);
    server->setPythonSnapshotDirectory(kj::mv(dir));
  }

  void parsePythonCompatFlag(kj::StringPtr compatFlagStr) {
    auto builder = kj::heap<capnp::MallocMessageBuilder>();
    auto configBuilder = builder->initRoot<config::Config>();
    auto service = configBuilder.initServices(1)[0];
    service.setName("main");
    auto worker = service.initWorker();
    worker.setCompatibilityDate("2023-12-18");
    auto flags = worker.initCompatibilityFlags(2);
    flags.set(0, compatFlagStr);
    flags.set(1, "python_workers");
    auto mod = worker.initModules(1)[0];
    mod.setName("main.py");
    mod.setPythonModule("def test():\n pass");
    config = configBuilder.asReader();
    configOwner = kj::mv(builder);
    util::Autogate::initAutogate(getConfig().getAutogates());
  }

  void watch() {
#if _WIN32
    // The watcher itself works on Windows, but --watch's reload (reloadFromConfigChange(), a
    // re-exec) is not implemented there yet, so the feature as a whole is not.
    CLI_ERROR("File watching is not yet implemented on your OS. Sorry! Pull requests welcome!");
#else
    auto& w = *watcher.emplace(kj::heap<kj_rs_io::FileWatcher>());
    KJ_IF_SOME(e, exeInfo) {
      w.watch(fs->getCurrentPath().eval(e.path));
    } else {
      CLI_ERROR("Can't use --watch when we're unable to find our own executable.");
    }
#endif
  }

  void parseConfigFile(kj::StringPtr pathStr) {
    if (pathStr == "-") {
      // Read from stdin.

      if (!binaryConfig) {
        CLI_ERROR("Reading config from stdin is only allowed with --binary.");
      }

      // Can't use mmap() because it's probably not a file.
#if _WIN32
      auto handle = GetStdHandle(STD_INPUT_HANDLE);
      auto stream = kj::HandleInputStream(handle);
      auto reader = kj::heap<capnp::InputStreamMessageReader>(stream, CONFIG_READER_OPTIONS);
#else
      auto reader = kj::heap<capnp::StreamFdMessageReader>(STDIN_FILENO, CONFIG_READER_OPTIONS);
#endif
      config = reader->getRoot<config::Config>();
      configOwner = kj::mv(reader);
    } else {
      // Read file from disk.
      auto path = fs->getCurrentPath().evalNative(pathStr);
      auto file = KJ_UNWRAP_OR(fs->getRoot().tryOpenFile(path), CLI_ERROR("No such file."));

      // Use stat() to check that we have a file vs a directory which will fail to mmap
      auto metadata = file->stat();
      if (metadata.type != kj::FsNode::Type::FILE) {
        CLI_ERROR("Config path is not a file.");
      }

      if (binaryConfig) {
        // Interpret as binary config.
        auto mapping = file->mmap(0, file->stat().size);
        auto words = kj::arrayPtr(reinterpret_cast<const capnp::word*>(mapping.begin()),
            mapping.size() / sizeof(capnp::word));
        auto reader = kj::heap<capnp::FlatArrayMessageReader>(words, CONFIG_READER_OPTIONS)
                          .attach(kj::mv(mapping));
        config = reader->getRoot<config::Config>();
        configOwner = kj::mv(reader);
      } else {
        // Interpret as schema file.
        schemaParser.loadCompiledTypeAndDependencies<config::Config>();

        parsedSchema = schemaParser.parseFile(kj::heap<SchemaFileImpl>(fs->getRoot(),
            fs->getCurrentPath(), kj::mv(path), nullptr, importPath, kj::mv(file),
            watcher.map(
                [](kj::Own<kj_rs_io::FileWatcher>& w) -> kj_rs_io::FileWatcher& { return *w; }),
            *this));

        // Construct a list of top-level constants of type `Config`. If there is exactly one,
        // we can use it by default.
        for (auto nested: parsedSchema.getAllNested()) {
          if (nested.getProto().isConst()) {
            auto constSchema = nested.asConst();
            auto type = constSchema.getType();
            if (type.isStruct() &&
                type.asStruct().getProto().getId() == capnp::typeId<config::Config>()) {
              topLevelConfigConstants.add(constSchema);
            }
          }
        }
      }
    }

    // We'll fail at getConfig() if there are multiple top level Config objects.
    // The error message says that you have to specify which config to use, but
    // it's not clear that there is any mechanism to do that.
    util::Autogate::initAutogate(getConfig().getAutogates());
  }

  void setConstName(kj::StringPtr name) {
    auto parent = parsedSchema;

    for (;;) {
      auto dotPos = KJ_UNWRAP_OR(name.findFirst('.'), break);
      auto parentName = name.first(dotPos);
      parent = KJ_UNWRAP_OR(parent.findNested(kj::str(parentName)),
          CLI_ERROR("No such constant is defined in the config file (the parent scope '",
              parentName, "' does not exist)."));
      name = name.slice(dotPos + 1);
    }

    auto node = KJ_UNWRAP_OR(parsedSchema.findNested(name),
        CLI_ERROR("No such constant is defined in the config file."));

    if (!node.getProto().isConst()) {
      CLI_ERROR("Symbol is not a constant.");
    }

    auto constSchema = node.asConst();
    auto type = constSchema.getType();
    if (!type.isStruct() || type.asStruct().getProto().getId() != capnp::typeId<config::Config>()) {
      CLI_ERROR("Constant is not of type 'Config'.");
    }

    config = constSchema.as<config::Config>();
  }

  void setTestFilter(kj::StringPtr filter) {
    kj::Vector<kj::String> parts;

    for (;;) {
      KJ_IF_SOME(pos, filter.findFirst(':')) {
        parts.add(kj::str(filter.first(pos)));
        filter = filter.slice(pos + 1);
      } else {
        parts.add(kj::str(filter));
        break;
      }
    }

    switch (parts.size()) {
      case 0:
        KJ_UNREACHABLE;
      case 1:
        testServicePattern = kj::mv(parts[0]);
        break;
      case 2:
        testServicePattern = kj::mv(parts[0]);
        testEntrypointPattern = kj::mv(parts[1]);
        break;
      case 3:
        setConstName(parts[0]);
        testServicePattern = kj::mv(parts[1]);
        testEntrypointPattern = kj::mv(parts[2]);
        break;
      default:
        CLI_ERROR("Too many colons.");
    }
  }

  void setTestCompatDate(kj::StringPtr date) {
    testCompatDate = kj::str(date);
  }

  void compile() {
    if (hadErrors) {
      // Errors were already reported with context.error(), so context.exit() will exit with a
      // non-zero code.
      context.exit();
    }

    config::Config::Reader config = getConfig();

#if _WIN32
    if (_isatty(_fileno(stdout))) {
#else
    if (isatty(STDOUT_FILENO)) {
#endif
      context.exitError(
          "Refusing to write binary to the terminal. Please use `>` to send the output to a file.");
    }

#if !_WIN32
    // Grab the inode info before we write anything.
    struct stat stats;
    KJ_SYSCALL(fstat(STDOUT_FILENO, &stats));
#endif

#if _WIN32
    kj::FdOutputStream out(_fileno(stdout));
#else
    kj::FdOutputStream out(STDOUT_FILENO);
#endif

    if (configOnly) {
      // Write just the config -- in normal message format -- to stdout.
      uint64_t size = config.totalSize().wordCount + 1;
      capnp::MallocMessageBuilder builder(size + 1);
      builder.setRoot(config);
      KJ_DASSERT(builder.getSegmentsForOutput().size() == 1);
      capnp::writeMessage(out, builder);
    } else {
      // Write an executable file to stdout by concatenating this executable, the config, and the
      // magic suffix. This takes advantage of the fact that you can append arbitrary stuff to an
      // ELF binary or Windows executable without affecting the ability to execute the program.

      // Copy the executable to the output.
      {
        auto& exe = KJ_UNWRAP_OR(exeInfo,
            CLI_ERROR(
                "Unable to find and open the program's own executable, so cannot produce a new "
                "binary with compiled-in config."));

        auto mapping = exe.file->mmap(0, exe.file->stat().size);
        out.write(mapping);

        // Pad to a word boundary if necessary.
        size_t n = mapping.size() % sizeof(capnp::word);
        if (n != 0) {
          kj::byte pad[sizeof(capnp::word)] = {0};
          out.write(kj::arrayPtr(pad).slice(n));
        }
      }

      // Now write the config, plus magic suffix. We're going to write the config as a
      // single-segment flat message, which makes it easier to consume.
      {
        uint64_t size = config.totalSize().wordCount + 1;
        static_assert(sizeof(uint64_t) + sizeof(COMPILED_MAGIC_SUFFIX) == sizeof(capnp::word) * 3);
        auto words = kj::heapArray<capnp::word>(size + 3);
        words.asBytes().fill(0);
        capnp::copyToUnchecked(config, words.first(size));

        memcpy(&words[words.size() - 3], &size, sizeof(size));
        memcpy(&words[words.size() - 2], COMPILED_MAGIC_SUFFIX, sizeof(COMPILED_MAGIC_SUFFIX));

        out.write(words.asBytes());
      }

#if !_WIN32
      // If we wrote a regular file, and it was empty before we started writing, then let's go ahead
      // and set the executable bit on the file.
      if (S_ISREG(stats.st_mode) && stats.st_size == 0) {
        // Add executable bit for all users who have read access.
        mode_t mode = stats.st_mode;
        if (mode & S_IRUSR) {
          mode |= S_IXUSR;
        }
        if (mode & S_IRGRP) {
          mode |= S_IXGRP;
        }
        if (mode & S_IROTH) {
          mode |= S_IXOTH;
        }
        KJ_SYSCALL(fchmod(STDOUT_FILENO, mode));
      }
#endif
    }
  }

  template <typename Func>
  void serveImpl(Func&& func) noexcept {
    if (hadErrors) {
      // Can't start, stuff is broken.
      KJ_IF_SOME(w, watcher) {
        // In --watch mode, it's annoying if the server exits and stops watching. Let's wait for
        // someone to fix the config.
        context.warning(
            "Can't start server due to config errors, waiting for config files to change...");
        waitForChanges(*w).wait(io.waitScope);
        reloadFromConfigChange();
      } else {
        // Errors were reported earlier, so context.exit() will exit with a non-zero status.
        context.exit();
      }
    } else {
#ifdef WORKERD_USE_PERFETTO
      kj::Maybe<PerfettoSession> maybePerfettoSession;
      KJ_IF_SOME(dest, perfettoTraceDestination) {
        maybePerfettoSession =
            PerfettoSession(dest, kj::mv(perfettoTraceCategories).orDefault(kj::String()));
      }
#endif
      TRACE_EVENT("workerd", "serveImpl()");
      auto config = getConfig();

      // Configure structured logging in the process context
      if (config.hasLogging() ? config.getLogging().getStructuredLogging()
                              : config.getStructuredLogging()) {
        context.enableStructuredLogging();
      }

      auto platform = jsg::defaultPlatform(0);
      WorkerdPlatform v8Platform(*platform);
      jsg::V8System v8System(v8Platform,
          KJ_MAP(flag, config.getV8Flags()) -> kj::StringPtr { return flag; }, platform.get());
      auto promise = func(v8System, config);
      KJ_IF_SOME(w, watcher) {
        promise = promise.exclusiveJoin(waitForChanges(*w).then([this]() {
          // Watch succeeded.
          reloadFromConfigChange();
        }));
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
  }

  void serve() noexcept {
    serveImpl([&](jsg::V8System& v8System, config::Config::Reader config) {
#if _WIN32
      return server->run(v8System, config);
#else
      return server->run(v8System, config,
          // Gracefully drain when SIGTERM is received.
          kj_rs_io::onSignal(SIGTERM));
#endif
    });
  }

  void test() {
    if (!noVerbose) {
      // Always turn on info logging when running tests so that uncaught exceptions are displayed.
      // TODO(beta): This can be removed once we improve our error logging story.
      kj::_::Debug::setLogLevel(kj::LogSeverity::INFO);
    }
    if (predictable) {
      setPredictableModeForTest();
    }
    if (gcStress) {
      setGcStressModeForTest();
    }
    if (allAutogates) {
      util::Autogate::initAllAutogates();
    }

    KJ_IF_SOME(compatDate, testCompatDate) {
      server->setTestCompatibilityDateOverride(kj::str(compatDate));
    }

    // Enable loopback sockets in tests only.
    kj::downcast<kj_rs_io::TokioNetwork>(io.provider->getNetwork()).enableLoopback();

    serveImpl([&](jsg::V8System& v8System, config::Config::Reader config) {
      return server
          ->test(v8System, config,
              testServicePattern.map([](auto& s) -> kj::StringPtr { return s; }).orDefault("*"_kj),
              testEntrypointPattern.map([](auto& s) -> kj::StringPtr {
        return s;
      }).orDefault("*"_kj))
          .then([this](bool result) -> kj::Promise<void> {
        if (!result) {
          context.error("Tests failed!");
        }

        if (watcher == kj::none) {
          return kj::READY_NOW;
        } else {
          // Pause forever waiting for watcher.
          return kj::NEVER_DONE;
        }
      });
    });
  }

#if _WIN32
  void reloadFromConfigChange() {
    KJ_UNREACHABLE("Watching is not yet implemented on Windows");
  }
#else
  [[noreturn]] void reloadFromConfigChange() {
    // Write extra spaces to fully overwrite the line that we wrote earlier with a CR but no LF:
    //     "Noticed configuration change, reloading shortly...\r"
    context.warning("Reloading due to config change...                                      ");
    for (auto fd: inheritedFds) {
      // Disable close-on-exec for inherited FDs so that the successor process can also inherit
      // them.
      KJ_SYSCALL(ioctl(fd, FIONCLEX));
    }
    bool missingBinary = false;
    for (;;) {
      KJ_SYSCALL_HANDLE_ERRORS(execve(KJ_ASSERT_NONNULL(exeInfo).path.cStr(), argv, environ)) {
        case ENOENT: {
          // Write a message
          // TODO(cleanup): Writing directly to stderr is super-hacky.
          if (!missingBinary) {
            context.warning("The server executable is missing! Waiting for it to reappear...\r");
            missingBinary = true;
          }
          sleep(1);
          break;
        }
        default:
          KJ_FAIL_SYSCALL("execve", error);
      }
    }
  }
#endif

 private:
  StructuredLoggingProcessContext& context;
  char** argv;

  bool binaryConfig = false;
  bool configOnly = false;
  bool noVerbose = false;
  bool predictable = false;
  bool gcStress = false;
  bool allAutogates = false;
  kj::Maybe<kj::String> testCompatDate;
  kj::Maybe<kj::Own<kj_rs_io::FileWatcher>> watcher;

  kj::Own<kj::Filesystem> fs = kj::newDiskFilesystem();
  kj::AsyncIoContext io = kj::setupAsyncIo();
  EntropySourceImpl entropySource;

  kj::Vector<kj::Path> importPath;
  capnp::SchemaParser schemaParser;
  capnp::ParsedSchema parsedSchema;
  kj::Vector<capnp::ConstSchema> topLevelConfigConstants;

  kj::Own<void> configOwner;  // backing object for `config`, if it's not `schemaParser`.
  kj::Maybe<config::Config::Reader> config;

  kj::Vector<int> inheritedFds;

  kj::Maybe<kj::String> testServicePattern;
  kj::Maybe<kj::String> testEntrypointPattern;

#ifdef WORKERD_USE_PERFETTO
  kj::Maybe<kj::String> perfettoTraceDestination;
  kj::Maybe<kj::String> perfettoTraceCategories;
#endif

  kj::Own<Server> server;

  // This is a randomly-generated 128-bit number that identifies when a binary has been compiled
  // with a specific config in order to run stand-alone.
  static constexpr uint64_t COMPILED_MAGIC_SUFFIX[2] = {// The layout of such a binary is:
    //
    // - Binary executable data (copy of the Workers Runtime binary).
    // - Padding to 8-byte boundary.
    // - Cap'n-Proto-encoded config.
    // - 8-byte size of config, counted in 8-byte words.
    // - 16-byte magic number COMPILED_MAGIC_SUFFIX.

    0xa69eda94d3cc02b5ull, 0xa3d977fdbf547d7full};

  struct ExeInfo {
    kj::String path;
    kj::Own<const kj::ReadableFile> file;
  };

#if _WIN32
  static kj::Maybe<ExeInfo> tryOpenExe(kj::Filesystem& fs, kj::StringPtr path) {
    // TODO(bug): Like with Unix below, we should probably use native CreateFile() here, but it has
    // sooooo many arguments, I don't want to deal with it.
    auto parsedPath = fs.getCurrentPath().evalNative(path);
    KJ_IF_SOME(file, fs.getRoot().tryOpenFile(parsedPath)) {
      return ExeInfo{kj::str(path), kj::mv(file)};
    }
    return kj::none;
  }
#else
  static kj::Maybe<ExeInfo> tryOpenExe(kj::Filesystem& fs, kj::StringPtr path) {
    // Use open() and not fs.getRoot().tryOpenFile() because we probably want to use true kernel
    // path resolution here, not KJ's logical path resolution.
    int fd = open(path.cStr(), O_RDONLY);
    if (fd < 0) {
      return kj::none;
    }
    return ExeInfo{kj::str(path), kj::newDiskFile(kj::OwnFd(fd))};
  }
#endif

  static kj::Maybe<ExeInfo> getExecFile(kj::ProcessContext& context, kj::Filesystem& fs) {
#ifdef __GLIBC__
    auto execfn = getauxval(AT_EXECFN);
    if (execfn != 0) {
      return tryOpenExe(fs, reinterpret_cast<const char*>(execfn));
    }
#endif

#if __linux__
    KJ_IF_SOME(link, fs.getRoot().tryReadlink(kj::Path({"proc", "self", "exe"}))) {
      return tryOpenExe(fs, link);
    }
#endif

#if __APPLE__
    // https://astojanov.github.io/blog/2011/09/26/pid-to-absolute-path.html
    pid_t pid = getpid();
    char pathbuf[PROC_PIDPATHINFO_MAXSIZE];
    if (proc_pidpath(pid, pathbuf, sizeof(pathbuf)) > 0) {
      return tryOpenExe(fs, pathbuf);
    }
#endif

#if _WIN32
    wchar_t pathbuf[MAX_PATH];
    int result = GetModuleFileNameW(NULL, pathbuf, MAX_PATH);
    if (result > 0) {
      auto decoded = kj::decodeWideString(kj::arrayPtr(pathbuf, result));
      KJ_ASSERT(!decoded.hadErrors);
      return tryOpenExe(fs, decoded);
    }
#endif

    // TODO(beta): Fall back to searching $PATH.
    return kj::none;
  }

  config::Config::Reader getConfig() {
    KJ_IF_SOME(c, config) {
      return c;
    } else {
      // The optional `<const-name>` parameter must not have been given -- otherwise we would have
      // a non-null `config` by this point. See if we can infer the correct constant...
      if (topLevelConfigConstants.empty()) {
        context.exitError(
            "The config file does not define any top-level constants of type 'Config'.");
      } else if (topLevelConfigConstants.size() == 1) {
        return config.emplace(topLevelConfigConstants[0].as<config::Config>());
      } else {
        auto names = KJ_MAP(cnst, topLevelConfigConstants) { return cnst.getShortDisplayName(); };
        // TODO: this error message says "you must specify which one to use".
        // This is not actually possible? Either fix the error message to say
        // **how** to specify which config object to use or tell user to define
        // exactly one top level Config constant.
        context.exitError(kj::str(
            "The config file defines multiple top-level constants of type 'Config', so you must "
            "specify which one to use. The options are: ",
            kj::strArray(names, ", ")));
      }
    }
  }

  kj::Maybe<ExeInfo> exeInfo = getExecFile(context, *fs);

  bool hadErrors = false;

  void reportParsingError(kj::StringPtr file,
      capnp::SchemaFile::SourcePos start,
      capnp::SchemaFile::SourcePos end,
      kj::StringPtr message) override {
    if (start.line == end.line && start.column < end.column) {
      context.error(kj::str(
          file, ":", start.line + 1, ":", start.column + 1, "-", end.column + 1, ": ", message));
    } else {
      context.error(kj::str(file, ":", start.line + 1, ":", start.column + 1, ": ", message));
    }

    hadErrors = true;
  }

#if _WIN32
  kj::Promise<void> waitForChanges(kj_rs_io::FileWatcher& watcher) {
    KJ_UNIMPLEMENTED("Watching is not yet implemented on Windows");
  }
#else
  // Wait for the FileWatcher to report a change, and then wait a moment for changes to settle
  // down, in case there's a bunch of changes all at once.
  kj::Promise<void> waitForChanges(kj_rs_io::FileWatcher& watcher) {
    co_await watcher.onChange();

    // Saw our first change!

    // Let the user know we saw the config change.
    // We don't include a newline but rather a carriage return so that when the next
    // line is written, this line disappears, to reduce noise.
    // TODO(cleanup): Writing directly to stderr is super-hacky.
    auto message = "Noticed configuration change, reloading shortly...\r"_kjb;
    kj::FdOutputStream(STDERR_FILENO).write(message);

    static auto const waitForResult = [](kj::Promise<void> promise,
                                          bool result = false) -> kj::Promise<bool> {
      co_await promise;
      co_return result;
    };

    for (;;) {
      auto nextChange = waitForResult(watcher.onChange());
      auto timeout =
          waitForResult(io.provider->getTimer().afterDelay(500 * kj::MILLISECONDS), true);
      bool sawTimeout = co_await nextChange.exclusiveJoin(kj::mv(timeout));

      // If we timed out, we end the loop. If we didn't time out, then we must have seen yet
      // another change, so we loop again with a new timeout.
      if (sawTimeout) break;
    }

    co_return;
  }
#endif
};

}  // namespace
}  // namespace workerd::server

int main(int argc, char* argv[]) {
  workerd::server::StructuredLoggingProcessContext context(argv[0]);

  workerd::server::CliMain mainObject(context, argv);

#if defined(WORKERD_FUZZILLI) && defined(__linux__)
  initSignalHandlers();
#endif

  return ::kj::runMainAndExit(context, mainObject.getMain(), argc, argv);
}
