// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/compat/http.h>
#include <capnp/compat/http-over-capnp.h>

#include <workerd/io/outcome.capnp.h>
#include <workerd/io/worker-interface.capnp.h>

namespace workerd {

namespace jsg { class Lock; }
namespace api {
  class EventTarget;
  struct ExportedHandler;
}

class IoContext_IncomingRequest;

// An interface representing the services made available by a worker/pipeline to handle a
// request.
class WorkerInterface: public kj::HttpService {
public:
  // Constructs a WorkerInterface where any method called will throw the given exception.
  static kj::Own<WorkerInterface> fromException(kj::Exception&& e);

  // Make an HTTP request. (This method is inherited from HttpService, but re-declared here for
  // visibility.)
  virtual kj::Promise<void> request(
      kj::HttpMethod method, kj::StringPtr url, const kj::HttpHeaders& headers,
      kj::AsyncInputStream& requestBody, kj::HttpService::Response& response) = 0;
  // TODO(perf): Consider changing this to return Promise<DeferredProxy>. This would allow
  //   more resources to be dropped when merely proxying a request. However, it means we would no
  //   longer be immplementing kj::HttpService. But maybe that doesn't matter too much in practice.

  // This is the same as the inherited HttpService::connect(), but we override it to be
  // pure-virtual to force all subclasses of WorkerInterface to implement it explicitly rather
  // than get the default implementation which throws an unimplemented exception.
  virtual kj::Promise<void> connect(kj::StringPtr host,
                                    const kj::HttpHeaders& headers,
                                    kj::AsyncIoStream& connection,
                                    ConnectResponse& response,
                                    kj::HttpConnectSettings settings) = 0;

  // Hints that this worker will likely be invoked in the near future, so should be warmed up now.
  // This method should also call `prewarm()` on any subsequent pipeline stages that are expected
  // to be invoked.
  //
  // If prewarm() has to do anything asynchronous, it should use "waitUntil" tasks.
  virtual void prewarm(kj::StringPtr url) = 0;

  struct ScheduledResult {
    bool retry = true;
    EventOutcome outcome = EventOutcome::UNKNOWN;
  };

  struct AlarmResult {
    bool retry = true;
    bool retryCountsAgainstLimit = true;
    EventOutcome outcome = EventOutcome::UNKNOWN;
  };

  // Trigger a scheduled event with the given scheduled (unix timestamp) time and cron string.
  // The cron string must be valid until the returned promise completes.
  // Async work is queued in a "waitUntil" task set.
  virtual kj::Promise<ScheduledResult> runScheduled(kj::Date scheduledTime, kj::StringPtr cron) = 0;

  // Trigger an alarm event with the given scheduled (unix timestamp) time.
  virtual kj::Promise<AlarmResult> runAlarm(kj::Date scheduledTime) = 0;

  // Run the test handler. The returned promise resolves to true or false to indicate that the test
  // passed or failed. In the case of a failure, information should have already been written to
  // stderr and to the devtools; there is no need for the caller to write anything further. (If the
  // promise rejects, this indicates a bug in the test harness itself.)
  virtual kj::Promise<bool> test() { return nullptr; }
  // TODO(someday): Produce a structured test report?

  // These two constants are shared by multiple systems that invoke alarms (the production
  // implementation, and the preview implementation), whose code live in completely different
  // places. We end up defining them here mostly for lack of a better option.
  static constexpr auto ALARM_RETRY_START_SECONDS = 2; // not a duration so we can left shift it
  static constexpr auto ALARM_RETRY_MAX_TRIES = 6;

  class CustomEvent {
  public:
    struct Result {
      // Outcome for logging / metrics purposes.
      EventOutcome outcome;
    };

    // Deliver the event to an isolate in this process. `incomingRequest` has been newly-allocated
    // for this event.
    virtual kj::Promise<Result> run(
        kj::Own<IoContext_IncomingRequest> incomingRequest,
        kj::Maybe<kj::StringPtr> entrypointName) = 0;

    // Forward the event over RPC.
    virtual kj::Promise<Result> sendRpc(
        capnp::HttpOverCapnpFactory& httpOverCapnpFactory,
        capnp::ByteStreamFactory& byteStreamFactory,
        kj::TaskSet& waitUntilTasks,
        rpc::EventDispatcher::Client dispatcher) = 0;

    // Get the type for this event for logging / metrics purposes. This is intended for use by the
    // RequestObserver. The RequestObserver implementation will define what numbers correspond to
    // what types.
    virtual uint16_t getType() = 0;
  };

  // Allows delivery of a variety of event types by implementing a callback that delivers the
  // event to a particular isolate. If and when the event is delivered to an isolate,
  // `callback->run()` will be called inside a fresh IoContext::IncomingRequest to begin the
  // event.
  //
  // If the event needs to return some sort of result, it's the responsibility of the callback to
  // store that result in a side object that the event's invoker can inspect after the promise has
  // resolved.
  //
  // Note that it is guaranteed that if the returned promise is canceled, `event` will be dropped
  // immediately; if its callbacks have not run yet, they will not run at all. So, a CustomEvent
  // implementation can hold references to objects it doesn't own as long as the returned promise
  // will be canceled before those objects go away.
  [[nodiscard]] virtual kj::Promise<CustomEvent::Result> customEvent(kj::Own<CustomEvent> event) = 0;

private:
  kj::Maybe<kj::Own<kj::HttpService>> adapterService;
};

// Given a Promise for a WorkerInterface, return a WorkerInterface whose methods will first wait
// for the promise, then invoke the destination object.
kj::Own<WorkerInterface> newPromisedWorkerInterface(
    kj::TaskSet& waitUntilTasks, kj::Promise<kj::Own<WorkerInterface>> promise);
// TODO(cleanup): `waitUntilTasks` is only needed to handle `sendTraces` and `prewarm` since they
//   don't return promises. We should maybe change them to return promises?

// Adapts WorkerInterface to HttpClient, including taking ownership.
//
// (Use kj::newHttpClient() if you don't want to take ownership.)
kj::Own<kj::HttpClient> asHttpClient(kj::Own<WorkerInterface> workerInterface);

// A WorkerInterface that cancels WebSockets when revokeProm is rejected.
// Currently only supports cancelling for upgrades.
class RevocableWebSocketWorkerInterface final: public WorkerInterface {
public:
  RevocableWebSocketWorkerInterface(WorkerInterface& worker, kj::Promise<void> revokeProm);
  kj::Promise<void> request(
      kj::HttpMethod method, kj::StringPtr url, const kj::HttpHeaders& headers,
      kj::AsyncInputStream& requestBody, Response& response) override;
  kj::Promise<void> connect(kj::StringPtr host, const kj::HttpHeaders& headers,
      kj::AsyncIoStream& connection, ConnectResponse& response,
      kj::HttpConnectSettings settings) override;
  void prewarm(kj::StringPtr url) override;
  kj::Promise<ScheduledResult> runScheduled(kj::Date scheduledTime, kj::StringPtr cron) override;
  kj::Promise<AlarmResult> runAlarm(kj::Date scheduledTime) override;
  kj::Promise<CustomEvent::Result> customEvent(kj::Own<CustomEvent> event) override;

private:
  WorkerInterface& worker;
  kj::ForkedPromise<void> revokeProm;
};

kj::Own<RevocableWebSocketWorkerInterface> newRevocableWebSocketWorkerInterface(kj::Own<WorkerInterface> worker,
    kj::Promise<void> revokeProm);

// Implementation of WorkerInterface on top of rpc::EventDispatcher. Since an EventDispatcher
// is intended to be single-use, this class is also inherently single-use (i.e. only one event
// can be delivered).
class RpcWorkerInterface: public WorkerInterface {
public:
  RpcWorkerInterface(capnp::HttpOverCapnpFactory& httpOverCapnpFactory,
                     capnp::ByteStreamFactory& byteStreamFactory,
                     kj::TaskSet& waitUntilTasks,
                     rpc::EventDispatcher::Client dispatcher);

  kj::Promise<void> request(
      kj::HttpMethod method, kj::StringPtr url, const kj::HttpHeaders& headers,
      kj::AsyncInputStream& requestBody, Response& response) override;

  kj::Promise<void> connect(
    kj::StringPtr host, const kj::HttpHeaders& headers, kj::AsyncIoStream& connection,
    ConnectResponse& tunnel, kj::HttpConnectSettings settings) override;

  void prewarm(kj::StringPtr url) override;
  kj::Promise<ScheduledResult> runScheduled(kj::Date scheduledTime, kj::StringPtr cron) override;
  kj::Promise<AlarmResult> runAlarm(kj::Date scheduledTime) override;
  kj::Promise<CustomEvent::Result> customEvent(kj::Own<CustomEvent> event) override;

private:
  capnp::HttpOverCapnpFactory& httpOverCapnpFactory;
  capnp::ByteStreamFactory& byteStreamFactory;
  kj::TaskSet& waitUntilTasks;
  rpc::EventDispatcher::Client dispatcher;
};

} // namespace workerd
