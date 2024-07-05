// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Based on the dawn node bindings

#pragma once

#include <kj/timer.h>
#include <webgpu/webgpu_cpp.h>
#include <workerd/io/io-timers.h>

namespace workerd::api::gpu {

// AsyncRunner is used to poll a wgpu::Instance with calls to ProcessEvents() while there
// are asynchronous tasks in flight.
class AsyncRunner : public kj::Refcounted {
public:
  AsyncRunner(wgpu::Instance instance) : instance_(instance){};

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
// and AsyncRunner::End() on destruction.
class AsyncTask {
public:
  inline AsyncTask(AsyncTask&&) = default;

  // Constructor.
  // Calls AsyncRunner::Begin()
  explicit inline AsyncTask(kj::Own<AsyncRunner> runner) : runner_(std::move(runner)) {
    runner_->Begin();
  }

  // Destructor.
  // Calls AsyncRunner::End()
  inline ~AsyncTask() {
    runner_->End();
  }

private:
  KJ_DISALLOW_COPY(AsyncTask);
  kj::Own<AsyncRunner> runner_;
};

} // namespace workerd::api::gpu
