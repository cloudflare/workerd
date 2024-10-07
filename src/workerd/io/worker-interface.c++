// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "worker-interface.h"

#include <kj/debug.h>

using kj::byte;
using kj::uint;

namespace workerd {

namespace {
// A WorkerInterface that delays requests until some promise resolves, then forwards them to the
// interface the promise resolved to.
class PromisedWorkerInterface final: public kj::Refcounted, public WorkerInterface {
public:
  PromisedWorkerInterface(kj::Promise<kj::Own<WorkerInterface>> promise)
      : promise(promise.then([this](kj::Own<WorkerInterface> result) { worker = kj::mv(result); })
                    .fork()) {}

  kj::Promise<void> request(kj::HttpMethod method,
      kj::StringPtr url,
      const kj::HttpHeaders& headers,
      kj::AsyncInputStream& requestBody,
      Response& response) override {
    KJ_IF_SOME(w, worker) {
      co_await w.get()->request(method, url, headers, requestBody, response);
    } else {
      co_await promise;
      co_await KJ_ASSERT_NONNULL(worker)->request(method, url, headers, requestBody, response);
    }
  }

  kj::Promise<void> connect(kj::StringPtr host,
      const kj::HttpHeaders& headers,
      kj::AsyncIoStream& connection,
      ConnectResponse& response,
      kj::HttpConnectSettings settings) override {
    KJ_IF_SOME(w, worker) {
      co_await w.get()->connect(host, headers, connection, response, kj::mv(settings));
    } else {
      co_await promise;
      co_await KJ_ASSERT_NONNULL(worker)->connect(
          host, headers, connection, response, kj::mv(settings));
    }
  }

  kj::Promise<void> prewarm(kj::StringPtr url) override {
    KJ_IF_SOME(w, worker) {
      co_return co_await w.get()->prewarm(url);
    } else {
      co_await promise;
      co_return co_await KJ_ASSERT_NONNULL(worker)->prewarm(url);
    }
  }

  kj::Promise<ScheduledResult> runScheduled(kj::Date scheduledTime, kj::StringPtr cron) override {
    KJ_IF_SOME(w, worker) {
      co_return co_await w.get()->runScheduled(scheduledTime, cron);
    } else {
      co_await promise;
      co_return co_await KJ_ASSERT_NONNULL(worker)->runScheduled(scheduledTime, cron);
    }
  }

  kj::Promise<AlarmResult> runAlarm(kj::Date scheduledTime, uint32_t retryCount) override {
    KJ_IF_SOME(w, worker) {
      co_return co_await w.get()->runAlarm(scheduledTime, retryCount);
    } else {
      co_await promise;
      co_return co_await KJ_ASSERT_NONNULL(worker)->runAlarm(scheduledTime, retryCount);
    }
  }

  kj::Promise<CustomEvent::Result> customEvent(kj::Own<CustomEvent> event) override {
    KJ_IF_SOME(w, worker) {
      co_return co_await w.get()->customEvent(kj::mv(event));
    } else {
      co_await promise;
      co_return co_await KJ_ASSERT_NONNULL(worker)->customEvent(kj::mv(event));
    }
  }

private:
  kj::ForkedPromise<void> promise;
  kj::Maybe<kj::Own<WorkerInterface>> worker;
};
}  // namespace

kj::Own<WorkerInterface> newPromisedWorkerInterface(kj::Promise<kj::Own<WorkerInterface>> promise) {
  return kj::refcounted<PromisedWorkerInterface>(kj::mv(promise));
}

kj::Own<kj::HttpClient> asHttpClient(kj::Own<WorkerInterface> workerInterface) {
  return kj::newHttpClient(*workerInterface).attach(kj::mv(workerInterface));
}

// =======================================================================================
namespace {
// A Revocable WebSocket wrapper, revoked when revokeProm rejects
class RevocableWebSocket final: public kj::WebSocket {
public:
  RevocableWebSocket(kj::Own<WebSocket> ws, kj::Promise<void> revokeProm)
      : ws(kj::mv(ws)),
        revokeProm(revokeProm
                       .catch_([this](kj::Exception&& e) -> kj::Promise<void> {
                         canceler.cancel(kj::cp(e));
                         KJ_IF_SOME(ws, this->ws.tryGet<kj::Own<kj::WebSocket>>()) {
                           (ws)->abort();
                         }
                         this->ws = kj::mv(e);
                         return kj::READY_NOW;
                       })
                       .eagerlyEvaluate(nullptr)) {}

  kj::Promise<void> send(kj::ArrayPtr<const byte> message) override {
    return wrap<void>(getInner().send(message));
  }
  kj::Promise<void> send(kj::ArrayPtr<const char> message) override {
    return wrap<void>(getInner().send(message));
  }

  kj::Promise<void> close(uint16_t code, kj::StringPtr reason) override {
    return wrap<void>(getInner().close(code, reason));
  }

  void disconnect() override {
    KJ_IF_SOME(ws, this->ws.tryGet<kj::Own<kj::WebSocket>>()) {
      return (ws)->disconnect();
    }
  }

  void abort() override {
    KJ_IF_SOME(ws, this->ws.tryGet<kj::Own<kj::WebSocket>>()) {
      return (ws)->abort();
    }
  }

  kj::Promise<void> whenAborted() override {
    return wrap<void>(getInner().whenAborted());
  }

  kj::Promise<Message> receive(size_t maxSize) override {
    return wrap<Message>(getInner().receive(maxSize));
  }

  kj::Promise<void> pumpTo(WebSocket& other) override {
    return wrap<void>(getInner().pumpTo(other));
  }

  kj::Maybe<kj::Promise<void>> tryPumpFrom(WebSocket& other) override {
    return wrap<void>(other.pumpTo(getInner()));
  }

  kj::Maybe<kj::String> getPreferredExtensions(ExtensionsContext ctx) override {
    return getInner().getPreferredExtensions(ctx);
  };

  uint64_t sentByteCount() override {
    return 0;
  }
  uint64_t receivedByteCount() override {
    return 0;
  }

private:
  template <typename T>
  kj::Promise<T> wrap(kj::Promise<T> prom) {
    // just to fix the revocation promise return type, serves no purpose otherwise
    return canceler.wrap(kj::mv(prom));
  }

  kj::WebSocket& getInner() {
    KJ_SWITCH_ONEOF(ws) {
      KJ_CASE_ONEOF(e, kj::Exception) {
        kj::throwFatalException(kj::cp(e));
      }
      KJ_CASE_ONEOF(ws, kj::Own<kj::WebSocket>) {
        return *ws.get();
      }
    }
    KJ_UNREACHABLE;
  }

  kj::OneOf<kj::Exception, kj::Own<kj::WebSocket>> ws;
  kj::Promise<void> revokeProm;
  kj::Canceler canceler;
};

// A HttpResponse that can revoke long-running websocket connections started as part of the
// response. Ordinary HTTP requests are not revoked.
class RevocableWebSocketHttpResponse final: public kj::HttpService::Response {
public:
  RevocableWebSocketHttpResponse(kj::HttpService::Response& inner, kj::Promise<void> revokeProm)
      : inner(inner),
        revokeProm(revokeProm.fork()) {}

  kj::Own<kj::AsyncOutputStream> send(uint statusCode,
      kj::StringPtr statusText,
      const kj::HttpHeaders& headers,
      kj::Maybe<uint64_t> expectedBodySize = kj::none) override {
    return inner.send(statusCode, statusText, headers, expectedBodySize);
  }

  kj::Own<kj::WebSocket> acceptWebSocket(const kj::HttpHeaders& headers) override {
    return kj::heap<RevocableWebSocket>(inner.acceptWebSocket(headers), revokeProm.addBranch());
  }

private:
  kj::HttpService::Response& inner;
  kj::ForkedPromise<void> revokeProm;
};

// A WorkerInterface that cancels WebSockets when revokeProm is rejected.
// Currently only supports cancelling for upgrades.
class RevocableWebSocketWorkerInterface final: public WorkerInterface {
public:
  RevocableWebSocketWorkerInterface(WorkerInterface& worker, kj::Promise<void> revokeProm);
  kj::Promise<void> request(kj::HttpMethod method,
      kj::StringPtr url,
      const kj::HttpHeaders& headers,
      kj::AsyncInputStream& requestBody,
      Response& response) override;
  kj::Promise<void> connect(kj::StringPtr host,
      const kj::HttpHeaders& headers,
      kj::AsyncIoStream& connection,
      ConnectResponse& response,
      kj::HttpConnectSettings settings) override;
  kj::Promise<void> prewarm(kj::StringPtr url) override;
  kj::Promise<ScheduledResult> runScheduled(kj::Date scheduledTime, kj::StringPtr cron) override;
  kj::Promise<AlarmResult> runAlarm(kj::Date scheduledTime, uint32_t retryCount) override;
  kj::Promise<CustomEvent::Result> customEvent(kj::Own<CustomEvent> event) override;

private:
  WorkerInterface& worker;
  kj::ForkedPromise<void> revokeProm;
};

kj::Promise<void> RevocableWebSocketWorkerInterface::request(kj::HttpMethod method,
    kj::StringPtr url,
    const kj::HttpHeaders& headers,
    kj::AsyncInputStream& requestBody,
    kj::HttpService::Response& response) {
  auto wrappedResponse = kj::heap<RevocableWebSocketHttpResponse>(response, revokeProm.addBranch());
  return worker.request(method, url, headers, requestBody, *wrappedResponse)
      .attach(kj::mv(wrappedResponse));
}

kj::Promise<void> RevocableWebSocketWorkerInterface::connect(kj::StringPtr host,
    const kj::HttpHeaders& headers,
    kj::AsyncIoStream& connection,
    ConnectResponse& response,
    kj::HttpConnectSettings settings) {
  KJ_UNIMPLEMENTED(
      "TODO(someday): RevocableWebSocketWorkerInterface::connect() should be implemented to "
      "disconnect long-lived connections similar to how it treats WebSockets");
}

RevocableWebSocketWorkerInterface::RevocableWebSocketWorkerInterface(
    WorkerInterface& worker, kj::Promise<void> revokeProm)
    : worker(worker),
      revokeProm(revokeProm.fork()) {}

kj::Promise<void> RevocableWebSocketWorkerInterface::prewarm(kj::StringPtr url) {
  return worker.prewarm(url);
}

kj::Promise<WorkerInterface::ScheduledResult> RevocableWebSocketWorkerInterface::runScheduled(
    kj::Date scheduledTime, kj::StringPtr cron) {
  return worker.runScheduled(scheduledTime, cron);
}

kj::Promise<WorkerInterface::AlarmResult> RevocableWebSocketWorkerInterface::runAlarm(
    kj::Date scheduledTime, uint32_t retryCount) {
  return worker.runAlarm(scheduledTime, retryCount);
}

kj::Promise<WorkerInterface::CustomEvent::Result> RevocableWebSocketWorkerInterface::customEvent(
    kj::Own<CustomEvent> event) {
  return worker.customEvent(kj::mv(event));
}

}  // namespace

kj::Own<WorkerInterface> newRevocableWebSocketWorkerInterface(
    kj::Own<WorkerInterface> worker, kj::Promise<void> revokeProm) {
  return kj::heap<RevocableWebSocketWorkerInterface>(*worker, kj::mv(revokeProm))
      .attach(kj::mv(worker));
}

// =======================================================================================

namespace {

class ErrorWorkerInterface final: public WorkerInterface {
public:
  ErrorWorkerInterface(kj::Exception&& exception): exception(exception) {}

  kj::Promise<void> request(kj::HttpMethod method,
      kj::StringPtr url,
      const kj::HttpHeaders& headers,
      kj::AsyncInputStream& requestBody,
      Response& response) override {
    kj::throwFatalException(kj::mv(exception));
  }

  kj::Promise<void> connect(kj::StringPtr host,
      const kj::HttpHeaders& headers,
      kj::AsyncIoStream& connection,
      ConnectResponse& response,
      kj::HttpConnectSettings settings) override {
    kj::throwFatalException(kj::mv(exception));
  }

  kj::Promise<void> prewarm(kj::StringPtr url) override {
    // ignore
    return kj::READY_NOW;
  }

  kj::Promise<ScheduledResult> runScheduled(kj::Date scheduledTime, kj::StringPtr cron) override {
    kj::throwFatalException(kj::mv(exception));
  }

  kj::Promise<AlarmResult> runAlarm(kj::Date scheduledTime, uint32_t retryCount) override {
    kj::throwFatalException(kj::mv(exception));
  }

  kj::Promise<CustomEvent::Result> customEvent(kj::Own<CustomEvent> event) override {
    kj::throwFatalException(kj::mv(exception));
  }

private:
  kj::Exception exception;
};

}  // namespace

kj::Own<WorkerInterface> WorkerInterface::fromException(kj::Exception&& e) {
  return kj::heap<ErrorWorkerInterface>(kj::mv(e));
}

// =======================================================================================

RpcWorkerInterface::RpcWorkerInterface(capnp::HttpOverCapnpFactory& httpOverCapnpFactory,
    capnp::ByteStreamFactory& byteStreamFactory,
    kj::TaskSet& waitUntilTasks,
    rpc::EventDispatcher::Client dispatcher)
    : httpOverCapnpFactory(httpOverCapnpFactory),
      byteStreamFactory(byteStreamFactory),
      waitUntilTasks(waitUntilTasks),
      dispatcher(kj::mv(dispatcher)) {}

kj::Promise<void> RpcWorkerInterface::request(kj::HttpMethod method,
    kj::StringPtr url,
    const kj::HttpHeaders& headers,
    kj::AsyncInputStream& requestBody,
    Response& response) {
  auto inner = httpOverCapnpFactory.capnpToKj(dispatcher.getHttpServiceRequest().send().getHttp());
  auto promise = inner->request(method, url, headers, requestBody, response);
  return promise.attach(kj::mv(inner));
}

kj::Promise<void> RpcWorkerInterface::connect(kj::StringPtr host,
    const kj::HttpHeaders& headers,
    kj::AsyncIoStream& connection,
    ConnectResponse& tunnel,
    kj::HttpConnectSettings settings) {
  auto inner = httpOverCapnpFactory.capnpToKj(dispatcher.getHttpServiceRequest().send().getHttp());
  auto promise = inner->connect(host, headers, connection, tunnel, kj::mv(settings));
  return promise.attach(kj::mv(inner));
}

kj::Promise<void> RpcWorkerInterface::prewarm(kj::StringPtr url) {
  auto req = dispatcher.prewarmRequest(capnp::MessageSize{url.size() / sizeof(capnp::word) + 4, 0});
  req.setUrl(url);
  return req.send().ignoreResult();
}

kj::Promise<WorkerInterface::ScheduledResult> RpcWorkerInterface::runScheduled(
    kj::Date scheduledTime, kj::StringPtr cron) {
  auto req = dispatcher.runScheduledRequest();
  req.setScheduledTime((scheduledTime - kj::UNIX_EPOCH) / kj::SECONDS);
  req.setCron(cron);
  return req.send().then([](auto resp) {
    auto respResult = resp.getResult();
    return WorkerInterface::ScheduledResult{
      .retry = respResult.getRetry(), .outcome = respResult.getOutcome()};
  });
}

kj::Promise<WorkerInterface::AlarmResult> RpcWorkerInterface::runAlarm(
    kj::Date scheduledTime, uint32_t retryCount) {
  auto req = dispatcher.runAlarmRequest();
  req.setScheduledTime((scheduledTime - kj::UNIX_EPOCH) / kj::MILLISECONDS);
  req.setRetryCount(retryCount);
  return req.send().then([](auto resp) {
    auto respResult = resp.getResult();
    return WorkerInterface::AlarmResult{.retry = respResult.getRetry(),
      .retryCountsAgainstLimit = respResult.getRetryCountsAgainstLimit(),
      .outcome = respResult.getOutcome()};
  });
}

kj::Promise<WorkerInterface::CustomEvent::Result> RpcWorkerInterface::customEvent(
    kj::Own<CustomEvent> event) {
  return event->sendRpc(httpOverCapnpFactory, byteStreamFactory, waitUntilTasks, dispatcher)
      .attach(kj::mv(event));
}

// ======================================================================================
WorkerInterface::AlarmFulfiller::AlarmFulfiller(
    kj::Own<kj::PromiseFulfiller<AlarmResult>> fulfiller)
    : maybeFulfiller(kj::mv(fulfiller)) {}

WorkerInterface::AlarmFulfiller::~AlarmFulfiller() noexcept(false) {
  KJ_IF_SOME(fulfiller, getFulfiller()) {
    fulfiller.reject(KJ_EXCEPTION(FAILED, "AlarmFulfiller destroyed without resolution"));
  }
}

void WorkerInterface::AlarmFulfiller::fulfill(const AlarmResult& result) {
  KJ_IF_SOME(fulfiller, getFulfiller()) {
    fulfiller.fulfill(kj::cp(result));
  }
}

void WorkerInterface::AlarmFulfiller::reject(const kj::Exception& e) {
  KJ_IF_SOME(fulfiller, getFulfiller()) {
    fulfiller.reject(kj::cp(e));
  }
}

void WorkerInterface::AlarmFulfiller::cancel() {
  KJ_IF_SOME(fulfiller, getFulfiller()) {
    fulfiller.fulfill(AlarmResult{
      .retry = false,
      .outcome = EventOutcome::CANCELED,
    });
  }
}

kj::Maybe<kj::PromiseFulfiller<WorkerInterface::AlarmResult>&> WorkerInterface::AlarmFulfiller::
    getFulfiller() {
  KJ_IF_SOME(fulfiller, maybeFulfiller) {
    if (fulfiller.get()->isWaiting()) {
      return *fulfiller;
    }
  }

  return kj::none;
}

}  // namespace workerd
