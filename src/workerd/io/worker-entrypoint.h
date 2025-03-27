// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/io/frankenvalue.h>
#include <workerd/io/worker.h>

namespace workerd {

class IoChannelFactory;
class LimitEnforcer;
class RequestObserver;
class ThreadContext;
class WorkerInterface;
class WorkerTracer;

namespace tracing {
class InvocationSpanContext;
};

// Create and return a wrapper around a Worker that handles receiving a new event
// from the outside. In particular,
// this handles:
// - Creating a IoContext and making it current.
// - Executing the worker under lock.
// - Catching exceptions and converting them to HTTP error responses.
//   - Or, falling back to proxying if passThroughOnException() was used.
// - Finish waitUntil() tasks.
kj::Own<WorkerInterface> newWorkerEntrypoint(ThreadContext& threadContext,
    kj::Own<const Worker> worker,
    kj::Maybe<kj::StringPtr> entrypointName,
    Frankenvalue props,
    kj::Maybe<kj::Own<Worker::Actor>> actor,
    kj::Own<LimitEnforcer> limitEnforcer,
    kj::Own<void> ioContextDependency,
    kj::Own<IoChannelFactory> ioChannelFactory,
    kj::Own<RequestObserver> metrics,
    kj::TaskSet& waitUntilTasks,
    bool tunnelExceptions,
    kj::Maybe<kj::Own<WorkerTracer>> workerTracer,
    kj::Maybe<kj::String> cfBlobJson,
    // The trigger invocation span may be propagated from other request. If it is provided,
    // the implication is that this worker entrypoint is being created as a subrequest or
    // subtask of another request. If it is kj::none, then this invocation is a top-level
    // invocation.
    kj::Maybe<tracing::InvocationSpanContext> maybeTriggerInvocationSpan = kj::none);

}  // namespace workerd
