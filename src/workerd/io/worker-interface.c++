// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "worker-interface.h"
#include <kj/debug.h>
#include <workerd/util/own-util.h>

namespace workerd {

class PromisedWorkerInterface final: public kj::Refcounted, public WorkerInterface {
  // A WorkerInterface that delays requests until some promise resolves, then forwards them to the
  // interface the promise resolved to.

public:
  PromisedWorkerInterface(kj::TaskSet& waitUntilTasks,
                          kj::Promise<kj::Own<WorkerInterface>> promise)
      : waitUntilTasks(waitUntilTasks),
        promise(promise.then([this](kj::Own<WorkerInterface> result) {
          worker = kj::mv(result);
        }).fork()) {}

  kj::Promise<void> request(
      kj::HttpMethod method, kj::StringPtr url, const kj::HttpHeaders& headers,
      kj::AsyncInputStream& requestBody, Response& response) override {
    KJ_IF_MAYBE(w, worker) {
      return w->get()->request(method, url, headers, requestBody, response);
    } else {
      return promise.addBranch().then([this,method,url,&headers,&requestBody,&response]() {
        return KJ_ASSERT_NONNULL(worker)
            ->request(method, url, headers, requestBody, response);
      });
    }
  }

  void prewarm(kj::StringPtr url) override {
    KJ_IF_MAYBE(w, worker) {
      w->get()->prewarm(url);
    } else {
      waitUntilTasks.add(promise.addBranch().then([this, url=kj::str(url)]() mutable {
        KJ_ASSERT_NONNULL(worker)->prewarm(url);
      }).attach(kj::addRef(*this)));
    }
  }

  kj::Promise<ScheduledResult> runScheduled(kj::Date scheduledTime, kj::StringPtr cron) override {
    KJ_IF_MAYBE(w, worker) {
      return w->get()->runScheduled(scheduledTime, cron);
    } else {
      return promise.addBranch().then([this, scheduledTime, cron]() mutable {
        return KJ_ASSERT_NONNULL(worker)->runScheduled(scheduledTime, cron);
      });
    }
  }

  kj::Promise<AlarmResult> runAlarm(kj::Date scheduledTime) override {
    KJ_IF_MAYBE(w, worker) {
      return w->get()->runAlarm(scheduledTime);
    } else {
      return promise.addBranch().then([this, scheduledTime]() mutable {
        return KJ_ASSERT_NONNULL(worker)->runAlarm(scheduledTime);
      });
    }
  }

  kj::Promise<CustomEvent::Result> customEvent(kj::Own<CustomEvent> event) override {
    KJ_IF_MAYBE(w, worker) {
      return w->get()->customEvent(kj::mv(event));
    } else {
      return promise.addBranch().then([this, event = kj::mv(event)]() mutable {
        return KJ_ASSERT_NONNULL(worker)->customEvent(kj::mv(event));
      });
    }
  }

private:
  kj::TaskSet& waitUntilTasks;
  kj::ForkedPromise<void> promise;
  kj::Maybe<kj::Own<WorkerInterface>> worker;
};

kj::Own<WorkerInterface> newPromisedWorkerInterface(
    kj::TaskSet& waitUntilTasks, kj::Promise<kj::Own<WorkerInterface>> promise) {
  return kj::refcounted<PromisedWorkerInterface>(waitUntilTasks, kj::mv(promise));
}

kj::Own<kj::HttpClient> asHttpClient(kj::Own<WorkerInterface> workerInterface) {
  return kj::newHttpClient(*workerInterface).attach(kj::mv(workerInterface));
}

class RevocableWebSocket final: public kj::WebSocket {
  // A Revocable WebSocket wrapper, revoked when revokeProm rejects
public:
  RevocableWebSocket(kj::Own<WebSocket> ws, kj::Promise<void> revokeProm)
      : ws(kj::mv(ws)), revokeProm(revokeProm.catch_([this](kj::Exception&& e) -> kj::Promise<void> {
        canceler.cancel(kj::cp(e));
        KJ_IF_MAYBE(ws, this->ws.tryGet<kj::Own<kj::WebSocket>>()) {
          (*ws)->abort();
        }
        this->ws = kj::mv(e);
        return kj::READY_NOW;
      }).eagerlyEvaluate(nullptr)) {}

  kj::Promise<void> send(kj::ArrayPtr<const byte> message) override {
    return wrap<void>(getInner().send(message));
  }
  kj::Promise<void> send(kj::ArrayPtr<const char> message) override {
    return wrap<void>(getInner().send(message));
  }

  kj::Promise<void> close(uint16_t code, kj::StringPtr reason) override {
    return wrap<void>(getInner().close(code, reason));
  }

  kj::Promise<void> disconnect() override {
    KJ_IF_MAYBE(ws, this->ws.tryGet<kj::Own<kj::WebSocket>>()) {
      return wrap<void>((*ws)->disconnect());
    }
    return kj::READY_NOW;
  }

  void abort() override {
    KJ_IF_MAYBE(ws, this->ws.tryGet<kj::Own<kj::WebSocket>>()) {
      return (*ws)->abort();
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

  uint64_t sentByteCount() override { return 0; }
  uint64_t receivedByteCount() override { return 0; }

private:
  template<typename T>
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

class RevocableHttpResponse final : public kj::HttpService::Response {
  // A HttpResponse that is revokable (including websocket connections started as part of the
  // response)
public:
  RevocableHttpResponse(kj::HttpService::Response& inner, kj::Promise<void> revokeProm)
      : inner(inner), revokeProm(revokeProm.fork()) {}

  kj::Own<kj::AsyncOutputStream> send(
      uint statusCode, kj::StringPtr statusText, const kj::HttpHeaders& headers,
      kj::Maybe<uint64_t> expectedBodySize = nullptr) override {
    return inner.send(statusCode, statusText, headers, expectedBodySize);
  }

  kj::Own<kj::WebSocket> acceptWebSocket(const kj::HttpHeaders& headers) override {
    return kj::heap<RevocableWebSocket>(inner.acceptWebSocket(headers), revokeProm.addBranch());
  }

private:
  kj::HttpService::Response& inner;
  kj::ForkedPromise<void> revokeProm;
};

kj::Promise<void> RevocableWorkerInterface::request(
    kj::HttpMethod method, kj::StringPtr url, const kj::HttpHeaders& headers,
    kj::AsyncInputStream& requestBody, kj::HttpService::Response& response) {
  auto wrappedResponse = kj::heap<RevocableHttpResponse>(response, revokeProm.addBranch());
  return worker.request(method, url, headers, requestBody, *wrappedResponse)
      .attach(kj::mv(wrappedResponse));
}

RevocableWorkerInterface::RevocableWorkerInterface(WorkerInterface& worker,
    kj::Promise<void> revokeProm)
    : worker(worker), revokeProm(revokeProm.fork()) {}

void RevocableWorkerInterface::prewarm(kj::StringPtr url) {
  worker.prewarm(url);
}

kj::Promise<WorkerInterface::ScheduledResult> RevocableWorkerInterface::runScheduled(
    kj::Date scheduledTime,
    kj::StringPtr cron) {
  return worker.runScheduled(scheduledTime, cron);
}

kj::Promise<WorkerInterface::AlarmResult> RevocableWorkerInterface::runAlarm(kj::Date scheduledTime) {
  return worker.runAlarm(scheduledTime);
}

kj::Promise<WorkerInterface::CustomEvent::Result>
    RevocableWorkerInterface::customEvent(kj::Own<CustomEvent> event) {
  return worker.customEvent(kj::mv(event));
}

kj::Own<RevocableWorkerInterface> newRevocableWorkerInterface(kj::Own<WorkerInterface> worker,
    kj::Promise<void> revokeProm) {
  return kj::heap<RevocableWorkerInterface>(*worker, kj::mv(revokeProm)).attach(kj::mv(worker));
}

// =======================================================================================

namespace {

class ErrorWorkerInterface final: public WorkerInterface {
public:
  ErrorWorkerInterface(kj::Exception&& exception): exception(exception) {}

  kj::Promise<void> request(
      kj::HttpMethod method, kj::StringPtr url, const kj::HttpHeaders& headers,
      kj::AsyncInputStream& requestBody, Response& response) override {
    kj::throwFatalException(kj::mv(exception));
  }

  void prewarm(kj::StringPtr url) override {
    // ignore
  }

  kj::Promise<ScheduledResult> runScheduled(kj::Date scheduledTime, kj::StringPtr cron) override {
    kj::throwFatalException(kj::mv(exception));
  }

  kj::Promise<AlarmResult> runAlarm(kj::Date scheduledTime) override {
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

kj::Promise<void> RpcWorkerInterface::request(
    kj::HttpMethod method, kj::StringPtr url, const kj::HttpHeaders& headers,
    kj::AsyncInputStream& requestBody, Response& response) {
  auto inner = httpOverCapnpFactory.capnpToKj(dispatcher.getHttpServiceRequest().send().getHttp());
  auto promise = inner->request(method, url, headers, requestBody, response);
  return promise.attach(kj::mv(inner));
}

kj::Promise<void> RpcWorkerInterface::connect(
    kj::StringPtr host, const kj::HttpHeaders& headers, kj::AsyncIoStream& connection,
    ConnectResponse& tunnel) {
  auto inner = httpOverCapnpFactory.capnpToKj(dispatcher.getHttpServiceRequest().send().getHttp());
  auto promise = inner->connect(host, headers, connection, tunnel);
  return promise.attach(kj::mv(inner));
}


void RpcWorkerInterface::prewarm(kj::StringPtr url) {
  auto req = dispatcher.prewarmRequest(
      capnp::MessageSize { url.size() / sizeof(capnp::word) + 4, 0 });
  req.setUrl(url);
  waitUntilTasks.add(req.send().ignoreResult());
}

kj::Promise<WorkerInterface::ScheduledResult> RpcWorkerInterface::runScheduled(
    kj::Date scheduledTime,
    kj::StringPtr cron) {
  auto req = dispatcher.runScheduledRequest();
  req.setScheduledTime((scheduledTime - kj::UNIX_EPOCH) / kj::SECONDS);
  req.setCron(cron);
  return req.send().then([](auto resp) {
    auto respResult = resp.getResult();
    return WorkerInterface::ScheduledResult {
      .retry = respResult.getRetry(),
      .outcome = respResult.getOutcome()
    };
  });
}

kj::Promise<WorkerInterface::AlarmResult> RpcWorkerInterface::runAlarm(kj::Date scheduledTime) {
  auto req = dispatcher.runAlarmRequest();
  req.setScheduledTime((scheduledTime - kj::UNIX_EPOCH) / kj::MILLISECONDS);
  return req.send().then([](auto resp) {
    auto respResult = resp.getResult();
    return WorkerInterface::AlarmResult {
      .retry = respResult.getRetry(),
      .retryCountsAgainstLimit = respResult.getRetryCountsAgainstLimit(),
      .outcome = respResult.getOutcome()
    };
  });
}

kj::Promise<WorkerInterface::CustomEvent::Result>
    RpcWorkerInterface::customEvent(kj::Own<CustomEvent> event) {
  return event->sendRpc(httpOverCapnpFactory, byteStreamFactory, waitUntilTasks, dispatcher);
}

} // namespace workerd

