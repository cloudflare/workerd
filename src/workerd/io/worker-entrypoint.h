// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "io-context.h"
#include <workerd/io/worker-interface.h>
#include <kj/compat/http.h>

namespace workerd {

class WorkerTracer;

class WorkerEntrypoint final: public WorkerInterface {
  // Wrapper around a Worker that handles receiving a new event from the outside. In particular,
  // this handles:
  // - Creating a IoContext and making it current.
  // - Executing the worker under lock.
  // - Catching exceptions and converting them to HTTP error responses.
  //   - Or, falling back to proxying if passThroughOnException() was used.
  // - Finish waitUntil() tasks.
public:
  static kj::Own<WorkerInterface> construct(
                   ThreadContext& threadContext,
                   kj::Own<const Worker> worker,
                   kj::Maybe<kj::StringPtr> entrypointName,
                   kj::Maybe<kj::Own<Worker::Actor>> actor,
                   kj::Own<LimitEnforcer> limitEnforcer,
                   kj::Own<void> ioContextDependency,
                   kj::Own<IoChannelFactory> ioChannelFactory,
                   kj::Own<RequestObserver> metrics,
                   kj::TaskSet& waitUntilTasks,
                   bool tunnelExceptions,
                   kj::Maybe<kj::Own<WorkerTracer>> workerTracer,
                   kj::Maybe<kj::String> cfBlobJson);
  // Call this instead of the constructor. It actually adds a wrapper object around the
  // `WorkerEntrypoint`, but the wrapper still implements `WorkerInterface`.
  //
  // WorkerEntrypoint will create a IoContext, and that IoContext may outlive the
  // WorkerEntrypoint by means of a waitUntil() task. Any object(s) which must be kept alive to
  // support the worker for the lifetime of the IoContext (e.g., subsequent pipeline stages)
  // must be passed in via `ioContextDependency`.
  //
  // If this is NOT a zone worker, then `zoneDefaultWorkerLimits` should be a default instance of
  // WorkerLimits::Reader. Hence this is not necessarily the same as
  // topLevelRequest.getZoneDefaultWorkerLimits(), since the top level request may be shared between
  // zone and non-zone workers.

  kj::Promise<void> request(
      kj::HttpMethod method, kj::StringPtr url, const kj::HttpHeaders& headers,
      kj::AsyncInputStream& requestBody, Response& response) override;
  kj::Promise<void> connect(kj::StringPtr host, const kj::HttpHeaders& headers,
      kj::AsyncIoStream& connection, ConnectResponse& response,
      kj::HttpConnectSettings settings) override;
  void prewarm(kj::StringPtr url) override;
  kj::Promise<ScheduledResult> runScheduled(kj::Date scheduledTime, kj::StringPtr cron) override;
  kj::Promise<AlarmResult> runAlarm(kj::Date scheduledTime) override;
  kj::Promise<bool> test() override;
  kj::Promise<CustomEvent::Result> customEvent(kj::Own<CustomEvent> event) override;

private:
  class ResponseSentTracker;

  ThreadContext& threadContext;
  kj::TaskSet& waitUntilTasks;
  kj::Maybe<kj::Own<IoContext::IncomingRequest>> incomingRequest;
  bool tunnelExceptions;
  kj::Maybe<kj::StringPtr> entrypointName;
  kj::Maybe<kj::String> cfBlobJson;
  // Members initialized at startup.

  kj::Maybe<kj::Promise<void>> proxyTask;
  kj::Maybe<kj::Own<kj::HttpClient>> failOpenClient;
  bool loggedExceptionEarlier = false;
  // Hacky members used to hold some temporary state while processing a request.
  // See gory details in WorkerEntrypoint::request().

  void init(
      kj::Own<const Worker> worker,
      kj::Maybe<kj::Own<Worker::Actor>> actor,
      kj::Own<LimitEnforcer> limitEnforcer,
      kj::Own<void> ioContextDependency,
      kj::Own<IoChannelFactory> ioChannelFactory,
      kj::Own<RequestObserver> metrics,
      kj::Maybe<kj::Own<WorkerTracer>> workerTracer);

  template <typename T>
  void maybeAddGcPassForTest(IoContext& context, kj::Promise<T>& promise);

public:  // For kj::heap() only; pretend this is private.
  WorkerEntrypoint(kj::Badge<WorkerEntrypoint> badge,
                   ThreadContext& threadContext,
                   kj::TaskSet& waitUntilTasks,
                   bool tunnelExceptions,
                   kj::Maybe<kj::StringPtr> entrypointName,
                   kj::Maybe<kj::String> cfBlobJson);
};

} // namespace workerd
