// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/io/worker.h>

namespace workerd {

class IoChannelFactory;
class LimitEnforcer;
class RequestObserver;
class ThreadContext;
class WorkerInterface;
class WorkerTracer;

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
    kj::Maybe<kj::Own<Worker::Actor>> actor,
    kj::Own<LimitEnforcer> limitEnforcer,
    kj::Own<void> ioContextDependency,
    kj::Own<IoChannelFactory> ioChannelFactory,
    kj::Own<RequestObserver> metrics,
    kj::TaskSet& waitUntilTasks,
    bool tunnelExceptions,
    kj::Maybe<kj::String> cfBlobJson);

}  // namespace workerd
