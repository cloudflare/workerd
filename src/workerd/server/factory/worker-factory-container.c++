// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Durable Object containers: the Cap'n Proto servers of `rpc::Container` and the capabilities it
// hands out, over the Rust client (server/container/), and the C++ channels a container's egress
// forwards to. The servers only unpack params, convert `ByteStream`s to and from KJ streams, and
// write results.

#include "worker-factory-impl.h"

#include <workerd/util/sentry.h>

#include <kj/compat/http.h>

namespace workerd::server {

namespace {

::rust::String toRust(capnp::Text::Reader text) {
  return ::rust::String(text.begin(), text.size());
}

::rust::Str toRustStr(capnp::Text::Reader text) {
  return ::rust::Str(text.begin(), text.size());
}

::rust::Vec<::rust::String> toRust(capnp::List<capnp::Text>::Reader list) {
  ::rust::Vec<::rust::String> result;
  result.reserve(list.size());
  for (const auto& text: list) result.push_back(toRust(text));
  return result;
}

::rust::Vec<ContainerLabel> toRust(capnp::List<rpc::Container::Label>::Reader list) {
  ::rust::Vec<ContainerLabel> result;
  result.reserve(list.size());
  for (const auto& label: list) {
    result.push_back(ContainerLabel{toRust(label.getName()), toRust(label.getValue())});
  }
  return result;
}

// A NUL-terminated copy of `text`, which a `capnp::Text::Reader` needs.
kj::String toText(const ::rust::String& text) {
  return kj::str(kj::ArrayPtr<const char>(text.data(), text.size()));
}

// A process's stdin, from the Rust stream whose `shutdownWrite()` ends it.
class ExecStdinStream final: public capnp::ExplicitEndOutputStream {
 public:
  explicit ExecStdinStream(kj::Own<kj::AsyncIoStream> inner): inner(kj::mv(inner)) {}

  kj::Promise<void> write(kj::ArrayPtr<const byte> buffer) override {
    return inner->write(buffer);
  }
  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const byte>> pieces) override {
    return inner->write(pieces);
  }
  kj::Promise<void> whenWriteDisconnected() override {
    return inner->whenWriteDisconnected();
  }
  kj::Promise<void> end() override {
    inner->shutdownWrite();
    return kj::READY_NOW;
  }

 private:
  kj::Own<kj::AsyncIoStream> inner;
};

class ProcessServer final: public rpc::Container::ProcessHandle::Server {
 public:
  ProcessServer(capnp::ByteStreamFactory& byteStreamFactory, ::rust::Box<ContainerProcess> process)
      : byteStreamFactory(byteStreamFactory),
        process(kj::mv(process)) {}

  kj::Promise<void> wait(WaitContext context) override {
    auto self = thisCap();
    auto exitCode = co_await process->wait();
    context.getResults().setExitCode(exitCode);
  }

  kj::Promise<void> stdinWriter(StdinWriterContext context) override {
    context.getResults().setWriter(
        byteStreamFactory.kjToCapnp(kj::heap<ExecStdinStream>(process->stdin_writer())));
    return kj::READY_NOW;
  }

  kj::Promise<void> kill(KillContext context) override {
    auto self = thisCap();
    co_await process->kill(context.getParams().getSigno());
  }

  kj::Promise<void> resize(ResizeContext context) override {
    auto self = thisCap();
    auto params = context.getParams();
    co_await process->resize(params.getCols(), params.getRows());
  }

 private:
  capnp::ByteStreamFactory& byteStreamFactory;
  ::rust::Box<ContainerProcess> process;
};

class PortServer final: public rpc::Container::Port::Server, private kj::TaskSet::ErrorHandler {
 public:
  PortServer(capnp::ByteStreamFactory& byteStreamFactory, ::rust::Box<ContainerPort> port)
      : byteStreamFactory(byteStreamFactory),
        port(kj::mv(port)),
        pumpTasks(*this) {}

  kj::Promise<void> connect(ConnectContext context) override {
    auto self = thisCap();
    auto connection = co_await port->connect();

    auto upPipe = kj::newOneWayPipe();
    auto upEnd = kj::mv(upPipe.in);
    context.getResults().setUp(byteStreamFactory.kjToCapnp(kj::mv(upPipe.out)));
    auto downEnd = byteStreamFactory.capnpToKjExplicitEnd(context.getParams().getDown());

    // Holds both pump endpoints so the continuations below can refer to them weakly.
    struct PumpEnds final: public kj::PtrTarget {
      PumpEnds(
          kj::Own<kj::AsyncIoStream> connection, kj::Own<capnp::ExplicitEndOutputStream> downEnd)
          : connection(kj::mv(connection)),
            downEnd(kj::mv(downEnd)) {}
      kj::Weak<PumpEnds> addWeak() {
        return addWeakToThis();
      }
      kj::Own<kj::AsyncIoStream> connection;
      kj::Own<capnp::ExplicitEndOutputStream> downEnd;
    };
    auto ends = kj::heap<PumpEnds>(kj::mv(connection), kj::mv(downEnd));
    auto upPump = upEnd->pumpTo(*ends->connection).then([weakEnds = ends->addWeak()](uint64_t) {
      KJ_REQUIRE_NONNULL(weakEnds.tryGet(), "container connection destroyed before pump finished")
          .connection->shutdownWrite();
    });
    auto downPump =
        ends->connection->pumpTo(*ends->downEnd).then([weakEnds = ends->addWeak()](uint64_t) {
      return KJ_REQUIRE_NONNULL(
          weakEnds.tryGet(), "container stream destroyed before pump finished")
          .downEnd->end();
    });

    pumpTasks.add(kj::joinPromisesFailFast(kj::arr(kj::mv(upPump), kj::mv(downPump)))
                      .attach(kj::mv(upEnd), kj::mv(ends)));
  }

 private:
  capnp::ByteStreamFactory& byteStreamFactory;
  ::rust::Box<ContainerPort> port;
  kj::TaskSet pumpTasks;

  void taskFailed(kj::Exception&& exception) override {
    // A tunnel's peer learns of a disconnect through the ByteStreams; only unexpected failures
    // are logged.
    if (isInterestingException(exception)) {
      KJ_LOG(ERROR, "container tunnel pump task failed", exception);
    }
  }
};

class ContainerServer final: public rpc::Container::Server {
 public:
  ContainerServer(capnp::ByteStreamFactory& byteStreamFactory, ::rust::Box<ContainerHandle> client)
      : byteStreamFactory(byteStreamFactory),
        client(kj::mv(client)) {}

  kj::Promise<void> status(StatusContext context) override {
    auto self = thisCap();
    auto running = co_await client->status();
    context.getResults().setRunning(running);
  }

  kj::Promise<void> start(StartContext context) override {
    auto self = thisCap();
    auto params = context.getParams();
    ContainerStart start{};
    start.has_entrypoint = params.hasEntrypoint();
    if (start.has_entrypoint) start.entrypoint = toRust(params.getEntrypoint());
    start.has_environment = params.hasEnvironmentVariables();
    if (start.has_environment) start.environment = toRust(params.getEnvironmentVariables());
    start.enable_internet = params.getEnableInternet();
    if (params.hasLabels()) start.labels = toRust(params.getLabels());
    for (const auto& snapshot: params.getDirectorySnapshots()) {
      start.directory_snapshots.push_back(ContainerSnapshotRestore{
        toRust(snapshot.getSnapshotId()), toRust(snapshot.getRestorePath())});
    }
    auto source = params.getSource();
    switch (source.which()) {
      case rpc::Container::StartParams::Source::IMAGE:
        start.source = ContainerSource::Image;
        start.source_text = toRust(source.getImage());
        break;
      case rpc::Container::StartParams::Source::CONTAINER_SNAPSHOT_ID:
        if (source.hasContainerSnapshotId()) {
          start.source = ContainerSource::Snapshot;
          start.source_text = toRust(source.getContainerSnapshotId());
        }
        break;
    }
    start.pid_namespace = params.getCompatibilityFlags().getContainersPidNamespace();
    co_await client->start(start);
  }

  kj::Promise<void> monitor(MonitorContext context) override {
    auto self = thisCap();
    auto exitCode = co_await client->monitor();
    context.getResults().setExitCode(exitCode);
  }

  kj::Promise<void> destroy(DestroyContext context) override {
    auto self = thisCap();
    co_await client->destroy();
  }

  kj::Promise<void> signal(SignalContext context) override {
    auto self = thisCap();
    co_await client->signal(context.getParams().getSigno());
  }

  kj::Promise<void> exec(ExecContext context) override {
    auto self = thisCap();
    auto request = context.getParams();
    auto options = request.getParams();
    ContainerExec exec{};
    exec.cmd = toRust(request.getCmd());
    exec.has_env = options.hasEnv();
    if (exec.has_env) exec.env = toRust(options.getEnv());
    exec.has_working_directory = options.hasWorkingDirectory();
    if (exec.has_working_directory) {
      exec.working_directory = toRust(options.getWorkingDirectory());
    }
    exec.has_user = options.hasUser();
    if (exec.has_user) exec.user = toRust(options.getUser());
    exec.combined_output = options.getCombinedOutput();
    exec.pty = options.hasPty();
    if (exec.pty) {
      exec.pty_cols = options.getPty().getCols();
      exec.pty_rows = options.getPty().getRows();
    }
    kj::Maybe<kj::Own<ExecOutput>> stdoutWriter;
    if (request.hasStdoutWriter()) {
      stdoutWriter = byteStreamFactory.capnpToKjExplicitEnd(request.getStdoutWriter());
    }
    kj::Maybe<kj::Own<ExecOutput>> stderrWriter;
    if (request.hasStderrWriter()) {
      stderrWriter = byteStreamFactory.capnpToKjExplicitEnd(request.getStderrWriter());
    }
    auto process = co_await client->exec(exec, kj::mv(stdoutWriter), kj::mv(stderrWriter));
    auto result = context.getResults().initProcess();
    result.setPid(process->pid());
    result.setHandle(kj::heap<ProcessServer>(byteStreamFactory, kj::mv(process)));
  }

  kj::Promise<void> getTcpPort(GetTcpPortContext context) override {
    auto self = thisCap();
    auto port = co_await client->tcp_port(context.getParams().getPort());
    context.getResults().setPort(kj::heap<PortServer>(byteStreamFactory, kj::mv(port)));
  }

  kj::Promise<void> listenTcp(ListenTcpContext context) override {
    KJ_UNIMPLEMENTED("listenTcp not implemented for Docker containers - use port mapping instead");
  }

  kj::Promise<void> setInactivityTimeout(SetInactivityTimeoutContext context) override {
    auto self = thisCap();
    co_await client->set_inactivity_timeout(context.getParams().getDurationMs());
  }

  kj::Promise<void> setEgressHttp(SetEgressHttpContext context) override {
    auto params = context.getParams();
    return setEgress(EgressProtocol::Http, params.getHostPort(), params.getChannelToken());
  }

  kj::Promise<void> setEgressHttps(SetEgressHttpsContext context) override {
    auto params = context.getParams();
    return setEgress(EgressProtocol::Https, params.getHostPort(), params.getChannelToken());
  }

  kj::Promise<void> setEgressTcp(SetEgressTcpContext context) override {
    auto params = context.getParams();
    return setEgress(EgressProtocol::Tcp, params.getHostPort(), params.getChannelToken());
  }

  kj::Promise<void> snapshotDirectory(SnapshotDirectoryContext context) override {
    auto self = thisCap();
    auto params = context.getParams();
    auto snapshot = co_await client->snapshot_directory(toRustStr(params.getDir()));
    auto result = context.getResults().initSnapshot();
    result.setId(toText(snapshot.id));
    result.setSize(snapshot.size);
    result.setDir(toText(snapshot.dir));
    if (params.hasName() && params.getName().size() > 0) result.setName(params.getName());
  }

  kj::Promise<void> snapshotContainer(SnapshotContainerContext context) override {
    auto self = thisCap();
    auto params = context.getParams();
    auto snapshot = co_await client->snapshot_container();
    auto result = context.getResults().initSnapshot();
    result.setId(toText(snapshot.id));
    result.setSize(snapshot.size);
    if (params.hasName() && params.getName().size() > 0) result.setName(params.getName());
  }

  kj::Promise<void> inspect(InspectContext context) override {
    auto self = thisCap();
    auto inspected = co_await client->inspect();
    auto info = context.getResults().initInfo();
    if (!inspected.started) {
      info.setNone();
      co_return;
    }
    auto started = info.initStarted();
    auto labels = started.initLabels(inspected.labels.size());
    for (auto i: kj::indices(inspected.labels)) {
      labels[i].setName(toText(inspected.labels[i].name));
      labels[i].setValue(toText(inspected.labels[i].value));
    }
    started.setImage(toText(inspected.image));
  }

  kj::Promise<void> setLabels(SetLabelsContext context) override {
    auto self = thisCap();
    auto labels = toRust(context.getParams().getLabels());
    co_await client->set_labels(::rust::Slice<const ContainerLabel>(labels.data(), labels.size()));
  }

 private:
  capnp::ByteStreamFactory& byteStreamFactory;
  ::rust::Box<ContainerHandle> client;

  kj::Promise<void> setEgress(
      EgressProtocol protocol, capnp::Text::Reader hostPort, capnp::Data::Reader token) {
    auto self = thisCap();
    co_await client->set_egress(
        protocol, toRustStr(hostPort), ::rust::Slice<const uint8_t>(token.begin(), token.size()));
  }
};

// The answer of a Worker's `connect()` to a container's TCP egress, whose CONNECT from the
// sidecar is already accepted: an acceptance has nothing more to say.
class TcpEgressConnectResponse final: public kj::HttpService::ConnectResponse {
 public:
  void accept(uint statusCode, kj::StringPtr statusText, const kj::HttpHeaders& headers) override {}

  kj::Own<kj::AsyncOutputStream> reject(uint statusCode,
      kj::StringPtr statusText,
      const kj::HttpHeaders& headers,
      kj::Maybe<uint64_t> expectedBodySize = kj::none) override {
    KJ_FAIL_REQUIRE("TCP egress worker rejected the connection: ", statusCode, " ", statusText);
  }
};

}  // namespace

kj::Own<ContainerCapability> new_container_capability(
    const WorkerFactory& factory, ::rust::Box<ContainerHandle> client) {
  return kj::heap<ContainerCapability>(rpc::Container::Client(
      kj::heap<ContainerServer>(factory.getImpl().byteStreamFactory, kj::mv(client))));
}

kj::Promise<void> exec_output_write(ExecOutput& output, ::rust::Slice<const uint8_t> data) {
  return output.write(kj::ArrayPtr<const byte>(data.data(), data.size()));
}

kj::Promise<void> exec_output_end(kj::Own<ExecOutput> output) {
  co_await output->end();
}

void exec_output_abort(ExecOutput& output, kj::Exception reason) {
  output.abortWrite(kj::mv(reason));
}

kj::Own<EgressChannel> factory_decode_egress_token(
    const WorkerFactory& factory, ::rust::Slice<const uint8_t> token) {
  auto channel = factory.getImpl().channelTokenHandler.decodeSubrequestChannelToken(
      IoChannelFactory::ChannelTokenUsage::RPC,
      kj::ArrayPtr<const byte>(token.data(), token.size()));
  return kj::heap<EgressChannel>(kj::mv(channel));
}

kj::Own<WorkerInterface> egress_channel_start_request(const EgressChannel& channel) {
  return channel.channel->startRequest({});
}

kj::Promise<void> egress_channel_connect(const WorkerFactory& factory,
    const EgressChannel& channel,
    ::rust::Str host,
    kj::Own<kj::AsyncIoStream> tunnel) {
  auto worker = channel.channel->startRequest({});
  auto destAddr = kj::str(host);
  TcpEgressConnectResponse response;
  kj::HttpHeaders headers(*factory.getImpl().headerTable);
  co_await worker->connect(destAddr, headers, *tunnel, response, {});
}

}  // namespace workerd::server
