// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "container.h"

#include <workerd/api/http.h>
#include <workerd/api/streams/readable.h>
#include <workerd/api/streams/writable.h>
#include <workerd/api/system-streams.h>
#include <workerd/io/features.h>
#include <workerd/io/io-context.h>

#include <capnp/compat/byte-stream.h>
#include <kj/filesystem.h>

namespace workerd::api {

namespace {

kj::Maybe<kj::Path> parseRestorePath(kj::StringPtr path) {
  JSG_REQUIRE(path.size() > 0 && path[0] == '/', TypeError,
      "Directory snapshot restore path must be absolute. Got: ", path);

  try {
    auto parsed = kj::Path::parse(path.slice(1));
    if (parsed.size() == 0) {
      return kj::none;
    }
    return kj::mv(parsed);
  } catch (kj::Exception&) {
    JSG_FAIL_REQUIRE(
        TypeError, "Directory snapshot restore path contains invalid components: ", path);
  }
}

void requireValidEnvNameAndValue(kj::StringPtr name, kj::StringPtr value) {
  JSG_REQUIRE(name.findFirst('=') == kj::none, Error,
      "Environment variable names cannot contain '=': ", name);
  JSG_REQUIRE(name.findFirst('\0') == kj::none, Error,
      "Environment variable names cannot contain '\\0': ", name);
  JSG_REQUIRE(value.findFirst('\0') == kj::none, Error,
      "Environment variable values cannot contain '\\0': ", name);
}

kj::String getExecOutputMode(jsg::Optional<kj::String> maybeMode, kj::StringPtr kind) {
  auto mode = kj::mv(maybeMode).orDefault(kj::str("pipe"));
  JSG_REQUIRE(mode == "pipe" || mode == "ignore" || (kind == "stderr" && mode == "combined"),
      TypeError, "Invalid ", kind, " option: ", mode);
  return mode;
}

kj::Array<kj::byte> emptyByteArray() {
  return kj::heapArray<kj::byte>(0);
}

capnp::ByteStream::Client makeExecPipe(
    capnp::ByteStreamFactory& factory, kj::Own<kj::AsyncOutputStream> output) {
  return factory.kjToCapnp(capnp::ExplicitEndOutputStream::wrap(kj::mv(output), []() {}));
}

}  // namespace

// =======================================================================================
// ExecOutput / ExecProcess

ExecOutput::ExecOutput(
    kj::Array<kj::byte> stdoutBytes, kj::Array<kj::byte> stderrBytes, int exitCode)
    : stdoutBytes(kj::mv(stdoutBytes)),
      stderrBytes(kj::mv(stderrBytes)),
      exitCode(exitCode) {}

jsg::JsArrayBuffer ExecOutput::getStdout(jsg::Lock& js) {
  return jsg::JsArrayBuffer::create(js, stdoutBytes);
}

jsg::JsArrayBuffer ExecOutput::getStderr(jsg::Lock& js) {
  return jsg::JsArrayBuffer::create(js, stderrBytes);
}

ExecProcess::ExecProcess(jsg::Optional<jsg::Ref<WritableStream>> stdinStream,
    jsg::Optional<jsg::Ref<ReadableStream>> stdoutStream,
    jsg::Optional<jsg::Ref<ReadableStream>> stderrStream,
    int pid,
    rpc::Container::ProcessHandle::Client handle)
    : stdinStream(kj::mv(stdinStream)),
      stdoutStream(kj::mv(stdoutStream)),
      stderrStream(kj::mv(stderrStream)),
      pid(pid),
      handle(IoContext::current().addObject(kj::heap(kj::mv(handle)))) {}

jsg::Optional<jsg::Ref<WritableStream>> ExecProcess::getStdin() {
  return stdinStream.map([](jsg::Ref<WritableStream>& stream) { return stream.addRef(); });
}

jsg::Optional<jsg::Ref<ReadableStream>> ExecProcess::getStdout() {
  return stdoutStream.map([](jsg::Ref<ReadableStream>& stream) { return stream.addRef(); });
}

jsg::Optional<jsg::Ref<ReadableStream>> ExecProcess::getStderr() {
  return stderrStream.map([](jsg::Ref<ReadableStream>& stream) { return stream.addRef(); });
}

void ExecProcess::ensureExitCodePromise(jsg::Lock& js) {
  if (exitCodePromise != kj::none) {
    return;
  }

  // jsg::Promise is single-use. Keep the original Promise<int> as the public `exitCode`
  // property and a separate whenResolved() branch for helpers like output().
  auto self = JSG_THIS;
  auto promise = IoContext::current().awaitIo(js,
      handle->waitRequest(capnp::MessageSize{4, 0})
          .send()
          .then([self = kj::mv(self)](
                    capnp::Response<rpc::Container::ProcessHandle::WaitResults>&& results) mutable {
    auto exitCode = results.getExitCode();
    self->resolvedExitCode = exitCode;
    return exitCode;
  }));

  exitCodePromiseCopy = promise.whenResolved(js);
  exitCodePromise = jsg::MemoizedIdentity<jsg::Promise<int>>(kj::mv(promise));
}

jsg::Promise<int> ExecProcess::getExitCodeForOutput(jsg::Lock& js) {
  ensureExitCodePromise(js);

  KJ_IF_SOME(exitCode, resolvedExitCode) {
    return js.resolvedPromise(static_cast<int>(exitCode));
  }

  auto self = JSG_THIS;
  return KJ_ASSERT_NONNULL(exitCodePromiseCopy)
      .whenResolved(js)
      .then(js, [self = kj::mv(self)](jsg::Lock&) -> int {
    return static_cast<int>(KJ_ASSERT_NONNULL(self->resolvedExitCode));
  });
}

jsg::MemoizedIdentity<jsg::Promise<int>>& ExecProcess::getExitCode(jsg::Lock& js) {
  ensureExitCodePromise(js);
  return KJ_ASSERT_NONNULL(exitCodePromise);
}

jsg::Promise<jsg::Ref<ExecOutput>> ExecProcess::output(jsg::Lock& js) {
  JSG_REQUIRE(!outputCalled, TypeError, "output() can only be called once.");
  outputCalled = true;

  auto stdoutPromise = js.resolvedPromise(emptyByteArray());
  KJ_IF_SOME(stream, stdoutStream) {
    JSG_REQUIRE(!stream->isDisturbed(), TypeError,
        "Cannot call output() after stdout has started being consumed.");
    stdoutPromise =
        stream->getController()
            .readAllBytes(js, IoContext::current().getLimitEnforcer().getBufferingLimit())
            .then(js, [](jsg::Lock&, jsg::BufferSource bytes) {
      return kj::heapArray(bytes.asArrayPtr());
    });
  }

  auto stderrPromise = js.resolvedPromise(emptyByteArray());
  KJ_IF_SOME(stream, stderrStream) {
    JSG_REQUIRE(!stream->isDisturbed(), TypeError,
        "Cannot call output() after stderr has started being consumed.");
    stderrPromise = stream->getController()
                        .readAllBytes(js, kj::maxValue)
                        .then(js, [](jsg::Lock&, jsg::BufferSource bytes) {
      return kj::heapArray(bytes.asArrayPtr());
    });
  }

  auto exitCodePromise = getExitCodeForOutput(js);

  return stdoutPromise.then(js,
      [stderrPromise = kj::mv(stderrPromise), exitCodePromise = kj::mv(exitCodePromise)](
          jsg::Lock& js,
          kj::Array<kj::byte> stdoutBytes) mutable -> jsg::Promise<jsg::Ref<ExecOutput>> {
    return stderrPromise.then(js,
        [stdoutBytes = kj::mv(stdoutBytes), exitCodePromise = kj::mv(exitCodePromise)](
            jsg::Lock& js,
            kj::Array<kj::byte> stderrBytes) mutable -> jsg::Promise<jsg::Ref<ExecOutput>> {
      return exitCodePromise.then(js,
          [stdoutBytes = kj::mv(stdoutBytes), stderrBytes = kj::mv(stderrBytes)](
              jsg::Lock& js, int exitCode) mutable -> jsg::Ref<ExecOutput> {
        return js.alloc<ExecOutput>(kj::mv(stdoutBytes), kj::mv(stderrBytes), exitCode);
      });
    });
  });
}

void ExecProcess::kill(jsg::Lock& js, jsg::Optional<int> signal) {
  auto signo = signal.orDefault(15);
  JSG_REQUIRE(signo > 0 && signo <= 64, RangeError, "Invalid signal number.");

  auto req = handle->killRequest(capnp::MessageSize{4, 0});
  req.setSigno(signo);
  IoContext::current().addTask(req.sendIgnoringResult());
}
// =======================================================================================
// Basic lifecycle methods

Container::Container(rpc::Container::Client rpcClient, bool running)
    : rpcClient(IoContext::current().addObject(kj::heap(kj::mv(rpcClient)))),
      running(running) {}

void Container::start(jsg::Lock& js, jsg::Optional<StartupOptions> maybeOptions) {
  auto flags = FeatureFlags::get(js);
  JSG_REQUIRE(!running, Error, "start() cannot be called on a container that is already running.");

  StartupOptions options = kj::mv(maybeOptions).orDefault({});

  auto req = rpcClient->startRequest();
  KJ_IF_SOME(entrypoint, options.entrypoint) {
    auto list = req.initEntrypoint(entrypoint.size());
    for (auto i: kj::indices(entrypoint)) {
      list.set(i, entrypoint[i]);
    }
  }
  req.setEnableInternet(options.enableInternet);

  KJ_IF_SOME(env, options.env) {
    auto list = req.initEnvironmentVariables(env.fields.size());
    for (auto i: kj::indices(env.fields)) {
      auto field = &env.fields[i];
      requireValidEnvNameAndValue(field->name, field->value);

      list.set(i, str(field->name, "=", field->value));
    }
  }

  if (flags.getWorkerdExperimental()) {
    KJ_IF_SOME(hardTimeoutMs, options.hardTimeout) {
      JSG_REQUIRE(hardTimeoutMs > 0, RangeError, "Hard timeout must be greater than 0");
      req.setHardTimeoutMs(hardTimeoutMs);
    }
  }

  KJ_IF_SOME(labels, options.labels) {
    auto list = req.initLabels(labels.fields.size());
    for (auto i: kj::indices(labels.fields)) {
      auto& field = labels.fields[i];
      JSG_REQUIRE(field.name.size() > 0, Error, "Label names cannot be empty");
      for (auto c: field.name) {
        JSG_REQUIRE(static_cast<kj::byte>(c) >= 0x20, Error,
            "Label names cannot contain control characters (index ", i, ")");
      }
      for (auto c: field.value) {
        JSG_REQUIRE(static_cast<kj::byte>(c) >= 0x20, Error,
            "Label values cannot contain control characters (index ", i, ")");
      }
      list[i].setName(field.name);
      list[i].setValue(field.value);
    }
  }

  KJ_IF_SOME(directorySnapshots, options.directorySnapshots) {
    auto list = req.initDirectorySnapshots(directorySnapshots.size());
    for (auto i: kj::indices(directorySnapshots)) {
      auto entry = list[i];
      auto& restore = directorySnapshots[i];
      auto& snap = restore.snapshot;
      auto effectiveRestorePath = snap.dir.asPtr();
      KJ_IF_SOME(mp, restore.mountPoint) {
        effectiveRestorePath = mp.asPtr();
      }

      JSG_REQUIRE_NONNULL(parseRestorePath(effectiveRestorePath), Error,
          "Directory snapshot cannot be restored to root directory.");

      entry.setSnapshotId(snap.id);
      entry.setRestorePath(effectiveRestorePath);
    }
  }

  KJ_IF_SOME(containerSnapshot, options.containerSnapshot) {
    req.setContainerSnapshotId(containerSnapshot.id);
  }

  req.setCompatibilityFlags(flags);

  IoContext::current().addTask(req.sendIgnoringResult());

  running = true;
}

jsg::Promise<kj::Maybe<Container::Info>> Container::inspect(jsg::Lock& js) {
  return IoContext::current().awaitIo(js, rpcClient->inspectRequest().send(),
      [](jsg::Lock& js,
          capnp::Response<rpc::Container::InspectResults> results) -> kj::Maybe<Info> {
    auto info = results.getInfo();
    if (info.isNone()) {
      return kj::none;
    }
    return Info{
      .labels =
          jsg::Dict<kj::String>{
            .fields =
                KJ_MAP(label, info.getStarted().getLabels()) {
      return jsg::Dict<kj::String>::Field{
        .name = kj::str(label.getName()),
        .value = kj::str(label.getValue()),
      };
    },
          },
    };
  });
}

jsg::Promise<Container::DirectorySnapshot> Container::snapshotDirectory(
    jsg::Lock& js, DirectorySnapshotOptions options) {
  JSG_REQUIRE(
      running, Error, "snapshotDirectory() cannot be called on a container that is not running.");
  JSG_REQUIRE(options.dir.size() > 0 && options.dir.startsWith("/"), TypeError,
      "snapshotDirectory() requires an absolute directory path (starting with '/').");

  auto req = rpcClient->snapshotDirectoryRequest();
  req.setDir(options.dir);

  KJ_IF_SOME(name, options.name) {
    req.setName(name);
  }

  return IoContext::current()
      .awaitIo(js, req.send())
      .then(
          js, [](jsg::Lock& js, capnp::Response<rpc::Container::SnapshotDirectoryResults> results) {
    auto snapshot = results.getSnapshot();
    JSG_REQUIRE(snapshot.getSize() <= jsg::MAX_SAFE_INTEGER, RangeError,
        "Snapshot size exceeds Number.MAX_SAFE_INTEGER");

    jsg::Optional<kj::String> name = kj::none;
    if (snapshot.getName().size() > 0) {
      name = kj::str(snapshot.getName());
    }

    return Container::DirectorySnapshot{kj::str(snapshot.getId()),
      static_cast<double>(snapshot.getSize()), kj::str(snapshot.getDir()), kj::mv(name)};
  });
}

jsg::Promise<Container::Snapshot> Container::snapshotContainer(
    jsg::Lock& js, SnapshotOptions options) {
  JSG_REQUIRE(
      running, Error, "snapshotContainer() cannot be called on a container that is not running.");

  auto req = rpcClient->snapshotContainerRequest();

  KJ_IF_SOME(name, options.name) {
    req.setName(name);
  }

  return IoContext::current()
      .awaitIo(js, req.send())
      .then(
          js, [](jsg::Lock& js, capnp::Response<rpc::Container::SnapshotContainerResults> results) {
    auto snapshot = results.getSnapshot();
    JSG_REQUIRE(snapshot.getSize() <= jsg::MAX_SAFE_INTEGER, RangeError,
        "Snapshot size exceeds Number.MAX_SAFE_INTEGER");

    jsg::Optional<kj::String> name = kj::none;
    if (snapshot.getName().size() > 0) {
      name = kj::str(snapshot.getName());
    }

    return Container::Snapshot{
      kj::str(snapshot.getId()), static_cast<double>(snapshot.getSize()), kj::mv(name)};
  });
}

jsg::Promise<void> Container::setInactivityTimeout(jsg::Lock& js, int64_t durationMs) {
  JSG_REQUIRE(
      durationMs > 0, TypeError, "setInactivityTimeout() cannot be called with a durationMs <= 0");

  auto req = rpcClient->setInactivityTimeoutRequest();

  req.setDurationMs(durationMs);
  return IoContext::current().awaitIo(js, req.sendIgnoringResult());
}

jsg::Promise<void> Container::interceptOutboundHttp(
    jsg::Lock& js, kj::String addr, jsg::Ref<Fetcher> binding) {
  auto& ioctx = IoContext::current();
  auto channel = binding->getSubrequestChannel(ioctx);

  // Get a channel token for RPC usage, the container runtime can use this
  // token later to redeem a Fetcher.
  auto token = channel->getToken(IoChannelFactory::ChannelTokenUsage::RPC);

  auto req = rpcClient->setEgressHttpRequest();
  req.setHostPort(addr);
  req.setChannelToken(token);
  return ioctx.awaitIo(js, req.sendIgnoringResult());
}

jsg::Promise<void> Container::interceptAllOutboundHttp(jsg::Lock& js, jsg::Ref<Fetcher> binding) {
  auto& ioctx = IoContext::current();
  auto channel = binding->getSubrequestChannel(ioctx);
  auto token = channel->getToken(IoChannelFactory::ChannelTokenUsage::RPC);

  // Register for all IPv4 and IPv6 addresses (on port 80)
  auto reqV4 = rpcClient->setEgressHttpRequest();
  reqV4.setHostPort("0.0.0.0/0"_kj);
  reqV4.setChannelToken(token);

  auto reqV6 = rpcClient->setEgressHttpRequest();
  reqV6.setHostPort("::/0"_kj);
  reqV6.setChannelToken(token);

  return ioctx.awaitIo(js,
      kj::joinPromisesFailFast(kj::arr(reqV4.sendIgnoringResult(), reqV6.sendIgnoringResult())));
}

jsg::Promise<void> Container::interceptOutboundHttps(
    jsg::Lock& js, kj::String addr, jsg::Ref<Fetcher> binding) {
  auto& ioctx = IoContext::current();
  auto channel = binding->getSubrequestChannel(ioctx);
  auto token = channel->getToken(IoChannelFactory::ChannelTokenUsage::RPC);

  auto req = rpcClient->setEgressHttpsRequest();
  req.setHostPort(addr);
  req.setChannelToken(token);

  return ioctx.awaitIo(js, req.sendIgnoringResult());
}

jsg::Promise<jsg::Ref<ExecProcess>> Container::exec(
    jsg::Lock& js, kj::Array<kj::String> cmd, jsg::Optional<ExecOptions> maybeOptions) {
  JSG_REQUIRE(running, Error, "exec() cannot be called on a container that is not running.");
  JSG_REQUIRE(cmd.size() > 0, TypeError, "exec() requires a non-empty command array.");

  auto options = kj::mv(maybeOptions).orDefault({});
  auto stdoutMode = getExecOutputMode(kj::mv(options.$stdout), "stdout");
  auto stderrMode = getExecOutputMode(kj::mv(options.$stderr), "stderr");
  bool combinedOutput = stderrMode == "combined";
  JSG_REQUIRE(!combinedOutput || stdoutMode == "pipe", TypeError,
      "stderr: \"combined\" requires stdout to be \"pipe\".");

  auto& ioContext = IoContext::current();
  auto& byteStreamFactory = ioContext.getByteStreamFactory();

  auto req = rpcClient->execRequest();
  auto cmdList = req.initCmd(cmd.size());
  for (auto i: kj::indices(cmd)) {
    cmdList.set(i, cmd[i]);
  }

  // Init the kj pipes to create the stdout/err bytestreams
  kj::Maybe<kj::Own<kj::AsyncInputStream>> stdoutInput;
  if (stdoutMode == "pipe") {
    auto pipe = kj::newOneWayPipe();
    req.setStdoutWriter(makeExecPipe(byteStreamFactory, kj::mv(pipe.out)));
    stdoutInput = kj::mv(pipe.in);
  }

  kj::Maybe<kj::Own<kj::AsyncInputStream>> stderrInput;
  if (!combinedOutput && stderrMode == "pipe") {
    auto pipe = kj::newOneWayPipe();
    req.setStderrWriter(makeExecPipe(byteStreamFactory, kj::mv(pipe.out)));
    stderrInput = kj::mv(pipe.in);
  }

  auto params = req.initParams();
  params.setCombinedOutput(combinedOutput);

  // Some basic validation...
  KJ_IF_SOME(cwd, options.cwd) {
    JSG_REQUIRE(cwd.findFirst('\0') == kj::none, TypeError, "cwd cannot contain '\\0' characters.");
    params.setWorkingDirectory(cwd);
  }

  KJ_IF_SOME(user, options.user) {
    JSG_REQUIRE(
        user.findFirst('\0') == kj::none, TypeError, "user cannot contain '\\0' characters.");
    params.setUser(user);
  }

  KJ_IF_SOME(env, options.env) {
    auto envList = params.initEnv(env.fields.size());
    for (auto i: kj::indices(env.fields)) {
      auto field = &env.fields[i];
      requireValidEnvNameAndValue(field->name, field->value);
      envList.set(i, str(field->name, "=", field->value));
    }
  }

  // We have to await, because PID won't be available until the response resolves
  return ioContext.awaitIo(js, req.send())
      .then(js,
          [&ioContext, &byteStreamFactory, options = kj::mv(options),
              stdoutInput = kj::mv(stdoutInput), stderrInput = kj::mv(stderrInput)](
              jsg::Lock& js, capnp::Response<rpc::Container::ExecResults> results) mutable
          -> jsg::Ref<ExecProcess> {
    auto process = results.getProcess();
    auto handle = process.getHandle();
    auto pid = process.getPid();

    // Init the ReadableStreams (stdout/stderr)
    jsg::Optional<jsg::Ref<ReadableStream>> stdoutStream = kj::none;
    KJ_IF_SOME(input, stdoutInput) {
      auto source = newSystemStream(kj::mv(input), StreamEncoding::IDENTITY, ioContext);
      stdoutStream = js.alloc<ReadableStream>(ioContext, kj::mv(source));
    }

    // stderrInput is only set if using "pipe" on stderr and not "combined"
    jsg::Optional<jsg::Ref<ReadableStream>> stderrStream = kj::none;
    KJ_IF_SOME(input, stderrInput) {
      auto source = newSystemStream(kj::mv(input), StreamEncoding::IDENTITY, ioContext);
      stderrStream = js.alloc<ReadableStream>(ioContext, kj::mv(source));
    }

    jsg::Optional<jsg::Ref<WritableStream>> stdinStream = kj::none;

    // If stdin is undefined, the JS API promises immediate EOF. We still use the pipelined stdin()
    // capability so exec() doesn't wait on an extra round-trip.
    KJ_IF_SOME(stdinOption, options.$stdin) {
      auto stdinRequest = handle.stdinWriterRequest(capnp::MessageSize{4, 0});
      // Get the stdinWriter() ByteStream, use the pipelined capability
      auto stdinPipeline = stdinRequest.send();
      // ... adapt bytestream into a writer
      auto stdinWriter = byteStreamFactory.capnpToKjExplicitEnd(stdinPipeline.getWriter());

      KJ_SWITCH_ONEOF(stdinOption) {
        // user sets ReadableStream...
        KJ_CASE_ONEOF(readable, jsg::Ref<ReadableStream>) {
          auto sink = newSystemStream(kj::mv(stdinWriter), StreamEncoding::IDENTITY, ioContext);
          auto pipePromise =
              (ioContext.waitForDeferredProxy(readable->pumpTo(js, kj::mv(sink), true)));
          ioContext.addTask(pipePromise.attach(readable.addRef()));
        }
        // user sets "pipe"... they want to consume the API with the stdin WritableStream
        KJ_CASE_ONEOF(mode, kj::String) {
          JSG_REQUIRE(
              mode == "pipe", TypeError, "stdin must be a ReadableStream or the string \"pipe\".");
          auto sink = newSystemStream(kj::mv(stdinWriter), StreamEncoding::IDENTITY, ioContext);
          auto writable = js.alloc<WritableStream>(ioContext, kj::mv(sink),
              ioContext.getMetrics().tryCreateWritableByteStreamObserver());
          stdinStream = kj::mv(writable);
        }
      }

      // all good, we have the stdinStream set
    } else {
      auto stdinRequest = handle.stdinWriterRequest(capnp::MessageSize{4, 0});
      auto stdinPipeline = stdinRequest.send();
      auto stdinWriter = byteStreamFactory.capnpToKjExplicitEnd(stdinPipeline.getWriter());
      ioContext.addTask(stdinWriter->end().attach(kj::mv(stdinWriter)));
    }

    // return the instance to the process after getting pipeline of the process handle
    return js.alloc<ExecProcess>(
        kj::mv(stdinStream), kj::mv(stdoutStream), kj::mv(stderrStream), pid, kj::mv(handle));
  });
}

jsg::Promise<void> Container::interceptOutboundTcp(
    jsg::Lock& js, kj::String addr, jsg::Ref<Fetcher> binding) {
  auto& ioctx = IoContext::current();
  auto channel = binding->getSubrequestChannel(ioctx);

  // Get a channel token for RPC usage, the container runtime can use this
  // token later to redeem a Fetcher whose connect() handler processes the TCP stream.
  auto token = channel->getToken(IoChannelFactory::ChannelTokenUsage::RPC);

  auto req = rpcClient->setEgressTcpRequest();
  req.setHostPort(addr);
  req.setChannelToken(token);
  return ioctx.awaitIo(js, req.sendIgnoringResult());
}

jsg::Promise<void> Container::monitor(jsg::Lock& js) {
  JSG_REQUIRE(running, Error, "monitor() cannot be called on a container that is not running.");

  return IoContext::current()
      .awaitIo(js, rpcClient->monitorRequest(capnp::MessageSize{4, 0}).send())
      .then(js, [this](jsg::Lock& js, capnp::Response<rpc::Container::MonitorResults> results) {
    running = false;
    auto exitCode = results.getExitCode();
    KJ_IF_SOME(d, destroyReason) {
      jsg::Value error = kj::mv(d);
      destroyReason = kj::none;
      js.throwException(kj::mv(error));
    }

    if (exitCode != 0) {
      auto err = js.error(kj::str("Container exited with unexpected exit code: ", exitCode));
      KJ_ASSERT_NONNULL(err.tryCast<jsg::JsObject>()).set(js, "exitCode", js.num(exitCode));
      js.throwException(err);
    }
  }, [this](jsg::Lock& js, jsg::Value&& error) {
    running = false;
    destroyReason = kj::none;
    js.throwException(kj::mv(error));
  });
}

jsg::Promise<void> Container::destroy(jsg::Lock& js, jsg::Optional<jsg::Value> error) {
  if (!running) return js.resolvedPromise();

  if (destroyReason == kj::none) {
    destroyReason = kj::mv(error);
  }

  return IoContext::current().awaitIo(
      js, rpcClient->destroyRequest(capnp::MessageSize{4, 0}).sendIgnoringResult());
}

void Container::signal(jsg::Lock& js, int signo) {
  JSG_REQUIRE(signo > 0 && signo <= 64, RangeError, "Invalid signal number.");
  JSG_REQUIRE(running, Error, "signal() cannot be called on a container that is not running.");

  auto req = rpcClient->signalRequest(capnp::MessageSize{4, 0});
  req.setSigno(signo);
  IoContext::current().addTask(req.sendIgnoringResult());
}

// =======================================================================================
// getTcpPort()

// `getTcpPort()` returns a `Fetcher`, on which `fetch()` and `connect()` can be called. `Fetcher`
// is a JavaScript wrapper around `WorkerInterface`, so we need to implement that.
class Container::TcpPortWorkerInterface final: public WorkerInterface {
 public:
  TcpPortWorkerInterface(capnp::ByteStreamFactory& byteStreamFactory,
      kj::EntropySource& entropySource,
      const kj::HttpHeaderTable& headerTable,
      rpc::Container::Port::Client port)
      : byteStreamFactory(byteStreamFactory),
        entropySource(entropySource),
        headerTable(headerTable),
        port(kj::mv(port)) {}

  // Implements fetch(), i.e., HTTP requests. We form a TCP connection, then run HTTP over it
  // (as opposed to, say, speaking http-over-capnp to the container service).
  kj::Promise<void> request(kj::HttpMethod method,
      kj::StringPtr url,
      const kj::HttpHeaders& headers,
      kj::AsyncInputStream& requestBody,
      kj::HttpService::Response& response) override {
    // URLs should have been validated earlier in the stack, so parsing the URL should succeed.
    auto parsedUrl = KJ_REQUIRE_NONNULL(kj::Url::tryParse(url, kj::Url::Context::HTTP_PROXY_REQUEST,
                                            {.percentDecode = false, .allowEmpty = true}),
        "invalid url?", url);

    // We don't support TLS.
    JSG_REQUIRE(parsedUrl.scheme != "https", Error,
        "Connecting to a container using HTTPS is not currently supported; use HTTP instead. "
        "TLS is unnecessary anyway, as the connection is already secure by default.");

    // Schemes other than http: and https: should have been rejected earlier, but let's verify.
    KJ_REQUIRE(parsedUrl.scheme == "http");

    // We need to convert the URL from proxy format (full URL in request line) to host format
    // (path in request line, hostname in Host header).
    auto newHeaders = headers.cloneShallow();
    newHeaders.setPtr(kj::HttpHeaderId::HOST, parsedUrl.host);
    auto noHostUrl = parsedUrl.toString(kj::Url::Context::HTTP_REQUEST);

    // Make a TCP connection...
    auto pipe = kj::newTwoWayPipe();
    kj::Maybe<kj::Exception> connectionException = kj::none;

    auto connectionPromise = connectImpl(*pipe.ends[1]);

    // ... and then stack an HttpClient on it ...
    auto client = kj::newHttpClient(headerTable, *pipe.ends[0], {.entropySource = entropySource});

    // ... and then adapt that to an HttpService ...
    auto service = kj::newHttpService(*client);

    // ... fork connection promises so we can keep the original exception around ...
    auto connectionPromiseForked = connectionPromise.fork();
    auto connectionPromiseBranch = connectionPromiseForked.addBranch();
    auto connectionPromiseToKeepException = connectionPromiseForked.addBranch();

    // ... and now we can just forward our call to that ...
    try {
      co_await service->request(method, noHostUrl, newHeaders, requestBody, response)
          .exclusiveJoin(
              // never done as we do not want a Connection RPC exiting successfully
              // affecting the request
              connectionPromiseBranch.then([]() -> kj::Promise<void> { return kj::NEVER_DONE; }));
    } catch (...) {
      auto exception = kj::getCaughtExceptionAsKj();
      connectionException = kj::some(kj::mv(exception));
    }

    // ... and last but not least, if the connect() call succeeded but the connection
    // was broken, we throw that exception.
    KJ_IF_SOME(exception, connectionException) {
      co_await connectionPromiseToKeepException;
      kj::throwFatalException(kj::mv(exception));
    }
  }

  // Implements connect(), i.e., forms a raw socket.
  kj::Promise<void> connect(kj::StringPtr host,
      const kj::HttpHeaders& headers,
      kj::AsyncIoStream& connection,
      ConnectResponse& response,
      kj::HttpConnectSettings settings) override {
    JSG_REQUIRE(!settings.useTls, Error,
        "Connencting to a container using TLS is not currently supported. It is unnecessary "
        "anyway, as the connection is already secure by default.");

    auto promise = connectImpl(connection);

    kj::HttpHeaders responseHeaders(headerTable);
    response.accept(200, "OK", responseHeaders);

    return promise;
  }

  // The only `CustomEvent` that can happen through `Fetcher` is a JSRPC call. Maybe we will
  // support this someday? But not today.
  kj::Promise<CustomEvent::Result> customEvent(kj::Own<CustomEvent> event) override {
    return event->notSupported();
  }

  // There's no way to invoke the remaining event types via `Fetcher`.
  kj::Promise<void> prewarm(kj::StringPtr url) override {
    KJ_UNREACHABLE;
  }
  kj::Promise<ScheduledResult> runScheduled(kj::Date scheduledTime, kj::StringPtr cron) override {
    KJ_UNREACHABLE;
  }
  kj::Promise<AlarmResult> runAlarm(kj::Date scheduledTime, uint32_t retryCount) override {
    KJ_UNREACHABLE;
  }

 private:
  capnp::ByteStreamFactory& byteStreamFactory;
  kj::EntropySource& entropySource;
  const kj::HttpHeaderTable& headerTable;
  rpc::Container::Port::Client port;

  // Connect to the port and pump bytes to/from `connection`. Used by both request() and
  // connect().
  kj::Promise<void> connectImpl(kj::AsyncIoStream& connection) {
    // A lot of the following is copied from
    // capnp::HttpOverCapnpFactory::KjToCapnpHttpServiceAdapter::connect().
    auto req = port.connectRequest(capnp::MessageSize{4, 1});
    auto downPipe = kj::newOneWayPipe();
    req.setDown(byteStreamFactory.kjToCapnp(kj::mv(downPipe.out)));
    auto pipeline = req.send();

    // Make sure the request message isn't pinned into memory through the co_await below.
    { auto drop = kj::mv(req); }

    auto downPumpTask =
        downPipe.in->pumpTo(connection)
            .then([&connection, down = kj::mv(downPipe.in)](uint64_t) -> kj::Promise<void> {
      connection.shutdownWrite();
      return kj::NEVER_DONE;
    });
    auto up = pipeline.getUp();

    auto upStream = byteStreamFactory.capnpToKjExplicitEnd(up);
    auto upPumpTask = connection.pumpTo(*upStream)
                          .then([&upStream = *upStream](uint64_t) mutable {
      return upStream.end();
    }).then([up = kj::mv(up), upStream = kj::mv(upStream)]() mutable -> kj::Promise<void> {
      return kj::NEVER_DONE;
    });

    co_await pipeline.ignoreResult();
    co_await kj::joinPromisesFailFast(kj::arr(kj::mv(upPumpTask), kj::mv(downPumpTask)));
  }
};

// `Fetcher` actually wants us to give it a factory that creates a new `WorkerInterface` for each
// request, so this is that.
class Container::TcpPortOutgoingFactory final: public Fetcher::OutgoingFactory {
 public:
  TcpPortOutgoingFactory(capnp::ByteStreamFactory& byteStreamFactory,
      kj::EntropySource& entropySource,
      const kj::HttpHeaderTable& headerTable,
      rpc::Container::Port::Client port)
      : byteStreamFactory(byteStreamFactory),
        entropySource(entropySource),
        headerTable(headerTable),
        port(kj::mv(port)) {}

  kj::Own<WorkerInterface> newSingleUseClient(kj::Maybe<kj::String> cfStr) override {
    // At present we have no use for `cfStr`.
    return IoContext::current().getSubrequestNoChecks([&](auto& tracing, auto& channelFactory) {
      return kj::heap<TcpPortWorkerInterface>(byteStreamFactory, entropySource, headerTable, port);
    }, {.inHouse = false, .wrapMetrics = false});
  }

 private:
  capnp::ByteStreamFactory& byteStreamFactory;
  kj::EntropySource& entropySource;
  const kj::HttpHeaderTable& headerTable;
  rpc::Container::Port::Client port;
};

jsg::Ref<Fetcher> Container::getTcpPort(jsg::Lock& js, int port) {
  JSG_REQUIRE(port > 0 && port < 65536, TypeError, "Invalid port number: ", port);

  auto req = rpcClient->getTcpPortRequest(capnp::MessageSize{4, 0});
  req.setPort(port);

  auto& ioctx = IoContext::current();

  kj::Own<Fetcher::OutgoingFactory> factory =
      kj::heap<TcpPortOutgoingFactory>(ioctx.getByteStreamFactory(), ioctx.getEntropySource(),
          ioctx.getHeaderTable(), req.send().getPort());

  return js.alloc<Fetcher>(
      ioctx.addObject(kj::mv(factory)), Fetcher::RequiresHostAndProtocol::YES, true);
}

}  // namespace workerd::api
