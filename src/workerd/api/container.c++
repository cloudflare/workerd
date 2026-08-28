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
#include <workerd/util/autogate.h>

#include <capnp/compat/byte-stream.h>
#include <kj/filesystem.h>
#include <kj/refcount.h>

#include <cmath>

namespace workerd::api {

namespace {

// Upper bound on the number of distinct container ports whose tunnels we pool per Container. This
// is a hard admission cap: once this many ports are pooled, any further distinct port falls back
// to the non-pooled connect path (a fresh tunnel per request) for the Container's lifetime.
// TODO(perf): This has no eviction policy or observability. Consider LRU eviction of cold entries
//   and/or a debug log or metric when the cap is hit, so ports beyond the cap are not silently and
//   permanently relegated to the slower path.
constexpr size_t MAX_CACHED_TCP_PORTS = 4;

constexpr size_t MAX_IMAGE_REFERENCE_SIZE = 4096;
constexpr kj::StringPtr VALID_CONTAINER_INSTANCE_TYPES[] = {
  "lite"_kj, "standard-1"_kj, "standard-2"_kj, "standard-3"_kj, "standard-4"_kj};

// JS numbers are doubles, but the wire format is UInt64.
uint64_t requireResourceAmount(double value, kj::StringPtr name) {
  JSG_REQUIRE(std::isfinite(value) && value > 0, RangeError, "Container resource ", name,
      " must be a finite number greater than 0.");
  JSG_REQUIRE(
      value == std::floor(value), RangeError, "Container resource ", name, " must be an integer.");
  JSG_REQUIRE(value <= static_cast<double>(jsg::MAX_SAFE_INTEGER), RangeError,
      "Container resource ", name, " exceeds Number.MAX_SAFE_INTEGER.");
  return static_cast<uint64_t>(value);
}

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

constexpr size_t MAX_CONTAINER_LABELS = 10;
constexpr size_t MAX_CONTAINER_LABEL_NAME_BYTES = 16;
constexpr size_t MAX_CONTAINER_LABEL_VALUE_BYTES = 64;

void requireValidLabels(const jsg::Dict<kj::String>& labels) {
  JSG_REQUIRE(labels.fields.size() <= MAX_CONTAINER_LABELS, Error, "Cannot specify more than ",
      MAX_CONTAINER_LABELS, " container labels");
  for (auto i: kj::indices(labels.fields)) {
    const auto& field = labels.fields[i];
    JSG_REQUIRE(field.name.size() > 0, Error, "Label names cannot be empty");
    JSG_REQUIRE(field.name.size() <= MAX_CONTAINER_LABEL_NAME_BYTES, Error,
        "Label names cannot exceed ", MAX_CONTAINER_LABEL_NAME_BYTES, " bytes (index ", i, ")");
    JSG_REQUIRE(field.value.size() <= MAX_CONTAINER_LABEL_VALUE_BYTES, Error,
        "Label values cannot exceed ", MAX_CONTAINER_LABEL_VALUE_BYTES, " bytes (index ", i, ")");
    for (auto c: field.name) {
      JSG_REQUIRE(static_cast<kj::byte>(c) >= 0x20, Error,
          "Label names cannot contain control characters (index ", i, ")");
    }
    for (auto c: field.value) {
      JSG_REQUIRE(static_cast<kj::byte>(c) >= 0x20, Error,
          "Label values cannot contain control characters (index ", i, ")");
    }
  }
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

ExecProcess::ExecProcess(jsg::Lock& js,
    IoContext& ioContext,
    jsg::Optional<JsWritableStream> stdinStream,
    jsg::Optional<JsReadableStream> stdoutStream,
    jsg::Optional<JsReadableStream> stderrStream,
    int pid,
    rpc::Container::ProcessHandle::Client handle,
    IsPty isPty,
    kj::Maybe<jsg::Ref<AbortSignal>> abortSignal)
    : stdinStream(kj::mv(stdinStream)),
      stdoutStream(kj::mv(stdoutStream)),
      stderrStream(kj::mv(stderrStream)),
      pid(pid),
      isPty(isPty.toBool()),
      handle(ioContext.addObject(kj::heap(kj::mv(handle)))) {
  KJ_IF_SOME(signal, abortSignal) {
    constexpr int kSigKill = 9;

    // exec() calls throwIfAborted() before sending the RPC, but the signal can still fire while the
    // RPC is in flight, i.e. before this constructor runs in the RPC's continuation. If that
    // happened, kill the freshly-started process immediately; there's no point registering an
    // abort action.
    if (signal->getAborted(js)) {
      sendKill(kSigKill);
    } else {
      abortRegistration = signal->addAbortAction(
          js, [self = JSG_THIS_WEAK(js)](jsg::Lock& js, const kj::Exception&) {
        KJ_IF_SOME(process, self.tryGet()) {
          process.sendKill(kSigKill);
        }
      });
    }
  }
}

jsg::Optional<JsWritableStream> ExecProcess::getStdin(jsg::Lock& js) {
  return stdinStream.map([&](JsWritableStream& stream) { return stream.addRef(js); });
}

jsg::Optional<JsReadableStream> ExecProcess::getStdout(jsg::Lock& js) {
  return stdoutStream.map([&](JsReadableStream& stream) { return stream.addRef(js); });
}

jsg::Optional<JsReadableStream> ExecProcess::getStderr(jsg::Lock& js) {
  return stderrStream.map([&](JsReadableStream& stream) { return stream.addRef(js); });
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
    JSG_REQUIRE(!stream.isDisturbed(js), TypeError,
        "Cannot call output() after stdout has started being consumed.");
    stdoutPromise =
        stream.arrayBuffer(js, IoContext::current().getLimitEnforcer().getBufferingLimit())
            .then(js, [](jsg::Lock& js, jsg::JsRef<jsg::JsArrayBuffer> bytes) {
      return bytes.getHandle(js).copy();
    });
  }

  auto stderrPromise = js.resolvedPromise(emptyByteArray());
  KJ_IF_SOME(stream, stderrStream) {
    JSG_REQUIRE(!stream.isDisturbed(js), TypeError,
        "Cannot call output() after stderr has started being consumed.");
    stderrPromise = stream.arrayBuffer(js, kj::maxValue)
                        .then(js, [](jsg::Lock& js, jsg::JsRef<jsg::JsArrayBuffer> bytes) {
      return bytes.getHandle(js).copy();
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
  sendKill(signo);
}

void ExecProcess::sendKill(int signo) {
  // Grab the IoContext first so we fail fast if there is no active IoContext.
  auto& ioContext = IoContext::current();
  auto req = handle->killRequest(capnp::MessageSize{4, 0});
  req.setSigno(signo);
  ioContext.addTask(req.sendIgnoringResult());
}

void ExecProcess::resize(jsg::Lock& js, int cols, int rows) {
  // PTY dimensions are UInt16 on the wire, so the largest value either dimension can take is 65535.
  static constexpr int kMaxPtyDimension = 65535;

  JSG_REQUIRE(
      isPty, TypeError, "resize() can only be called on a process that was started with a PTY.");
  JSG_REQUIRE(cols >= 1 && cols <= kMaxPtyDimension, RangeError, "PTY cols must be between 1 and ",
      kMaxPtyDimension, ".");
  JSG_REQUIRE(rows >= 1 && rows <= kMaxPtyDimension, RangeError, "PTY rows must be between 1 and ",
      kMaxPtyDimension, ".");

  // TODO: This is fire-and-forget with no coalescing or rate-limiting
  auto& ioContext = IoContext::current();
  auto req = handle->resizeRequest(capnp::MessageSize{4, 0});
  req.setCols(cols);
  req.setRows(rows);
  ioContext.addTask(req.sendIgnoringResult());
}
// =======================================================================================
// Basic lifecycle methods

Container::Container(rpc::Container::Client rpcClient, bool running)
    : rpcClient(IoContext::current().addObject(kj::heap(kj::mv(rpcClient)))) {
  if (running) startMonitor();
}

kj::Promise<void> Container::Monitor::observe(kj::ForkedPromise<int32_t>& promise, bool& running) {
  auto markStopped = kj::defer([&running]() { running = false; });
  return promise.addBranch().then([](int32_t) {}, [](kj::Exception&&) {
  }).attach(kj::mv(markStopped));
}

Container::Monitor::Monitor(kj::ForkedPromise<int32_t> promise, uint64_t generation)
    : generation(generation),
      promise(kj::mv(promise)),
      stopped(observe(this->promise, running).fork()),
      background(stopped.addBranch().eagerlyEvaluate(nullptr)) {}

bool Container::getRunning() {
  KJ_IF_SOME(monitor, currentMonitor) {
    return monitor->running;
  }
  return false;
}

bool Container::isCurrentMonitor(uint64_t generation) {
  KJ_IF_SOME(monitor, currentMonitor) {
    return monitor->generation == generation;
  }
  return false;
}

void Container::start(jsg::Lock& js, jsg::Optional<StartupOptions> maybeOptions) {
  auto flags = FeatureFlags::get(js);
  JSG_REQUIRE(
      !getRunning(), Error, "start() cannot be called on a container that is already running.");
  invalidateTcpPortStates();

  StartupOptions options = kj::mv(maybeOptions).orDefault({});

  auto req = rpcClient->startRequest();
  KJ_IF_SOME(spanContext, IoContext::current().getCurrentTraceSpan().toSpanContext()) {
    spanContext.toCapnp(req.initSpanContext());
  }
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

  JSG_REQUIRE(options.image == kj::none || options.containerSnapshot == kj::none, TypeError,
      "`image` and `containerSnapshot` are mutually exclusive.");
  if (flags.getWorkerdExperimental()) {
    KJ_IF_SOME(hardTimeoutMs, options.hardTimeout) {
      JSG_REQUIRE(hardTimeoutMs > 0, RangeError, "Hard timeout must be greater than 0");
      req.setHardTimeoutMs(hardTimeoutMs);
    }
  }
  KJ_IF_SOME(image, options.image) {
    JSG_REQUIRE(image.size() <= MAX_IMAGE_REFERENCE_SIZE, TypeError,
        "Container image reference cannot exceed ", MAX_IMAGE_REFERENCE_SIZE, " bytes.");
    for (auto c: image) {
      auto byte = static_cast<kj::byte>(c);
      JSG_REQUIRE(byte > 0x20 && byte < 0x7f, TypeError,
          "Container image reference must contain only non-space printable ASCII characters.");
    }
    req.getSource().setImage(image);
  }
  KJ_IF_SOME(instance, options.instance) {
    auto instanceBuilder = req.initInstance();
    KJ_SWITCH_ONEOF(instance) {
      KJ_CASE_ONEOF(named, kj::String) {
        JSG_REQUIRE(
            kj::arrayPtr(VALID_CONTAINER_INSTANCE_TYPES).findFirst(named.asPtr()) != kj::none,
            TypeError, "Invalid container instance type.");
        instanceBuilder.setNamed(named);
      }
      KJ_CASE_ONEOF(custom, StartResources) {
        JSG_REQUIRE(std::isfinite(custom.vcpu) && custom.vcpu > 0, RangeError,
            "Container resource vcpu must be a finite number greater than 0.");
        auto memoryMib = requireResourceAmount(custom.memoryMib, "memoryMib"_kj);
        auto diskMb = requireResourceAmount(custom.diskMb, "diskMb"_kj);
        auto resources = instanceBuilder.initCustom();
        resources.setVcpu(custom.vcpu);
        resources.setMemoryMib(memoryMib);
        resources.setDiskMb(diskMb);
      }
    }
  }

  KJ_IF_SOME(labels, options.labels) {
    requireValidLabels(labels);
    auto list = req.initLabels(labels.fields.size());
    for (auto i: kj::indices(labels.fields)) {
      auto& field = labels.fields[i];
      list[i].setName(field.name);
      list[i].setValue(field.value);
    }
  }

  KJ_IF_SOME(directorySnapshots, options.directorySnapshots) {
    auto list = req.initDirectorySnapshots(directorySnapshots.size());
    for (auto i: kj::indices(directorySnapshots)) {
      auto entry = list[i];
      auto& restore = directorySnapshots[i];

      kj::Maybe<kj::StringPtr> effectiveRestorePath;
      KJ_IF_SOME(snap, restore.snapshot) {
        effectiveRestorePath = snap.dir.asPtr();
      }
      KJ_IF_SOME(mp, restore.mountPoint) {
        effectiveRestorePath = mp.asPtr();
      }

      auto restorePath = JSG_REQUIRE_NONNULL(effectiveRestorePath, Error,
          "Directory snapshot restore requires a mountPoint when no snapshot is given.");

      JSG_REQUIRE_NONNULL(parseRestorePath(restorePath), Error,
          "Directory snapshot cannot be restored to root directory.");

      KJ_IF_SOME(snap, restore.snapshot) {
        entry.setSnapshotId(snap.id);
      }
      entry.setRestorePath(restorePath);
    }
  }

  KJ_IF_SOME(containerSnapshot, options.containerSnapshot) {
    req.getSource().setContainerSnapshotId(containerSnapshot.id);
  }

  req.setCompatibilityFlags(flags);

  IoContext::current().addTask(req.sendIgnoringResult());

  // We reset the monitor state as we are re-creating the container here.
  destroyReason = kj::none;
  currentMonitor = kj::none;
  startMonitor();
}

void Container::startMonitor() {
  KJ_ASSERT(currentMonitor == kj::none);

  auto monitor = rpcClient->monitorRequest(capnp::MessageSize{4, 0})
                     .send()
                     .then([](capnp::Response<rpc::Container::MonitorResults> results) {
    return results.getExitCode();
  }).fork();

  currentMonitor =
      IoContext::current().createObject<Monitor>(kj::mv(monitor), ++nextMonitorGeneration);
}

jsg::Promise<void> Container::setLabels(jsg::Lock& js, jsg::Dict<kj::String> labels) {
  JSG_REQUIRE(
      getRunning(), Error, "setLabels() cannot be called on a container that is not running.");

  auto& ioContext = IoContext::current();

  requireValidLabels(labels);

  auto req = rpcClient->setLabelsRequest();
  auto list = req.initLabels(labels.fields.size());
  for (auto i: kj::indices(labels.fields)) {
    auto& field = labels.fields[i];
    list[i].setName(field.name);
    list[i].setValue(field.value);
  }

  return ioContext.awaitIo(js, req.sendIgnoringResult());
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
      .image = kj::str(info.getStarted().getImage()),
    };
  });
}

jsg::Promise<Container::DirectorySnapshot> Container::snapshotDirectory(
    jsg::Lock& js, DirectorySnapshotOptions options) {
  JSG_REQUIRE(getRunning(), Error,
      "snapshotDirectory() cannot be called on a container that is not running.");
  JSG_REQUIRE(options.dir.size() > 0 && options.dir.startsWith("/"), TypeError,
      "snapshotDirectory() requires an absolute directory path (starting with '/').");

  auto req = rpcClient->snapshotDirectoryRequest();
  KJ_IF_SOME(spanContext, IoContext::current().getCurrentTraceSpan().toSpanContext()) {
    spanContext.toCapnp(req.initSpanContext());
  }
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
  JSG_REQUIRE(getRunning(), Error,
      "snapshotContainer() cannot be called on a container that is not running.");

  auto req = rpcClient->snapshotContainerRequest();
  KJ_IF_SOME(spanContext, IoContext::current().getCurrentTraceSpan().toSpanContext()) {
    spanContext.toCapnp(req.initSpanContext());
  }

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
  return ioctx.awaitIo(js, interceptOutboundHttpImpl(*rpcClient, kj::mv(addr), kj::mv(channel)));
}

kj::Promise<void> Container::interceptOutboundHttpImpl(rpc::Container::Client rpcClient,
    kj::String addr,
    kj::Own<IoChannelFactory::SubrequestChannel> channel) {
  // Get a channel token for RPC usage, the container runtime can use this
  // token later to redeem a Fetcher.
  kj::Array<byte> token = co_await channel->getToken(IoChannelFactory::ChannelTokenUsage::RPC);
  { auto drop = kj::mv(channel); }  // no longer needed

  auto req = rpcClient.setEgressHttpRequest();
  req.setHostPort(addr);
  req.setChannelToken(token);
  co_await req.sendIgnoringResult();
}

jsg::Promise<void> Container::interceptAllOutboundHttp(jsg::Lock& js, jsg::Ref<Fetcher> binding) {
  auto& ioctx = IoContext::current();
  auto channel = binding->getSubrequestChannel(ioctx);
  return ioctx.awaitIo(js, interceptAllOutboundHttpImpl(*rpcClient, kj::mv(channel)));
}

kj::Promise<void> Container::interceptAllOutboundHttpImpl(
    rpc::Container::Client rpcClient, kj::Own<IoChannelFactory::SubrequestChannel> channel) {
  auto token = co_await channel->getToken(IoChannelFactory::ChannelTokenUsage::RPC);
  { auto drop = kj::mv(channel); }  // no longer needed

  // Register for all IPv4 and IPv6 addresses (on port 80)
  auto reqV4 = rpcClient.setEgressHttpRequest();
  reqV4.setHostPort("0.0.0.0/0"_kj);
  reqV4.setChannelToken(token);

  auto reqV6 = rpcClient.setEgressHttpRequest();
  reqV6.setHostPort("::/0"_kj);
  reqV6.setChannelToken(token);

  co_await kj::joinPromisesFailFast(
      kj::arr(reqV4.sendIgnoringResult(), reqV6.sendIgnoringResult()));
}

jsg::Promise<void> Container::interceptOutboundHttps(
    jsg::Lock& js, kj::String addr, jsg::Ref<Fetcher> binding) {
  auto& ioctx = IoContext::current();
  auto channel = binding->getSubrequestChannel(ioctx);
  return ioctx.awaitIo(js, interceptOutboundHttpsImpl(*rpcClient, kj::mv(addr), kj::mv(channel)));
}

kj::Promise<void> Container::interceptOutboundHttpsImpl(rpc::Container::Client rpcClient,
    kj::String addr,
    kj::Own<IoChannelFactory::SubrequestChannel> channel) {
  auto token = co_await channel->getToken(IoChannelFactory::ChannelTokenUsage::RPC);
  { auto drop = kj::mv(channel); }  // no longer needed

  auto req = rpcClient.setEgressHttpsRequest();
  req.setHostPort(addr);
  req.setChannelToken(token);

  co_await req.sendIgnoringResult();
}

jsg::Promise<jsg::Ref<ExecProcess>> Container::exec(
    jsg::Lock& js, kj::Array<kj::String> cmd, jsg::Optional<ExecOptions> maybeOptions) {
  JSG_REQUIRE(getRunning(), Error, "exec() cannot be called on a container that is not running.");
  JSG_REQUIRE(cmd.size() > 0, TypeError, "exec() requires a non-empty command array.");

  auto options = kj::mv(maybeOptions).orDefault({});

  // Resolve the pty option, which can be a boolean shorthand or an object with dimensions.
  // Presence of an object always means the PTY is enabled; `pty: true` is the shorthand for an
  // object with default dimensions, while `pty: false` (or an absent option) leaves it disabled.
  bool ptyEnabled = false;
  kj::Maybe<uint16_t> ptyCols;
  kj::Maybe<uint16_t> ptyRows;
  KJ_IF_SOME(pty, options.pty) {
    KJ_SWITCH_ONEOF(pty) {
      KJ_CASE_ONEOF(enabled, bool) {
        ptyEnabled = enabled;
      }
      KJ_CASE_ONEOF(ptyOptions, ExecPtyOptions) {
        ptyEnabled = true;
        ptyCols = ptyOptions.cols;
        ptyRows = ptyOptions.rows;
      }
    }
  }

  // A PTY exposes a single stream that merges stdout and stderr, so when one is enabled stdin
  // defaults to "pipe" and stderr defaults to (and must be) "combined".
  if (ptyEnabled && options.$stdin == kj::none) {
    options.$stdin.emplace(kj::str("pipe"));
  }

  auto stdoutMode = getExecOutputMode(kj::mv(options.$stdout), "stdout");
  kj::String stderrMode;
  if (ptyEnabled) {
    KJ_IF_SOME(mode, options.$stderr) {
      stderrMode = kj::mv(mode);
    } else {
      stderrMode = kj::str("combined");
    }
    JSG_REQUIRE(
        stderrMode == "combined", TypeError, "stderr must be \"combined\" when PTY is enabled.");
  } else {
    stderrMode = getExecOutputMode(kj::mv(options.$stderr), "stderr");
  }
  bool combinedOutput = stderrMode == "combined";
  JSG_REQUIRE(!combinedOutput || stdoutMode == "pipe", TypeError,
      "stderr: \"combined\" requires stdout to be \"pipe\".");

  // If an already-aborted signal is provided, fail fast before spawning anything.
  KJ_IF_SOME(signal, options.signal) {
    signal->throwIfAborted(js);
  }

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
  KJ_IF_SOME(spanContext, ioContext.getCurrentTraceSpan().toSpanContext()) {
    spanContext.toCapnp(params.initSpanContext());
  }

  if (ptyEnabled) {
    auto ptyParams = params.initPty();
    KJ_IF_SOME(cols, ptyCols) {
      ptyParams.setCols(cols);
    }
    KJ_IF_SOME(rows, ptyRows) {
      ptyParams.setRows(rows);
    }
  }

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
          [&ioContext, &byteStreamFactory, options = kj::mv(options), ptyEnabled,
              stdoutInput = kj::mv(stdoutInput), stderrInput = kj::mv(stderrInput)](
              jsg::Lock& js, capnp::Response<rpc::Container::ExecResults> results) mutable
          -> jsg::Ref<ExecProcess> {
    auto process = results.getProcess();
    auto handle = process.getHandle();
    auto pid = process.getPid();

    // Init the ReadableStreams (stdout/stderr)
    jsg::Optional<JsReadableStream> stdoutStream = kj::none;
    KJ_IF_SOME(input, stdoutInput) {
      auto source = newSystemStream(kj::mv(input), StreamEncoding::IDENTITY, ioContext);
      stdoutStream = JsReadableStream::create(js, ioContext, kj::mv(source));
    }

    // stderrInput is only set if using "pipe" on stderr and not "combined"
    jsg::Optional<JsReadableStream> stderrStream = kj::none;
    KJ_IF_SOME(input, stderrInput) {
      auto source = newSystemStream(kj::mv(input), StreamEncoding::IDENTITY, ioContext);
      stderrStream = JsReadableStream::create(js, ioContext, kj::mv(source));
    }

    jsg::Optional<JsWritableStream> stdinStream = kj::none;

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
        KJ_CASE_ONEOF(readable, JsReadableStream) {
          auto sink = newSystemStream(kj::mv(stdinWriter), StreamEncoding::IDENTITY, ioContext);
          auto pipePromise =
              (ioContext.waitForDeferredProxy(readable.pumpTo(js, kj::mv(sink), EndStream::YES)));
          ioContext.addTask(pipePromise.attach(readable.addRef(js)));
        }
        // user sets "pipe"... they want to consume the API with the stdin WritableStream
        KJ_CASE_ONEOF(mode, kj::String) {
          JSG_REQUIRE(
              mode == "pipe", TypeError, "stdin must be a ReadableStream or the string \"pipe\".");
          auto sink = newSystemStream(kj::mv(stdinWriter), StreamEncoding::IDENTITY, ioContext);
          auto writable = JsWritableStream::create(js, ioContext, kj::mv(sink),
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
    return js.alloc<ExecProcess>(js, ioContext, kj::mv(stdinStream), kj::mv(stdoutStream),
        kj::mv(stderrStream), pid, kj::mv(handle), IsPty(ptyEnabled), kj::mv(options.signal));
  });
}

jsg::Promise<void> Container::interceptOutboundTcp(
    jsg::Lock& js, kj::String addr, jsg::Ref<Fetcher> binding) {
  auto& ioctx = IoContext::current();
  auto channel = binding->getSubrequestChannel(ioctx);
  return ioctx.awaitIo(js, interceptOutboundTcpImpl(*rpcClient, kj::mv(addr), kj::mv(channel)));
}

kj::Promise<void> Container::interceptOutboundTcpImpl(rpc::Container::Client rpcClient,
    kj::String addr,
    kj::Own<IoChannelFactory::SubrequestChannel> channel) {

  // Get a channel token for RPC usage, the container runtime can use this
  // token later to redeem a Fetcher whose connect() handler processes the TCP stream.
  auto token = co_await channel->getToken(IoChannelFactory::ChannelTokenUsage::RPC);
  { auto drop = kj::mv(channel); }  // no longer needed

  auto req = rpcClient.setEgressTcpRequest();
  req.setHostPort(addr);
  req.setChannelToken(token);
  co_await req.sendIgnoringResult();
}

jsg::Promise<void> Container::monitor(jsg::Lock& js) {
  // The background monitor may have already set running to false, but callers can still observe
  // that lifecycle's result through the retained promise.
  JSG_REQUIRE(currentMonitor != kj::none, Error,
      "monitor() cannot be called on a container that is not running.");

  auto generation = KJ_ASSERT_NONNULL(currentMonitor)->generation;
  return IoContext::current()
      .awaitIo(js, KJ_ASSERT_NONNULL(currentMonitor)->promise.addBranch())
      // Note: `self` (jsg::Ref) is captured to prevent GC from collecting this object while
      // the promise continuation is pending. Without it, the bare `this` pointer dangles.
      .then(js, [self = JSG_THIS, generation](jsg::Lock& js, int32_t exitCode) mutable {
    // An old monitor can settle after restart, so only mutate the current lifecycle.
    if (self->isCurrentMonitor(generation)) {
      self->invalidateTcpPortStates();
      KJ_IF_SOME(d, self->destroyReason) {
        jsg::Value error = kj::mv(d);
        self->destroyReason = kj::none;
        js.throwException(kj::mv(error));
      }
    }

    if (exitCode != 0) {
      auto err = js.error(kj::str("Container exited with unexpected exit code: ", exitCode));
      KJ_ASSERT_NONNULL(err.tryCast<jsg::JsObject>()).set(js, "exitCode", js.num(exitCode));
      js.throwException(err);
    }
  }, [self = JSG_THIS, generation](jsg::Lock& js, jsg::Value&& error) mutable {
    if (self->isCurrentMonitor(generation)) {
      self->invalidateTcpPortStates();
      self->destroyReason = kj::none;
    }
    js.throwException(kj::mv(error));
  });
}

jsg::Promise<void> Container::destroy(jsg::Lock& js, jsg::Optional<jsg::Value> error) {
  if (!getRunning()) return js.resolvedPromise();
  invalidateTcpPortStates();

  if (destroyReason == kj::none) {
    destroyReason = kj::mv(error);
  }

  auto stopped = KJ_ASSERT_NONNULL(currentMonitor)->stopped.addBranch();
  auto destroyed = rpcClient->destroyRequest(capnp::MessageSize{4, 0}).sendIgnoringResult();
  return IoContext::current().awaitIo(
      js, kj::joinPromisesFailFast(kj::arr(kj::mv(destroyed), kj::mv(stopped))));
}

void Container::signal(jsg::Lock& js, int signo) {
  JSG_REQUIRE(signo > 0 && signo <= 64, RangeError, "Invalid signal number.");
  JSG_REQUIRE(getRunning(), Error, "signal() cannot be called on a container that is not running.");

  auto req = rpcClient->signalRequest(capnp::MessageSize{4, 0});
  req.setSigno(signo);
  IoContext::current().addTask(req.sendIgnoringResult());
}

// =======================================================================================
// getTcpPort()

// A bidirectional stream over a container Port.connect() tunnel. It owns the tunnel's byte-stream
// halves and its disconnect state: a kj::Canceler that forcefully cancels in-flight reads on
// disconnect, plus a sticky exception so that reads and disconnect waiters registered after the
// disconnect still observe the failure.
//
// The `down`-drop callback must be baked into the `down` capability before the connect request is
// sent, i.e. before the write half exists. The stream is therefore constructed with only its read
// half and adopts the write half via adoptOutput() once the connect pipeline resolves.
//
// The stream is kj::Refcounted so that its disconnect callbacks can hold it safely. The `down`-drop
// callback is owned by the capnp byte-stream machinery and so can fire after the stream itself is
// gone; the write-failure and write-disconnect continuations can likewise outlive the stream. Each
// captures a kj::WeakRc and upgrades it before touching the stream, keeping them memory-safe (and
// satisfying the workerd-unsafe-continuation-capture lint). Construct via kj::rc<>() and hand the
// single reference back to callers with toOwn().
class ContainerPortAsyncIoStream final: public kj::AsyncIoStream, public kj::Refcounted {
 public:
  explicit ContainerPortAsyncIoStream(kj::Own<kj::AsyncInputStream> input): input(kj::mv(input)) {}

  // Adopts the write half of the tunnel and begins watching it for disconnection. In the
  // pipelined connect model this runs before the `Port.connect()` RPC has resolved, so an
  // output-side failure observed here must defer to the connect outcome (see
  // notifyUncleanDisconnect()) rather than tearing the stream down with a generic exception that
  // would mask the specific connect error.
  void adoptOutput(kj::Own<capnp::ExplicitEndOutputStream> up) {
    output = kj::mv(up);
    // whenWriteDisconnected()'s contract is to *resolve* (not reject) once writes would fail, and
    // in that case we route the teardown through notifyUncleanDisconnect() so a failure observed
    // during the connecting window yields to the (more specific) connect result. Defensively
    // handle a rejection as well: it is not known to be reachable through the capnp byte-stream
    // machinery today, but if it ever were, funneling it into disconnect() keeps the invariant
    // that every output-side failure tears the stream down, rather than silently stranding a
    // parked read or a rejectWhenDisconnected() waiter.
    outputWatchTask = KJ_REQUIRE_NONNULL(output)
                          ->whenWriteDisconnected()
                          .then([weak = addWeakToThis()]() {
      KJ_IF_SOME(stream, weak) {
        stream->notifyUncleanDisconnect();
      }
    }, [weak = addWeakToThis()](kj::Exception&& exception) {
      KJ_IF_SOME(stream, weak) {
        stream->disconnect(kj::mv(exception));
      }
    }).eagerlyEvaluate(nullptr);
  }

  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    KJ_IF_SOME(exception, disconnectException) {
      return exception.clone();
    }
    return canceler.wrap(KJ_REQUIRE_NONNULL(input)->tryRead(buffer, minBytes, maxBytes));
  }

  kj::Maybe<uint64_t> tryGetLength() override {
    return KJ_REQUIRE_NONNULL(input)->tryGetLength();
  }

  kj::Promise<void> write(kj::ArrayPtr<const kj::byte> buffer) override {
    KJ_IF_SOME(exception, disconnectException) {
      return exception.clone();
    }
    return propagateWriteFailure(KJ_REQUIRE_NONNULL(output)->write(buffer));
  }

  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) override {
    KJ_IF_SOME(exception, disconnectException) {
      return exception.clone();
    }
    return propagateWriteFailure(KJ_REQUIRE_NONNULL(output)->write(pieces));
  }

  kj::Promise<void> whenWriteDisconnected() override {
    // rejectWhenDisconnected() has exactly one failure mode -- the disconnect exception -- and
    // whenWriteDisconnected()'s contract is to *resolve* (not reject) once writes would fail, so
    // mapping that sole rejection to a clean resolution is correct.
    return rejectWhenDisconnected().catch_([](kj::Exception&&) {});
  }

  // Returns a promise that rejects when the connection is disconnected, or immediately if it is
  // already disconnected.
  kj::Promise<void> rejectWhenDisconnected() {
    KJ_IF_SOME(exception, disconnectException) {
      return exception.clone();
    }
    return canceler.wrap(kj::Promise<void>(kj::NEVER_DONE));
  }

  void shutdownWrite() override {
    // shutdownWrite() is a clean half-close, which we would convey to the container by calling the
    // capnp stream's explicit end(). Since shutdownWrite() is a synchronous void method, there is
    // nowhere to await end(); we detach it, keeping the stream alive via attach() until it flushes.
    //
    // No current caller actually reaches this override: the HTTP path tears the transport down by
    // dropping it (an abort -- see the destructor), and the raw-socket path half-closes via
    // endWrite() instead. We nonetheless implement the AsyncIoStream contract correctly. If end()
    // fails while the read half is still open, funnel the failure into disconnect() -- capturing a
    // WeakRc, since the detached promise can outlive the stream -- so the read side is torn down
    // deterministically rather than left hanging.
    outputWatchTask = nullptr;
    KJ_IF_SOME(stream, kj::mv(output)) {
      stream->end()
          .attach(kj::mv(stream))
          .detach([weak = addWeakToThis()](kj::Exception&& exception) {
        KJ_IF_SOME(self, weak) {
          self->disconnect(kj::mv(exception));
        }
      });
    }
  }

  kj::Promise<void> endWrite() {
    outputWatchTask = nullptr;
    auto stream = kj::mv(KJ_REQUIRE_NONNULL(output));
    output = kj::none;
    return stream->end().attach(kj::mv(stream));
  }

  void abortRead() override {
    disconnect(KJ_EXCEPTION(DISCONNECTED, "Container port connection read aborted."));
    input = kj::none;
  }

  void disconnect(kj::Exception exception = JSG_KJ_EXCEPTION(
                      DISCONNECTED, Error, "Container port connection closed unexpectedly.")) {
    // A single teardown can invoke disconnect() more than once -- e.g. a write failure both
    // rejects the pending write (propagateWriteFailure) and resolves whenWriteDisconnected()
    // (outputWatchTask), and the `down`-drop callback and abortRead() are further triggers. Make
    // it idempotent: only the first call takes effect, so every observer sees the first reported
    // exception. Recording that exception is also what lets reads, writes, and disconnect waiters
    // registered *after* this point observe the failure, since kj::Canceler retains no canceled
    // state of its own. (Writes consult the recorded exception rather than being canceler-wrapped:
    // an in-flight write should not be forcibly cancelled -- unlike a parked read, it either makes
    // progress or fails on its own -- but a write issued after disconnect must fail deterministically
    // instead of being retried against a stream we already consider broken.)
    if (disconnectException != kj::none) {
      return;
    }
    disconnectException = exception.clone();
    canceler.cancel(kj::mv(exception));
  }

  // Adopts the background task that watches the `Port.connect()` RPC. connectTyped() hands the
  // stream back before that RPC resolves (promise pipelining), so ownership of the task lives on
  // the stream; it upgrades a WeakRc before delivering connectSucceeded()/connectFailed().
  void adoptConnectTask(kj::Promise<void> task) {
    connectTask = task.eagerlyEvaluate(nullptr);
  }

  // Called when an output-side signal (down-drop callback or whenWriteDisconnected()) reports an
  // unclean disconnect. While the connect RPC is still outstanding we cannot tell an ordinary
  // teardown apart from the container refusing/failing the connection, and the latter carries a
  // more specific exception. So during the connecting window we merely remember that a disconnect
  // is pending and let the connect result decide; once settled, we tear down immediately.
  void notifyUncleanDisconnect() {
    if (connectState == ConnectState::SETTLED) {
      disconnect();
    } else {
      connectState = ConnectState::PENDING_WITH_DISCONNECT;
    }
  }

  // The connect RPC resolved successfully. If an unclean-disconnect signal arrived during the
  // connecting window, honor it now with a generic exception.
  void connectSucceeded() {
    bool disconnectPending = connectState == ConnectState::PENDING_WITH_DISCONNECT;
    connectState = ConnectState::SETTLED;
    if (disconnectPending) {
      disconnect();
    }
  }

  // The connect RPC failed. Tear the stream down with the specific connect exception, which (by
  // disconnect()'s first-wins rule) takes precedence over any buffered generic disconnect.
  void connectFailed(kj::Exception exception) {
    connectState = ConnectState::SETTLED;
    disconnect(kj::mv(exception));
  }

 private:
  kj::Maybe<kj::Own<kj::AsyncInputStream>> input;
  // The write half of the tunnel. `output` is an ExplicitEndOutputStream, whose clean-EOF signal
  // is end(); dropping it without end() is an abort. The clean paths (shutdownWrite() for an HTTP
  // half-close, endWrite() for a completed raw-socket up pump) move `output` out before they run,
  // so if it is still present when this stream is destroyed the teardown is abnormal and the
  // destructor's default drop correctly conveys an abort -- not a clean end -- to the container.
  // (This matches the behavior before container tunnels were pooled, where the up stream was
  // likewise dropped without end() on any abnormal teardown.)
  kj::Maybe<kj::Own<capnp::ExplicitEndOutputStream>> output;
  kj::Canceler canceler;
  kj::Maybe<kj::Exception> disconnectException;
  kj::Promise<void> outputWatchTask = nullptr;

  // Tracks the pipelined `Port.connect()` RPC while it is outstanding. See
  // notifyUncleanDisconnect() for why an unclean disconnect observed during the connecting window
  // must be buffered rather than acted on immediately.
  enum class ConnectState {
    // The RPC is outstanding and no unclean-disconnect signal has fired.
    PENDING,
    // The RPC is outstanding and an unclean-disconnect signal has fired; it will be honored once
    // the connect resolves, unless the connect itself fails first (whose specific exception wins).
    PENDING_WITH_DISCONNECT,
    // The RPC has settled (resolved or rejected); disconnect signals now take effect immediately.
    SETTLED,
  };
  ConnectState connectState = ConnectState::PENDING;

  // Background watcher for the connect RPC; see adoptConnectTask().
  kj::Promise<void> connectTask = nullptr;

  kj::Promise<void> propagateWriteFailure(kj::Promise<void> promise) {
    // The returned promise is handed to the caller and can, in principle, outlive the stream, so
    // capture a weak reference and upgrade it before recording the disconnect.
    return promise.catch_([weak = addWeakToThis()](kj::Exception&& exception) -> kj::Promise<void> {
      KJ_IF_SOME(stream, weak) {
        stream->disconnect(exception.clone());
      }
      return kj::mv(exception);
    });
  }
};

// A kj::NetworkAddress backed by a single container `Port` capability. Each connect() opens a
// fresh tunnel to that port via `Port.connect()`.
//
// This adapter exists mainly because `kj::newHttpClient()` produces a client that dials a
// `kj::NetworkAddress&`; the pooled HTTP path holds one of these and reuses its connections. The
// raw-socket path instead calls connectTyped() directly to obtain the concrete stream type.
// listen() and clone() are unsupported: this is a dial-only address, not a bindable endpoint.
//
// `failed` is a sticky flag recording that a connection attempt to this port failed. Callers
// consult it to decide whether a cached port is still usable or must be discarded and rebuilt.
class ContainerPortNetworkAddress final: public kj::NetworkAddress {
 public:
  ContainerPortNetworkAddress(
      capnp::ByteStreamFactory& byteStreamFactory, rpc::Container::Port::Client port)
      : byteStreamFactory(byteStreamFactory),
        port(kj::mv(port)) {}

  // Establishes a new connection to the container port, returning the concrete stream type so
  // callers that need `ContainerPortAsyncIoStream`-specific behavior (e.g. `endWrite()`,
  // `rejectWhenDisconnected()`) can avoid a downcast. The `connect()` override upcasts for the
  // `kj::NetworkAddress` interface (used by the pooled `NetworkAddressHttpClient`).
  //
  // We pipeline the `Port.connect()` RPC: the stream is handed back immediately, before the RPC
  // resolves, so the caller can begin writing request bytes that ride the same round-trip as the
  // connect itself. The connect result is watched in the background (see adoptConnectTask()); a
  // failure surfaces as a disconnect carrying the specific connect exception, and the stream's
  // connecting-window bookkeeping (notifyUncleanDisconnect()) ensures that specific error wins
  // over any generic unclean-disconnect signal that races it.
  //
  // We deliberately do not impose our own timeout on the connect. The Workers runtime relies on a
  // higher-level bound -- the enclosing request's lifetime -- to cap how long any subrequest can
  // wait, and historically edgeworker has not layered its own timeouts underneath that. Timeout
  // policy is the application's to choose: the Worker (or the code driving the container) is best
  // placed to decide an appropriate deadline for its own traffic, so we surface the connection as
  // it completes or fails rather than second-guessing it here.
  kj::Own<ContainerPortAsyncIoStream> connectTyped() {
    auto req = port.connectRequest(capnp::MessageSize{4, 1});
    auto downPipe = kj::newOneWayPipe();

    // Construct the connection object up-front (with only its read half) so that the down-drop
    // callback -- which must be baked into the `down` capability before we send -- can reference
    // it. The write half is adopted below, before we return.
    auto conn = kj::rc<ContainerPortAsyncIoStream>(kj::mv(downPipe.in));

    // Capture a weak reference: this callback is owned by the capnp byte-stream machinery once
    // `down` is sent to the container, so it may fire after `conn` has been destroyed. Upgrading
    // the weak reference makes that safe. Route through notifyUncleanDisconnect() so a drop seen
    // while the connect is still outstanding defers to the (more specific) connect result.
    auto down =
        capnp::ExplicitEndOutputStream::wrap(kj::mv(downPipe.out), [weak = conn.addWeakRef()]() {
      KJ_IF_SOME(stream, weak) {
        stream->notifyUncleanDisconnect();
      }
    });
    req.setDown(byteStreamFactory.kjToCapnp(kj::mv(down)));
    auto pipeline = req.send();
    { auto drop = kj::mv(req); }

    auto up = byteStreamFactory.capnpToKjExplicitEnd(pipeline.getUp());
    conn->adoptOutput(kj::mv(up));

    // Watch the connect result in the background. On failure, mark the port failed (so a future
    // getTcpPort() lookup discards this cached address and rebuilds) and tear the stream down with
    // the specific exception. `failed` is shared into the callback because the connect may resolve
    // after this address (and the stream) are gone.
    conn->adoptConnectTask(pipeline.ignoreResult().then([weak = conn.addWeakRef()]() {
      KJ_IF_SOME(stream, weak) {
        stream->connectSucceeded();
      }
    }, [weak = conn.addWeakRef(), failed = failed.addRef()](kj::Exception&& exception) mutable {
      *failed = true;
      KJ_IF_SOME(stream, weak) {
        stream->connectFailed(kj::mv(exception));
      }
    }));

    return conn.toOwn();
  }

  kj::Promise<kj::Own<kj::AsyncIoStream>> connect() override {
    kj::Own<kj::AsyncIoStream> result = connectTyped();
    return kj::mv(result);
  }

  kj::Own<kj::ConnectionReceiver> listen() override {
    KJ_UNIMPLEMENTED("cannot listen on a container port address");
  }

  kj::Own<kj::NetworkAddress> clone() override {
    KJ_UNIMPLEMENTED("cannot clone a container port address");
  }

  kj::String toString() override {
    return kj::str("container-port");
  }

  void markFailed() {
    *failed = true;
  }

  bool hasFailed() const {
    return *failed;
  }

 private:
  capnp::ByteStreamFactory& byteStreamFactory;
  rpc::Container::Port::Client port;
  // Sticky flag recording that a connect attempt to this port failed. It is a shared Rc because
  // the pipelined connect task (see connectTyped()) may set it after this address is gone.
  kj::Rc<bool> failed = kj::rc<bool>(false);
};

class Container::TcpPortState {
 public:
  TcpPortState(capnp::ByteStreamFactory& byteStreamFactory, rpc::Container::Port::Client port)
      : address(byteStreamFactory, kj::mv(port)) {}

  TcpPortState(kj::Timer& timer,
      capnp::ByteStreamFactory& byteStreamFactory,
      kj::EntropySource& entropySource,
      const kj::HttpHeaderTable& headerTable,
      rpc::Container::Port::Client port)
      : TcpPortState(byteStreamFactory, kj::mv(port)) {
    pooled.emplace(PooledPort{
      .client = kj::newHttpClient(timer, headerTable, address, {.entropySource = entropySource}),
    });
  }

  // Establishes a new raw-socket connection, returning the concrete stream type so raw-socket
  // pumping can call `endWrite()`/`rejectWhenDisconnected()` without a downcast. The connect is
  // pipelined, so this returns synchronously.
  kj::Own<ContainerPortAsyncIoStream> connectForRawSocket() {
    return address.connectTyped();
  }

  // Used by the HTTP fallback path (non-pooled port, or one whose pooled client was
  // invalidated), via `kj::newPromisedStream`.
  kj::Promise<kj::Own<kj::AsyncIoStream>> connect() {
    return address.connect();
  }

  void invalidate() {
    KJ_IF_SOME(p, pooled) {
      p.invalidated = true;
    }
  }

  kj::Maybe<kj::HttpClient&> getHttpClient() {
    KJ_IF_SOME(p, pooled) {
      if (!p.invalidated) return *p.client;
    }
    return kj::none;
  }

  // `markPortFailed()`/`hasPortFailed()` record a sticky failure so the next getTcpPort() lookup
  // discards this cached state and rebuilds a fresh tunnel. This is intentionally not a full
  // circuit breaker (no failure-count threshold, fail-fast window, or state-transition metrics):
  // the container sidecar is reached over a local socket via a single Port capability, and the
  // enclosing request lifetime already bounds how long a request can wait on an unhealthy sidecar.
  // A retry storm against a dead sidecar would be capped by that request-level teardown rather
  // than by an explicit breaker here.
  void markPortFailed() {
    address.markFailed();
  }

  bool hasPortFailed() const {
    return address.hasFailed();
  }

 private:
  // Present only for pooled ports (the CONTAINER_TUNNEL_REUSE autogate): a persistent HTTP client
  // that reuses connections across requests. `invalidated` is set when the container lifecycle
  // changes such that pooled reuse must stop; thereafter a fresh connection is formed per request.
  struct PooledPort {
    kj::Own<kj::HttpClient> client;
    bool invalidated = false;
  };

  ContainerPortNetworkAddress address;
  kj::Maybe<PooledPort> pooled;
};

void Container::invalidateTcpPortStates() {
  KJ_IF_SOME(states, tcpPortStates) {
    for (auto& entry: *states) {
      entry.value->invalidate();
    }
  }
  tcpPortStates = kj::none;
}

// `getTcpPort()` returns a `Fetcher`, on which `fetch()` and `connect()` can be called. `Fetcher`
// is a JavaScript wrapper around `WorkerInterface`, so we need to implement that.
class Container::TcpPortWorkerInterface final: public WorkerInterface {
 public:
  TcpPortWorkerInterface(kj::EntropySource& entropySource,
      const kj::HttpHeaderTable& headerTable,
      kj::Rc<TcpPortState> portState)
      : entropySource(entropySource),
        headerTable(headerTable),
        portState(kj::mv(portState)) {}

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

    KJ_IF_SOME(client, portState->getHttpClient()) {
      auto service = kj::newHttpService(client);
      co_await service->request(method, noHostUrl, newHeaders, requestBody, response);
      co_return;
    }

    auto connection = kj::newPromisedStream(portState->connect());
    auto client = kj::newHttpClient(headerTable, *connection, {.entropySource = entropySource});
    auto service = kj::newHttpService(*client);
    co_await service->request(method, noHostUrl, newHeaders, requestBody, response);
  }

  // Implements connect(), i.e., forms a raw socket.
  kj::Promise<void> connect(kj::StringPtr host,
      const kj::HttpHeaders& headers,
      kj::AsyncIoStream& connection,
      ConnectResponse& response,
      kj::HttpConnectSettings settings) override {
    JSG_REQUIRE(!settings.useTls, Error,
        "Connecting to a container using TLS is not currently supported. It is unnecessary "
        "anyway, as the connection is already secure by default.");

    auto promise = connectImpl(kj::Own<kj::AsyncIoStream>(&connection, kj::NullDisposer::instance));

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
  kj::EntropySource& entropySource;
  const kj::HttpHeaderTable& headerTable;
  kj::Rc<TcpPortState> portState;

  // Connect to the port and pump bytes to/from `connection`.
  kj::Promise<void> connectImpl(kj::Own<kj::AsyncIoStream> connection) {
    auto portConnection = portState->connectForRawSocket();
    auto& portStream = *portConnection;

    auto downPumpTask =
        portConnection->pumpTo(*connection).then([&connection](uint64_t) -> kj::Promise<void> {
      connection->shutdownWrite();
      return kj::NEVER_DONE;
    });
    auto upPumpTask = connection->pumpTo(*portConnection)
                          .then([&portStream](uint64_t) {
      return portStream.endWrite();
    }).then([]() -> kj::Promise<void> { return kj::NEVER_DONE; });
    auto disconnected = portStream.rejectWhenDisconnected();

    co_await kj::joinPromisesFailFast(
        kj::arr(kj::mv(upPumpTask), kj::mv(downPumpTask), kj::mv(disconnected)));
  }
};

// `Fetcher` actually wants us to give it a factory that creates a new `WorkerInterface` for each
// request, so this is that.
class Container::TcpPortOutgoingFactory final: public Fetcher::OutgoingFactory {
 public:
  TcpPortOutgoingFactory(kj::EntropySource& entropySource,
      const kj::HttpHeaderTable& headerTable,
      kj::Rc<TcpPortState> portState)
      : entropySource(entropySource),
        headerTable(headerTable),
        portState(kj::mv(portState)) {}

  Result newSingleUseClient(
      kj::Maybe<kj::String> cfStr, MakeUserSpanParent makeUserSpanParent) override {
    // At present we have no use for `cfStr`. This factory creates no operation span.
    auto client = IoContext::current().getSubrequestNoChecks(
        [&](auto& tracing, auto& channelFactory) -> kj::Own<WorkerInterface> {
      makeUserSpanParent(tracing);
      return kj::heap<TcpPortWorkerInterface>(entropySource, headerTable, portState.addRef());
    }, {.inHouse = false, .wrapMetrics = false});
    return {.client = kj::mv(client), .spanParents = kj::none};
  }

 private:
  kj::EntropySource& entropySource;
  const kj::HttpHeaderTable& headerTable;

  kj::Rc<TcpPortState> portState;
};

jsg::Ref<Fetcher> Container::getTcpPort(jsg::Lock& js, int port) {
  JSG_REQUIRE(port > 0 && port < 65536, TypeError, "Invalid port number: ", port);

  auto& ioctx = IoContext::current();
  auto makePortRequest = [&]() {
    auto req = rpcClient->getTcpPortRequest(
        capnp::MessageSize{4 + capnp::sizeInWords<rpc::SpanContext>(), 0});
    req.setPort(port);
    KJ_IF_SOME(spanContext, ioctx.getCurrentTraceSpan().toSpanContext()) {
      spanContext.toCapnp(req.initSpanContext());
    }
    return req;
  };

  auto portState = [&]() -> kj::Rc<TcpPortState> {
    if (util::Autogate::isEnabled(util::AutogateKey::CONTAINER_TUNNEL_REUSE)) {
      if (tcpPortStates == kj::none) {
        tcpPortStates = ioctx.createObject<kj::HashMap<int, kj::Rc<TcpPortState>>>();
      }
      auto& states = *KJ_ASSERT_NONNULL(tcpPortStates);

      KJ_IF_SOME(entry, states.findEntry(port)) {
        if (!entry.value->hasPortFailed()) return entry.value.addRef();
        states.erase(entry);
      }
      if (states.size() < MAX_CACHED_TCP_PORTS) {
        auto req = makePortRequest();
        auto response = req.send();
        auto state = kj::rc<TcpPortState>(ioctx.getUnsafeTimer(), ioctx.getByteStreamFactory(),
            ioctx.getEntropySource(), ioctx.getHeaderTable(), response.getPort());
        ioctx.addTask(response.ignoreResult().catch_(
            [state = state.addRef()](kj::Exception&&) mutable { state->markPortFailed(); }));
        states.insert(port, state.addRef());
        return state;
      }
    }

    auto req = makePortRequest();
    return kj::rc<TcpPortState>(ioctx.getByteStreamFactory(), req.send().getPort());
  }();

  kj::Own<Fetcher::OutgoingFactory> factory = kj::heap<TcpPortOutgoingFactory>(
      ioctx.getEntropySource(), ioctx.getHeaderTable(), kj::mv(portState));

  return js.alloc<Fetcher>(
      ioctx.addObject(kj::mv(factory)), Fetcher::RequiresHostAndProtocol::YES, true);
}

}  // namespace workerd::api
