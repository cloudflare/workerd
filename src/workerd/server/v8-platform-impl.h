// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/jsg/setup.h>
#include <v8-platform.h>

namespace workerd::server {

class WorkerdPlatform final: public v8::Platform {
  // Workerd-specific implementation of v8::Platform.
  //
  // We customize the CurrentClockTimeMillis() virtual method in order to control the value
  // returned by `Date.now()`.
  //
  // Everything else gets passed through to the wrapped v8::Platform implementation (presumably
  // from `jsg::defaultPlatform()`).

public:
  explicit WorkerdPlatform(v8::Platform& inner)
      : inner(inner) {}
  // This takes a reference to its wrapped platform because otherwise we would have to destroy a
  // kj::Own in our noexcept destructor (feasible but ugly).

  ~WorkerdPlatform() noexcept {}

  // =====================================================================================
  // v8::Platform API

  v8::PageAllocator* GetPageAllocator() noexcept override {
    return inner.GetPageAllocator();
  }

  void OnCriticalMemoryPressure() noexcept override {
    return inner.OnCriticalMemoryPressure();
  }

  int NumberOfWorkerThreads() noexcept override {
    return inner.NumberOfWorkerThreads();
  }

  std::shared_ptr<v8::TaskRunner> GetForegroundTaskRunner(v8::Isolate* isolate) noexcept override {
    return inner.GetForegroundTaskRunner(isolate);
  }

  void CallOnWorkerThread(std::unique_ptr<v8::Task> task) noexcept override {
    return inner.CallOnWorkerThread(kj::mv(task));
  }

  void CallDelayedOnWorkerThread(std::unique_ptr<v8::Task> task,
                                 double delay_in_seconds) noexcept override {
    return inner.CallDelayedOnWorkerThread(kj::mv(task), delay_in_seconds);
  }

  bool IdleTasksEnabled(v8::Isolate* isolate) noexcept override {
    return inner.IdleTasksEnabled(isolate);
  }

  std::unique_ptr<v8::JobHandle> PostJob(
      v8::TaskPriority priority, std::unique_ptr<v8::JobTask> job_task) noexcept override {
    return inner.PostJob(priority, kj::mv(job_task));
  }

  std::unique_ptr<v8::JobHandle> CreateJob(
      v8::TaskPriority priority, std::unique_ptr<v8::JobTask> job_task) noexcept override {
    return inner.CreateJob(priority, kj::mv(job_task));
  }

  double MonotonicallyIncreasingTime() noexcept override {
    return inner.MonotonicallyIncreasingTime();
  }

  double CurrentClockTimeMillis() noexcept override;
  // Overridden to return KJ time

  StackTracePrinter GetStackTracePrinter() noexcept override {
    return inner.GetStackTracePrinter();
  }

  v8::TracingController* GetTracingController() noexcept override {
    return inner.GetTracingController();
  }

private:
  v8::Platform& inner;
};

}
