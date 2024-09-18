// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Based on the dawn node bindings

#pragma once

#include <workerd/io/io-context.h>
#include <workerd/io/io-timers.h>

#include <webgpu/webgpu_cpp.h>

#include <kj/timer.h>

namespace workerd::api::gpu {

// AsyncRunner is used to poll a wgpu::Instance with calls to ProcessEvents() while there
// are asynchronous tasks in flight.
class AsyncRunner: public kj::Refcounted {
public:
  AsyncRunner(wgpu::Instance instance): instance_(instance) {};

  // Begin() should be called when a new asynchronous task is started.
  // If the number of executing asynchronous tasks transitions from 0 to 1, then
  // a function will be scheduled on the main JavaScript thread to call
  // wgpu::Instance::ProcessEvents() whenever the thread is idle. This will be repeatedly
  // called until the number of executing asynchronous tasks reaches 0 again.
  void Begin();

  // End() should be called once the asynchronous task has finished.
  // Every call to Begin() should eventually result in a call to End().
  void End();

private:
  void QueueTick();
  wgpu::Instance const instance_;
  uint64_t count_ = 0;
  bool tick_queued_ = false;
  TimeoutId::Generator timeoutIdGenerator;
};

// AsyncTask is a RAII helper for calling AsyncRunner::Begin() on construction,
// and AsyncRunner::End() on destruction, that also encapsulates the promise generally
// associated with any async task.
template <typename T>
class AsyncContext: public kj::Refcounted {
public:
  inline AsyncContext(AsyncContext&&) = default;

  // Constructor.
  // Calls AsyncRunner::Begin()
  explicit inline AsyncContext(jsg::Lock& js, kj::Own<AsyncRunner> runner)
      : promise_(nullptr),
        runner_(kj::mv(runner)) {
    auto& context = IoContext::current();
    auto paf = kj::newPromiseAndFulfiller<T>();
    fulfiller_ = kj::mv(paf.fulfiller);
    promise_ = context.awaitIo(js, kj::mv(paf.promise));

    runner_->Begin();
  }

  // Destructor.
  // Calls AsyncRunner::End()
  inline ~AsyncContext() {
    runner_->End();
  }

  kj::Own<kj::PromiseFulfiller<T>> fulfiller_;
  jsg::Promise<T> promise_;

private:
  KJ_DISALLOW_COPY(AsyncContext);
  kj::Own<AsyncRunner> runner_;
};

}  // namespace workerd::api::gpu
