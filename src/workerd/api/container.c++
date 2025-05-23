// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "container.h"

#include <workerd/api/http.h>
#include <workerd/io/io-context.h>
#include <workerd/io/docker-client.h>

namespace workerd::api {

// =======================================================================================
// Basic lifecycle methods

Container::Container(rpc::Container::Client rpcClient, bool running)
    : rpcClient(IoContext::current().addObject(kj::heap(kj::mv(rpcClient)))),
      running(running), isDockerMode(false) {}

Container::Container(kj::String containerId, kj::String imageTag, io::DockerClient& dockerClient)
    : containerId(kj::mv(containerId)), imageTag(kj::mv(imageTag)), dockerClient(dockerClient),
      running(false), isDockerMode(true) {}

void Container::start(jsg::Lock& js, jsg::Optional<StartupOptions> maybeOptions) {
  JSG_REQUIRE(!running, Error, "start() cannot be called on a container that is already running.");

  StartupOptions options = kj::mv(maybeOptions).orDefault({});

  if (isDockerMode) {
    // Docker mode implementation
    KJ_IF_SOME(client, dockerClient) {
      // Convert entrypoint to StringPtr array
      kj::Array<kj::StringPtr> entrypointPtrs;
      KJ_IF_SOME(entrypoint, options.entrypoint) {
        auto builder = kj::heapArrayBuilder<kj::StringPtr>(entrypoint.size());
        for (auto& cmd : entrypoint) {
          builder.add(cmd.asPtr());
        }
        entrypointPtrs = builder.finish();
      } else {
        entrypointPtrs = kj::heapArray<kj::StringPtr>(0);
      }
      
      // Convert environment variables
      kj::Array<kj::StringPtr> envPtrs;
      KJ_IF_SOME(env, options.env) {
        auto builder = kj::heapArrayBuilder<kj::StringPtr>(env.fields.size());
        for (auto& field : env.fields) {
          JSG_REQUIRE(field.name.findFirst('=') == kj::none, Error,
              "Environment variable names cannot contain '=': ", field.name);
          JSG_REQUIRE(field.name.findFirst('\0') == kj::none, Error,
              "Environment variable names cannot contain '\\0': ", field.name);
          JSG_REQUIRE(field.value.findFirst('\0') == kj::none, Error,
              "Environment variable values cannot contain '\\0': ", field.name);
          
          builder.add(kj::str(field.name, "=", field.value).asPtr());
        }
        envPtrs = builder.finish();
      } else {
        envPtrs = kj::heapArray<kj::StringPtr>(0);
      }
      
      KJ_IF_SOME(cId, containerId) {
        KJ_IF_SOME(tag, imageTag) {
          // Start container via Docker
          IoContext::current().addTask(
            client.startContainer(tag, cId, entrypointPtrs, envPtrs, portMappings)
              .catch_([](kj::Exception&& e) {
                // Log error but don't propagate to avoid crashing
                KJ_LOG(ERROR, "Failed to start container", e);
              }));
        }
      }
    }
  } else {
    // RPC mode implementation (existing code)
    KJ_IF_SOME(client, rpcClient) {
      auto req = client->startRequest();
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
          JSG_REQUIRE(field->name.findFirst('=') == kj::none, Error,
              "Environment variable names cannot contain '=': ", field->name);

          JSG_REQUIRE(field->name.findFirst('\0') == kj::none, Error,
              "Environment variable names cannot contain '\\0': ", field->name);

          JSG_REQUIRE(field->value.findFirst('\0') == kj::none, Error,
              "Environment variable values cannot contain '\\0': ", field->name);

          list.set(i, str(field->name, "=", field->value));
        }
      }

      IoContext::current().addTask(req.sendIgnoringResult());
    }
  }

  running = true;
}

jsg::Promise<void> Container::monitor(jsg::Lock& js) {
  JSG_REQUIRE(running, Error, "monitor() cannot be called on a container that is not running.");

  if (isDockerMode) {
    // Docker mode implementation
    KJ_IF_SOME(client, dockerClient) {
      KJ_IF_SOME(cId, containerId) {
        return IoContext::current()
            .awaitIo(js, client.waitForContainerExit(cId))
            .then(js, [this](jsg::Lock& js) {
          running = false;
          KJ_IF_SOME(d, destroyReason) {
            jsg::Value error = kj::mv(d);
            destroyReason = kj::none;
            js.throwException(kj::mv(error));
          }
        }, [this](jsg::Lock& js, jsg::Value&& error) {
          running = false;
          destroyReason = kj::none;
          js.throwException(kj::mv(error));
        });
      }
    }
    return js.resolvedPromise();
  } else {
    // RPC mode implementation (existing code)
    KJ_IF_SOME(client, rpcClient) {
      return IoContext::current()
          .awaitIo(js, client->monitorRequest(capnp::MessageSize{4, 0}).sendIgnoringResult())
          .then(js, [this](jsg::Lock& js) {
        running = false;
        KJ_IF_SOME(d, destroyReason) {
          jsg::Value error = kj::mv(d);
          destroyReason = kj::none;
          js.throwException(kj::mv(error));
        }
      }, [this](jsg::Lock& js, jsg::Value&& error) {
        running = false;
        destroyReason = kj::none;
        js.throwException(kj::mv(error));
      });
    }
    return js.resolvedPromise();
  }
}

jsg::Promise<void> Container::destroy(jsg::Lock& js, jsg::Optional<jsg::Value> error) {
  if (!running) return js.resolvedPromise();

  if (destroyReason == kj::none) {
    destroyReason = kj::mv(error);
  }

  if (isDockerMode) {
    // Docker mode implementation
    KJ_IF_SOME(client, dockerClient) {
      KJ_IF_SOME(cId, containerId) {
        return IoContext::current().awaitIo(js, client.stopContainer(cId));
      }
    }
    return js.resolvedPromise();
  } else {
    // RPC mode implementation (existing code)
    KJ_IF_SOME(client, rpcClient) {
      return IoContext::current().awaitIo(
          js, client->destroyRequest(capnp::MessageSize{4, 0}).sendIgnoringResult());
    }
    return js.resolvedPromise();
  }
}

void Container::signal(jsg::Lock& js, int signo) {
  JSG_REQUIRE(signo > 0 && signo <= 64, RangeError, "Invalid signal number.");
  JSG_REQUIRE(running, Error, "signal() cannot be called on a container that is not running.");

  if (isDockerMode) {
    // Docker mode implementation
    KJ_IF_SOME(client, dockerClient) {
      KJ_IF_SOME(cId, containerId) {
        IoContext::current().addTask(
          client.killContainer(cId, signo)
            .catch_([](kj::Exception&& e) {
              KJ_LOG(ERROR, "Failed to signal container", e);
            }));
      }
    }
  } else {
    // RPC mode implementation (existing code)
    KJ_IF_SOME(client, rpcClient) {
      auto req = client->signalRequest(capnp::MessageSize{4, 0});
      req.setSigno(signo);
      IoContext::current().addTask(req.sendIgnoringResult());
    }
  }
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
        "Connencting to a container using HTTPS is not currently supported; use HTTP instead. "
        "TLS is unnecessary anyway, as the connection is already secure by default.");

    // Schemes other than http: and https: should have been rejected earlier, but let's verify.
    KJ_REQUIRE(parsedUrl.scheme == "http");

    // We need to convert the URL from proxy format (full URL in request line) to host format
    // (path in request line, hostname in Host header).
    auto newHeaders = headers.cloneShallow();
    newHeaders.set(kj::HttpHeaderId::HOST, parsedUrl.host);
    auto noHostUrl = parsedUrl.toString(kj::Url::Context::HTTP_REQUEST);

    // Make a TCP connection...
    auto pipe = kj::newTwoWayPipe();
    auto connectionPromise =
        connectImpl(*pipe.ends[1]).then([]() -> kj::Promise<void> { return kj::NEVER_DONE; });

    // ... and then stack an HttpClient on it ...
    auto client = kj::newHttpClient(headerTable, *pipe.ends[0], {.entropySource = entropySource});

    // ... and then adapt that to an HttpService ...
    auto service = kj::newHttpService(*client);

    // ... and now we can just forward our call to that.
    co_await connectionPromise.exclusiveJoin(
        service->request(method, noHostUrl, newHeaders, requestBody, response));
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
    return kj::heap<TcpPortWorkerInterface>(byteStreamFactory, entropySource, headerTable, port);
  }

 private:
  capnp::ByteStreamFactory& byteStreamFactory;
  kj::EntropySource& entropySource;
  const kj::HttpHeaderTable& headerTable;
  rpc::Container::Port::Client port;
};

// Docker-specific implementations for getTcpPort()
class Container::DockerTcpPortWorkerInterface final: public WorkerInterface {
 public:
  DockerTcpPortWorkerInterface(capnp::ByteStreamFactory& byteStreamFactory,
      kj::EntropySource& entropySource,
      const kj::HttpHeaderTable& headerTable,
      io::DockerClient& dockerClient,
      kj::StringPtr containerId,
      uint16_t containerPort)
      : byteStreamFactory(byteStreamFactory),
        entropySource(entropySource),
        headerTable(headerTable),
        dockerClient(dockerClient),
        containerId(kj::str(containerId)),
        containerPort(containerPort) {}

  kj::Promise<void> request(kj::HttpMethod method,
      kj::StringPtr url,
      const kj::HttpHeaders& headers,
      kj::AsyncInputStream& requestBody,
      kj::HttpService::Response& response) override {
    KJ_UNIMPLEMENTED("Docker HTTP request not yet implemented");
  }

  kj::Promise<void> connect(kj::StringPtr host,
      const kj::HttpHeaders& headers,
      kj::AsyncIoStream& connection,
      ConnectResponse& response,
      kj::HttpConnectSettings settings) override {
    JSG_REQUIRE(!settings.useTls, Error,
        "Connecting to a container using TLS is not currently supported.");

    auto promise = dockerClient.connectToContainerPort(containerId, containerPort, connection);

    kj::HttpHeaders responseHeaders(headerTable);
    response.accept(200, "OK", responseHeaders);

    return promise;
  }

  kj::Promise<CustomEvent::Result> customEvent(kj::Own<CustomEvent> event) override {
    return event->notSupported();
  }

  kj::Promise<void> prewarm(kj::StringPtr url) override { KJ_UNREACHABLE; }
  kj::Promise<ScheduledResult> runScheduled(kj::Date scheduledTime, kj::StringPtr cron) override { KJ_UNREACHABLE; }
  kj::Promise<AlarmResult> runAlarm(kj::Date scheduledTime, uint32_t retryCount) override { KJ_UNREACHABLE; }

 private:
  capnp::ByteStreamFactory& byteStreamFactory;
  kj::EntropySource& entropySource;
  const kj::HttpHeaderTable& headerTable;
  io::DockerClient& dockerClient;
  kj::String containerId;
  uint16_t containerPort;
};

class Container::DockerTcpPortOutgoingFactory final: public Fetcher::OutgoingFactory {
 public:
  DockerTcpPortOutgoingFactory(capnp::ByteStreamFactory& byteStreamFactory,
      kj::EntropySource& entropySource,
      const kj::HttpHeaderTable& headerTable,
      io::DockerClient& dockerClient,
      kj::String containerId,
      uint16_t containerPort)
      : byteStreamFactory(byteStreamFactory),
        entropySource(entropySource),
        headerTable(headerTable),
        dockerClient(dockerClient),
        containerId(kj::mv(containerId)),
        containerPort(containerPort) {}

  kj::Own<WorkerInterface> newSingleUseClient(kj::Maybe<kj::String> cfStr) override {
    return kj::heap<DockerTcpPortWorkerInterface>(byteStreamFactory, entropySource, 
        headerTable, dockerClient, containerId, containerPort);
  }

 private:
  capnp::ByteStreamFactory& byteStreamFactory;
  kj::EntropySource& entropySource;
  const kj::HttpHeaderTable& headerTable;
  io::DockerClient& dockerClient;
  kj::String containerId;
  uint16_t containerPort;
};

jsg::Ref<Fetcher> Container::getTcpPort(jsg::Lock& js, int port) {
  JSG_REQUIRE(port > 0 && port < 65536, TypeError, "Invalid port number: ", port);

  auto& ioctx = IoContext::current();

  if (isDockerMode) {
    // Docker mode implementation
    KJ_IF_SOME(client, dockerClient) {
      KJ_IF_SOME(cId, containerId) {
        // For Docker mode, we need to connect to the host-mapped port
        // This is a simplified implementation
        kj::Own<Fetcher::OutgoingFactory> factory =
            kj::heap<DockerTcpPortOutgoingFactory>(ioctx.getByteStreamFactory(), 
                ioctx.getEntropySource(), ioctx.getHeaderTable(), client, 
                kj::str(cId), port);

        return js.alloc<Fetcher>(
            ioctx.addObject(kj::mv(factory)), Fetcher::RequiresHostAndProtocol::YES, true);
      }
    }
    // Fallback if we can't get the required components
    JSG_FAIL_REQUIRE(Error, "Container not properly initialized for Docker mode");
  } else {
    // RPC mode implementation (existing code)
    KJ_IF_SOME(client, rpcClient) {
      auto req = client->getTcpPortRequest(capnp::MessageSize{4, 0});
      req.setPort(port);

      kj::Own<Fetcher::OutgoingFactory> factory =
          kj::heap<TcpPortOutgoingFactory>(ioctx.getByteStreamFactory(), ioctx.getEntropySource(),
              ioctx.getHeaderTable(), req.send().getPort());

      return js.alloc<Fetcher>(
          ioctx.addObject(kj::mv(factory)), Fetcher::RequiresHostAndProtocol::YES, true);
    }
    JSG_FAIL_REQUIRE(Error, "Container not properly initialized for RPC mode");
  }
}

}  // namespace workerd::api
