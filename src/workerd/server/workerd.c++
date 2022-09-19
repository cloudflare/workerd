// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <kj/main.h>
#include <kj/filesystem.h>
#include <kj/map.h>
#include <capnp/message.h>
#include <capnp/serialize.h>
#include <capnp/schema-parser.h>
#include <capnp/dynamic.h>
#include <workerd/server/workerd.capnp.h>
#include <workerd/server/workerd-meta.capnp.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include "server.h"
#include <unistd.h>
#include <sys/syscall.h>
#include <workerd/jsg/setup.h>
#include <kj/async-unix.h>
#include <sys/ioctl.h>
#include <openssl/rand.h>
#include <workerd/io/compatibility-date.h>

#if __linux__
#include <sys/inotify.h>
#endif

#ifdef __GLIBC__
#include <sys/auxv.h>
#endif

namespace workerd::server {

static kj::StringPtr getVersionString() {
  static const kj::String result = kj::str("workerd ", SUPPORTED_COMPATIBILITY_DATE);
  return result;
}

// =======================================================================================

class EntropySourceImpl: public kj::EntropySource {
public:
  void generate(kj::ArrayPtr<kj::byte> buffer) override {
    KJ_ASSERT(RAND_bytes(buffer.begin(), buffer.size()) == 1);
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

#define CLI_METHOD(name) cliMethod(KJ_BIND_METHOD(*this, name))
// Pass to MainBuilder when a function returning kj::MainBuilder::Validity is needed, implemented
// by a method of this class.

#define CLI_ERROR(...) throw CliError(kj::str(__VA_ARGS__))
// Throws an exception that is caught and reported as a usage error.

constexpr capnp::ReaderOptions CONFIG_READER_OPTIONS = {
  .traversalLimitInWords = kj::maxValue
  // Configs can legitimately be very large and are not malicious, so use an effectively-infinite
  // traversal limit.
};

// =======================================================================================

#if __linux__

class FileWatcher {
  // Class which uses inotify to watch a set of files and alert when they change.

public:
  FileWatcher(kj::UnixEventPort& port)
      : timer(port.getTimer()), inotifyFd(makeInotify()),
        observer(port, inotifyFd, kj::UnixEventPort::FdObserver::OBSERVE_READ) {}

  bool isSupported() { return true; }

  void watch(kj::PathPtr path) {
    auto pathStr = path.parent().toNativeString(true);

    int wd = watches.findOrCreate(pathStr, [&]() {
      int wd;
      uint32_t mask = IN_DELETE | IN_MODIFY | IN_MOVE | IN_CREATE;
      KJ_SYSCALL(wd = inotify_add_watch(inotifyFd, pathStr.cStr(), mask));
      return decltype(watches)::Entry { kj::mv(pathStr), wd };
    });

    auto& files = filesWatched.findOrCreate(wd, [&]() {
      return decltype(filesWatched)::Entry { wd, {} };
    });

    files.upsert(kj::str(path.basename()[0]), [](auto&&...) {});
  }

  kj::Promise<void> onChange() {
    kj::byte buffer[4096];

    for (;;) {
      ssize_t n;
      KJ_NONBLOCKING_SYSCALL(n = read(inotifyFd, buffer, sizeof(buffer)));

      if (n < 0) {
        auto promise = observer.whenBecomesReadable().then([]() { return false; });
        if (sawChange) {
          promise = promise.exclusiveJoin(
              timer.afterDelay(500 * kj::MILLISECONDS).then([]() { return true; }));
        }
        return promise.then([this](bool timeout) -> kj::Promise<void> {
          if (timeout) {
            // We've seen a change in the past, and then saw nothing change for a moment. We're
            // done!
            return kj::READY_NOW;
          } else {
            // There's new changes to read.
            return onChange();
          }
        });
      }

      kj::byte* ptr = buffer;
      while (n > 0) {
        KJ_ASSERT(n >= sizeof(struct inotify_event));

        auto& event = *reinterpret_cast<struct inotify_event*>(ptr);
        size_t eventSize = sizeof(struct inotify_event) + event.len;
        KJ_ASSERT(n >= eventSize);
        KJ_ASSERT(eventSize % sizeof(void*) == 0);
        ptr += eventSize;
        n -= eventSize;

        if (event.len > 0 && event.name[0] != '\0') {
          auto& watched = KJ_ASSERT_NONNULL(filesWatched.find(event.wd));
          if (watched.find(kj::StringPtr(event.name)) != nullptr) {
            // HIT! Don't resolve yet, though. Let's wait for things to settle down.
            if (!sawChange) {
              // Let the user know we saw the config change.
              // We don't include a newline but rather a carriage return so that when the next
              // line is written, this line disappears, to reduce noise.
              // TODO(cleanup): Writing directly to stderr is super-hacky.
              kj::StringPtr message = "Noticed configuration change, reloading shortly...\r";
              kj::FdOutputStream(STDERR_FILENO).write(message.begin(), message.size());
            }
            sawChange = true;
            return onChange();
          }
        }
      }
    }
  }

private:
  kj::Timer& timer;
  kj::AutoCloseFd inotifyFd;
  kj::UnixEventPort::FdObserver observer;

  kj::HashMap<kj::String, int> watches;
  kj::HashMap<int, kj::HashSet<kj::String>> filesWatched;

  bool sawChange = false;

  static kj::AutoCloseFd makeInotify() {
    int fd;
    KJ_SYSCALL(fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC));
    return kj::AutoCloseFd(fd);
  }
};

#else  // #if __linux__

class FileWatcher {
  // Dummy FileWatcher implementation for operating systems that aren't supported yet.

public:
  FileWatcher(kj::UnixEventPort& port) {}

  bool isSupported() { return false; }

  void watch(kj::PathPtr path) {}

  void watch(const kj::ReadableFile& file) {}

  kj::Promise<void> onChange() { return kj::NEVER_DONE; }
private:
};

#endif  // #__linux__, #else

// =======================================================================================

kj::Maybe<kj::Own<capnp::SchemaFile>> tryImportBulitin(kj::StringPtr name);

class SchemaFileImpl final: public capnp::SchemaFile {
  // Callbacks for capnp::SchemaFileLoader. Implementing this interface lets us control import
  // resolution, which we want to do mainly so that we can set watches on all imported files.
  //
  // These callbacks also give us more control over error reporting, in particular the ability
  // to not throw an exception on the first error seen.

public:
  class ErrorReporter {
  public:
    virtual void reportParsingError(kj::StringPtr file,
        SourcePos start, SourcePos end, kj::StringPtr message) = 0;
  };

  SchemaFileImpl(const kj::Directory& root, kj::PathPtr current,
                 kj::Path fullPathParam, kj::PathPtr basePath,
                 kj::ArrayPtr<const kj::Path> importPath,
                 kj::Own<const kj::ReadableFile> file,
                 kj::Maybe<FileWatcher&> watcher,
                 ErrorReporter& errorReporter)
      : root(root), current(current), fullPath(kj::mv(fullPathParam)), basePath(basePath),
        importPath(importPath), file(kj::mv(file)), watcher(watcher),
        errorReporter(errorReporter) {
    if (fullPath.startsWith(current)) {
      // Simplify display name by removing current directory prefix.
      displayName = fullPath.slice(current.size(), fullPath.size()).toNativeString();
    } else {
      // Use full path.
      displayName = fullPath.toNativeString(true);
    }

    KJ_IF_MAYBE(w, watcher) {
      w->watch(fullPath);
    }
  }

  kj::StringPtr getDisplayName() const override {
    return displayName;
  }

  kj::Array<const char> readContent() const override {
    return file->mmap(0, file->stat().size).releaseAsChars();
  }

  kj::Maybe<kj::Own<SchemaFile>> import(kj::StringPtr target) const override {
    if (target.startsWith("/")) {
      auto parsedPath = kj::Path::parse(target.slice(1));
      for (auto& candidate: importPath) {
        auto newFullPath = candidate.append(parsedPath);

        KJ_IF_MAYBE(newFile, root.tryOpenFile(newFullPath)) {
          return kj::implicitCast<kj::Own<SchemaFile>>(kj::heap<SchemaFileImpl>(
              root, current, kj::mv(newFullPath), candidate, importPath,
              kj::mv(*newFile), watcher, errorReporter));
        }
      }
      // No matching file found. Check if we have a builtin.
      return tryImportBulitin(target);
    } else {
      auto relativeTo = fullPath.slice(basePath.size(), fullPath.size());
      auto parsed = relativeTo.parent().eval(target);
      auto newFullPath = basePath.append(parsed);

      KJ_IF_MAYBE(newFile, root.tryOpenFile(newFullPath)) {
        return kj::implicitCast<kj::Own<SchemaFile>>(kj::heap<SchemaFileImpl>(
            root, current, kj::mv(newFullPath), basePath, importPath, kj::mv(*newFile),
            watcher, errorReporter));
      } else {
        return nullptr;
      }
    }
  }

  bool operator==(const SchemaFile& other) const override {
    if (auto downcasted = dynamic_cast<const SchemaFileImpl*>(&other)) {
      return fullPath == downcasted->fullPath;
    } else {
      return false;
    }
  }
  bool operator!=(const SchemaFile& other) const override {
    return !operator==(other);
  }
  size_t hashCode() const override {
    return kj::hashCode(fullPath);
  }

  void reportError(SourcePos start, SourcePos end, kj::StringPtr message) const override {
    errorReporter.reportParsingError(displayName, start, end, message);
  }

private:
  const kj::Directory& root;
  kj::PathPtr current;

  kj::Path fullPath;
  // Full path from root of filesystem to the file.

  kj::PathPtr basePath;
  // If this file was reached by scanning `importPath`, `basePath` is the particular import path
  // directory that was used, otherwise it is empty. `basePath` is always a prefix of `fullPath`.

  kj::ArrayPtr<const kj::Path> importPath;
  // Paths to search for absolute imports.

  kj::Own<const kj::ReadableFile> file;
  kj::String displayName;

  mutable kj::Maybe<FileWatcher&> watcher;
  // Mutable because the SchemaParser interface forces us to make all our methods `const` so that
  // parsing can happen on multiple threads, but we do not actually use multiple threads for
  // parsing, so we're good.

  ErrorReporter& errorReporter;
};

class BuiltinSchemaFileImpl final: public capnp::SchemaFile {
  // A schema file whose text is embedded into the binary for convenience.
  //
  // TODO(someday): Could `capnp::SchemaParser` be updated such that it can use the compiled-in
  //   schema nodes rather than re-parse the file from scratch? This is tricky as some information
  //   is lost after compilation which is needed to compile dependents, e.g. aliases are erased.
public:
  BuiltinSchemaFileImpl(kj::StringPtr name, kj::StringPtr content)
      : name(name), content(content) {}

  kj::StringPtr getDisplayName() const override {
    return name;
  }

  kj::Array<const char> readContent() const override {
    return kj::Array<const char>(content.begin(), content.size(), kj::NullArrayDisposer::instance);
  }

  kj::Maybe<kj::Own<SchemaFile>> import(kj::StringPtr target) const override {
    return tryImportBulitin(target);
  }

  bool operator==(const SchemaFile& other) const override {
    if (auto downcasted = dynamic_cast<const BuiltinSchemaFileImpl*>(&other)) {
      return downcasted->name == name;
    } else {
      return false;
    }
  }
  bool operator!=(const SchemaFile& other) const override {
    return !operator==(other);
  }
  size_t hashCode() const override {
    return kj::hashCode(name);
  }

  void reportError(SourcePos start, SourcePos end, kj::StringPtr message) const override {
    KJ_FAIL_ASSERT("parse error in built-in schema?", start.line, start.column, message);
  }

private:
  kj::StringPtr name;
  kj::StringPtr content;
};

kj::Maybe<kj::Own<capnp::SchemaFile>> tryImportBulitin(kj::StringPtr name) {
  if (name == "/capnp/c++.capnp") {
    return kj::heap<BuiltinSchemaFileImpl>("/capnp/c++.capnp", CPP_CAPNP_SCHEMA);
  } else if (name == "/workerd/workerd.capnp") {
    return kj::heap<BuiltinSchemaFileImpl>("/workerd/workerd.capnp", WORKERD_CAPNP_SCHEMA);
  } else {
    return nullptr;
  }
}

// =======================================================================================

class CliMain: public SchemaFileImpl::ErrorReporter {
public:
  CliMain(kj::ProcessContext& context, char** argv)
      : context(context), argv(argv),
        server(*fs, io.provider->getTimer(), io.provider->getNetwork(), entropySource,
            [&](kj::String error) {
          if (watcher == nullptr) {
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
        }) {
    KJ_IF_MAYBE(e, exeInfo) {
      auto& exe = *e->file;
      auto size = exe.stat().size;
      KJ_ASSERT(size > sizeof(COMPILED_MAGIC_SUFFIX) + sizeof(uint64_t));
      kj::byte magic[sizeof(COMPILED_MAGIC_SUFFIX)];
      exe.read(size - sizeof(COMPILED_MAGIC_SUFFIX), magic);
      if (memcmp(magic, COMPILED_MAGIC_SUFFIX, sizeof(COMPILED_MAGIC_SUFFIX)) == 0) {
        // Oh! It appears we are running a compiled binary, it has a config appended to the end.
        uint64_t configSize;
        exe.read(size - sizeof(COMPILED_MAGIC_SUFFIX) - sizeof(uint64_t),
            kj::arrayPtr(&configSize, 1).asBytes());
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
    if (config == nullptr) {
      return kj::MainBuilder(context, getVersionString(),
            "Runs the Cloudflare Workers Runtime.")
          .addSubCommand("serve", KJ_BIND_METHOD(*this, getServe),
              "run the server")
          .addSubCommand("compile", KJ_BIND_METHOD(*this, getCompile),
              "create a self-contained binary")
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
      return addServeOptionsAndBuild(builder);
    }
  }

  kj::MainBuilder& addConfigParsingOptions(kj::MainBuilder& builder) {
    return builder
        .addOptionWithArg({'I', "import-path"}, CLI_METHOD(addImportPath), "<dir>",
                          "Add <dir> to the list of directories searched for non-relative "
                          "imports in the config file (ones that start with a '/').")
        .addOption({'b', "binary"}, [this]() { binaryConfig = true; return true; },
                   "Specifies that the configuration file is an encoded binary Cap'n Proto "
                   "message, rather than the usual text format. This is particularly useful when "
                   "driving the server from higher-level tooling that automatically generates a "
                   "config.")
        .expectArg("<config-file>", CLI_METHOD(parseConfigFile))
        .expectOptionalArg("<const-name>", CLI_METHOD(setConstName));
  }

  kj::MainFunc addServeOptionsAndBuild(kj::MainBuilder& builder) {
    return builder
        .addOptionWithArg({'s', "socket-addr"}, CLI_METHOD(overrideSocketAddr), "<name>=<addr>",
                          "Override the socket named <name> to bind to the address <addr> instead "
                          "of the address specified in the config file.")
        .addOptionWithArg({'S', "socket-fd"}, CLI_METHOD(overrideSocketFd), "<name>=<fd>",
                          "Override the socket named <name> to listen on the already-open socket "
                          "descriptor <fd> instead of the address specified in the config file.")
        .addOptionWithArg({'d', "directory-path"}, CLI_METHOD(overrideDirectory), "<name>=<path>",
                          "Override the directory named <name> to point to <path> instead of the "
                          "path specified in the config file.")
        .addOptionWithArg({'e', "external-addr"}, CLI_METHOD(overrideExternal), "<name>=<addr>",
                          "Override the external service named <name> to connect to the address "
                          "<addr> instead of the address specified in the config file.")
        .addOption({'w', "watch"}, CLI_METHOD(watch),
                   "Watch configuration files (and server binary) and reload if they change. "
                   "Useful for development, but not recommended in production.")
        .callAfterParsing(CLI_METHOD(serve))
        .build();
  }

  kj::MainFunc getServe() {
    auto builder = kj::MainBuilder(context, getVersionString(),
          "Serve requests based on a config.",
          "Serves requests based on the configuration specified in <config-file>.");
    return addServeOptionsAndBuild(addConfigParsingOptions(builder));
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
        .addOption({"config-only"}, [this]() { configOnly = true; return true; },
          "Only write the encoded binary config to stdout. Do not attach it to an executable. "
          "The encoded config can be used as input to the \"serve\" command, without the need "
          "for any other files to be present.")
        .callAfterParsing(CLI_METHOD(compile))
        .build();
  }

  void addImportPath(kj::StringPtr pathStr) {
    auto path = fs->getCurrentPath().evalNative(pathStr);
    KJ_IF_MAYBE(dir, fs->getRoot().tryOpenSubdir(path)) {
      importPath.add(kj::mv(path));
    } else {
      CLI_ERROR("No such directory.");
    }
  }

  struct Override { kj::String name; kj::StringPtr value; };
  Override parseOverride(kj::StringPtr str) {
    auto equalPos = KJ_UNWRAP_OR(str.findFirst('='), CLI_ERROR("Expected <name>=<value>"));
    return { kj::str(str.slice(0, equalPos)), str.slice(equalPos + 1) };
  }

  void overrideSocketAddr(kj::StringPtr param) {
    auto [ name, value ] = parseOverride(param);
    server.overrideSocket(kj::mv(name), kj::str(value));
  }

  void overrideSocketFd(kj::StringPtr param) {
    auto [ name, value ] = parseOverride(param);

    int fd = KJ_UNWRAP_OR(value.tryParseAs<uint>(),
        CLI_ERROR("Socket value must be a file descriptor (non-negative integer)."));

    // Validate the socket.
    {
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
      } else {
        if (!acceptcon) {
          CLI_ERROR("Socket is not listening.");
        }
      }
    }

    inheritedFds.add(fd);
    server.overrideSocket(kj::mv(name), io.lowLevelProvider->wrapListenSocketFd(
        fd, kj::LowLevelAsyncIoProvider::TAKE_OWNERSHIP));
  }

  void overrideDirectory(kj::StringPtr param) {
    auto [ name, value ] = parseOverride(param);
    server.overrideDirectory(kj::mv(name), kj::str(value));
  }

  void overrideExternal(kj::StringPtr param) {
    auto [ name, value ] = parseOverride(param);
    server.overrideExternal(kj::mv(name), kj::str(value));
  }

  void watch() {
    auto& w = watcher.emplace(io.unixEventPort);
    if (!w.isSupported()) {
      CLI_ERROR("File watching is not yet implemented on your OS. Sorry! Pull requests welcome!");
    }

    KJ_IF_MAYBE(e, exeInfo) {
      w.watch(fs->getCurrentPath().eval(e->path));
    } else {
      CLI_ERROR("Can't use --watch when we're unable to find our own executable.");
    }
  }

  void parseConfigFile(kj::StringPtr pathStr) {
    if (pathStr == "-") {
      // Read from stdin.

      if (!binaryConfig) {
        CLI_ERROR("Reading config from stdin is only allowed with --binary.");
      }

      // Can't use mmap() because it's probably not a file.
      auto reader = kj::heap<capnp::StreamFdMessageReader>(STDIN_FILENO, CONFIG_READER_OPTIONS);
      config = reader->getRoot<config::Config>();
      configOwner = kj::mv(reader);
    } else {
      // Read file from disk.
      auto path = fs->getCurrentPath().evalNative(pathStr);
      auto file = KJ_UNWRAP_OR(fs->getRoot().tryOpenFile(path), CLI_ERROR("No such file."));

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

        parsedSchema = schemaParser.parseFile(
            kj::heap<SchemaFileImpl>(fs->getRoot(), fs->getCurrentPath(),
                kj::mv(path), nullptr, importPath, kj::mv(file), watcher, *this));

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
  }

  void setConstName(kj::StringPtr name) {
    auto parent = parsedSchema;

    for (;;) {
      auto dotPos = KJ_UNWRAP_OR(name.findFirst('.'), break);
      auto parentName = name.slice(0, dotPos);
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
    if (!type.isStruct() ||
        type.asStruct().getProto().getId() != capnp::typeId<config::Config>()) {
      CLI_ERROR("Constant is not of type 'Config'.");
    }

    config = constSchema.as<config::Config>();
  }

  void compile() {
    if (hadErrors) {
      // Errors were already reported with context.error(), so contex.exit() will exit with a
      // non-zero code.
      context.exit();
    }

    config::Config::Reader config = getConfig();

    if (isatty(STDOUT_FILENO)) {
      context.exitError(
          "Refusing to write binary to the terminal. Please use `>` to send the output to a file.");
    }

    // Grab the inode info before we write anything.
    struct stat stats;
    KJ_SYSCALL(fstat(STDOUT_FILENO, &stats));

    kj::FdOutputStream out(STDOUT_FILENO);

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
      // ELF binary without affecting the ability to execute the program.

      // Copy the executable to the output.
      {
        auto& exe = KJ_UNWRAP_OR(exeInfo, CLI_ERROR(
            "Unable to find and open the program's own executable, so cannot produce a new "
            "binary with compiled-in config."));

        auto mapping = exe.file->mmap(0, exe.file->stat().size);
        out.write(mapping.begin(), mapping.size());

        // Pad to a word boundary if necessary.
        size_t n = mapping.size() % sizeof(capnp::word);
        if (n != 0) {
          kj::byte pad[sizeof(capnp::word)] = {0};
          out.write(pad, sizeof(capnp::word) - n);
        }
      }

      // Now write the config, plus magic suffix. We're going to write the config as a
      // single-segment flat message, which makes it easier to consume.
      {
        uint64_t size = config.totalSize().wordCount + 1;
        static_assert(sizeof(uint64_t) + sizeof(COMPILED_MAGIC_SUFFIX) == sizeof(capnp::word) * 3);
        auto words = kj::heapArray<capnp::word>(size + 3);
        memset(words.asBytes().begin(), 0, words.asBytes().size());
        capnp::copyToUnchecked(config, words.slice(0, size));

        memcpy(&words[words.size() - 3], &size, sizeof(size));
        memcpy(&words[words.size() - 2], COMPILED_MAGIC_SUFFIX, sizeof(COMPILED_MAGIC_SUFFIX));

        out.write(words.asBytes().begin(), words.asBytes().size());
      }

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
    }
  }

  [[noreturn]] void serve() noexcept {
    if (hadErrors) {
      // Can't start, stuff is broken.
      KJ_IF_MAYBE(w, watcher) {
        // In --watch mode, it's annoying if the server exits and stops watching. Let's wait for
        // someone to fix the config.
        context.warning(
            "Can't start server due to config errors, waiting for config files to change...");
        w->onChange().wait(io.waitScope);
        reloadFromConfigChange();
      } else {
        // Errors were reported earlier, so context.exit() will exit with a non-zero status.
        context.exit();
      }
    } else {
      auto config = getConfig();
      jsg::V8System v8System(
          KJ_MAP(flag, config.getV8Flags()) -> kj::StringPtr { return flag; });
      auto promise = server.run(v8System, config);
      KJ_IF_MAYBE(w, watcher) {
        promise = promise.exclusiveJoin(w->onChange().then([this]() {
          // Watch succeeded.
          reloadFromConfigChange();
        }));
      }
      promise.wait(io.waitScope);
      context.exit();
    }
  }

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

private:
  kj::ProcessContext& context;
  char** argv;

  bool binaryConfig = false;
  bool configOnly = false;
  kj::Maybe<FileWatcher> watcher;

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

  Server server;

  static constexpr uint64_t COMPILED_MAGIC_SUFFIX[2] = {
    // This is a randomly-generated 128-bit number that identifies when a binary has been compiled
    // with a specific config in order to run stand-alone. The layout of such a binary is:
    //
    // - Binary executable data (copy of the Workers Runtime binary).
    // - Padding to 8-byte boundary.
    // - Cap'n-Proto-encoded config.
    // - 8-byte size of config, counted in 8-byte words.
    // - 16-byte magic number COMPILED_MAGIC_SUFFIX.

    0xa69eda94d3cc02b5ull,
    0xa3d977fdbf547d7full
  };

  struct ExeInfo {
    kj::String path;
    kj::Own<const kj::ReadableFile> file;
  };

  static kj::Maybe<ExeInfo> tryOpenExe(kj::StringPtr path) {
    // Use open() and not fs.getRoot().tryOpenFile() because we probably want to use true kernel
    // path resolution here, not KJ's logical path resolution.
    int fd = open(path.cStr(), O_RDONLY);
    if (fd < 0) {
      return nullptr;
    }
    return ExeInfo { kj::str(path), kj::newDiskFile(kj::AutoCloseFd(fd)) };
  }

  static kj::Maybe<ExeInfo> getExecFile(
      kj::ProcessContext& context, kj::Filesystem& fs) {
  #ifdef __GLIBC__
    auto execfn = getauxval(AT_EXECFN);
    if (execfn != 0) {
      return tryOpenExe(reinterpret_cast<const char*>(execfn));
    }
  #endif

  #if __linux__
    KJ_IF_MAYBE(link, fs.getRoot().tryReadlink(kj::Path({"proc", "self", "exe"}))) {
      return tryOpenExe(*link);
    }
  #endif

    // TODO(launch): Fall back to searching $PATH. Or on Mac, maybe use _NSGetExecutablePath()?
    return nullptr;
  }

  config::Config::Reader getConfig() {
    KJ_IF_MAYBE(c, config) {
      return *c;
    } else {
      // The optional `<const-name>` parameter must not have been given -- otherwise we would have
      // a non-null `config` by this point. See if we can infer the correct constant...
      if (topLevelConfigConstants.empty()) {
        context.exitError(
            "The config file does not define any top-level constants of type 'Config'.");
      } else if (topLevelConfigConstants.size() == 1) {
        return config.emplace(topLevelConfigConstants[0].as<config::Config>());
      } else {
        auto names = KJ_MAP(cnst, topLevelConfigConstants) {
          return cnst.getShortDisplayName();
        };
        context.exitError(kj::str(
            "The config file defines multiple top-level constants of type 'Config', so you must "
            "specify which one to use. The options are: ", kj::strArray(names, ", ")));
      }
    }
  }

  kj::Maybe<ExeInfo> exeInfo = getExecFile(context, *fs);

  bool hadErrors = false;

  void reportParsingError(kj::StringPtr file,
      capnp::SchemaFile::SourcePos start, capnp::SchemaFile::SourcePos end,
      kj::StringPtr message) override {
    if (start.line == end.line && start.column < end.column) {
      context.error(kj::str(
          file, ":", start.line+1, ":", start.column+1, "-", end.column+1,  ": ", message));
    } else {
      context.error(kj::str(
          file, ":", start.line+1, ":", start.column+1, ": ", message));
    }

    hadErrors = true;
  }
};

}  // namespace workerd::server

int main(int argc, char* argv[]) {
  ::kj::TopLevelProcessContext context(argv[0]);
  workerd::server::CliMain mainObject(context, argv);
  return ::kj::runMainAndExit(context, mainObject.getMain(), argc, argv);
}
