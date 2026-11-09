// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Measures the per-request cost of wrapping a worker with newRevocableWebSocketWorkerInterface()
// when the request sends an ordinary response, revoking through a cross-thread wait list the way
// a killed pipeline does.

#include "worker-interface.h"

#include <workerd/tests/bench-tools.h>
#include <workerd/util/stream-utils.h>
#include <workerd/util/wait-list.h>

namespace workerd {
namespace {

class SendingWorker final: public WorkerInterface {
 public:
  kj::Promise<void> request(kj::HttpMethod method,
      kj::StringPtr url,
      const kj::HttpHeaders& headers,
      kj::AsyncInputStream& requestBody,
      kj::HttpService::Response& response) override {
    response.send(200, "OK"_kj, headers, static_cast<uint64_t>(0));
    return kj::READY_NOW;
  }

  kj::Promise<void> connect(kj::StringPtr host,
      const kj::HttpHeaders& headers,
      kj::AsyncIoStream& connection,
      ConnectResponse& response,
      kj::HttpConnectSettings settings) override {
    KJ_UNIMPLEMENTED("not used in this benchmark");
  }
  kj::Promise<void> prewarm(kj::StringPtr url) override {
    KJ_UNIMPLEMENTED("not used in this benchmark");
  }
  kj::Promise<ScheduledResult> runScheduled(kj::Date scheduledTime, kj::StringPtr cron) override {
    KJ_UNIMPLEMENTED("not used in this benchmark");
  }
  kj::Promise<AlarmResult> runAlarm(kj::Date scheduledTime, uint32_t retryCount) override {
    KJ_UNIMPLEMENTED("not used in this benchmark");
  }
  kj::Promise<kj::Maybe<kj::Date>> abandonAlarm(kj::Date scheduledTime) override {
    KJ_UNIMPLEMENTED("not used in this benchmark");
  }
  kj::Promise<CustomEvent::Result> customEvent(kj::Own<CustomEvent> event) override {
    return event->notSupported();
  }
};

class DiscardingResponse final: public kj::HttpService::Response {
 public:
  kj::Own<kj::AsyncOutputStream> send(uint statusCode,
      kj::StringPtr statusText,
      const kj::HttpHeaders& headers,
      kj::Maybe<uint64_t> expectedBodySize) override {
    return newNullOutputStream();
  }

  kj::Own<kj::WebSocket> acceptWebSocket(const kj::HttpHeaders& headers) override {
    KJ_UNIMPLEMENTED("not used in this benchmark");
  }
};

enum class Revocation { EAGER, LAZY };

void benchRevocableRequest(benchmark::State& state, Revocation revocation) {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);
  kj::HttpHeaderTable headerTable;
  kj::HttpHeaders headers(headerTable);
  DiscardingResponse response;
  CrossThreadWaitList killed{{.useThreadLocalOptimization = true}};
  auto makeRevokeProm = [&killed]() {
    return killed.addWaiter().then(
        []() -> kj::Promise<void> { return KJ_EXCEPTION(FAILED, "revoked"); });
  };

  for (auto _: state) {
    auto revocable = revocation == Revocation::LAZY
        ? newRevocableWebSocketWorkerInterface(
              kj::heap<SendingWorker>(), kj::Function<kj::Promise<void>()>(makeRevokeProm))
        : newRevocableWebSocketWorkerInterface(kj::heap<SendingWorker>(), makeRevokeProm());
    auto body = newNullInputStream();
    revocable->request(kj::HttpMethod::GET, "/"_kj, headers, *body, response).wait(waitScope);
  }
}

void revocableRequestEager(benchmark::State& state) {
  benchRevocableRequest(state, Revocation::EAGER);
}

void revocableRequestLazy(benchmark::State& state) {
  benchRevocableRequest(state, Revocation::LAZY);
}

WD_BENCHMARK(revocableRequestEager)->Unit(benchmark::kNanosecond);
WD_BENCHMARK(revocableRequestLazy)->Unit(benchmark::kNanosecond);

}  // namespace
}  // namespace workerd
