// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <v8-platform.h>

#include <kj/common.h>

namespace workerd::server {

// Workerd-specific implementation of v8::Platform.
//
// We customize the CurrentClockTimeMillis() virtual method in order to control the value
// returned by `Date.now()`.
//
// Everything else gets passed through to the wrapped v8::Platform implementation (presumably
// from `jsg::defaultPlatform()`).
class WorkerdPlatform final: public v8::Platform {
 public:
  // This takes a reference to its wrapped platform because otherwise we would have to destroy a
  // kj::Own in our noexcept destructor (feasible but ugly).
  explicit WorkerdPlatform(v8::Platform& inner): inner(inner) {}

  ~WorkerdPlatform() noexcept {}

  // =====================================================================================
  // v8::Platform API

  v8::PageAllocator* GetPageAllocator() noexcept override {
    return inner.GetPageAllocator();
  }

  int NumberOfWorkerThreads() noexcept override {
    return inner.NumberOfWorkerThreads();
  }

  std::shared_ptr<v8::TaskRunner> GetForegroundTaskRunner(
      v8::Isolate* isolate, v8::TaskPriority priority) noexcept override {
    return inner.GetForegroundTaskRunner(isolate, priority);
  }

  void PostTaskOnWorkerThreadImpl(v8::TaskPriority priority,
      std::unique_ptr<v8::Task> task,
      const v8::SourceLocation& location) override {
    inner.PostTaskOnWorkerThreadImpl(priority, kj::mv(task), location);
  }

  void PostDelayedTaskOnWorkerThreadImpl(v8::TaskPriority priority,
      std::unique_ptr<v8::Task> task,
      double delay_in_seconds,
      const v8::SourceLocation& location) override {
    inner.PostDelayedTaskOnWorkerThreadImpl(priority, kj::mv(task), delay_in_seconds, location);
  }

  bool IdleTasksEnabled(v8::Isolate* isolate) noexcept override {
    return inner.IdleTasksEnabled(isolate);
  }

  std::unique_ptr<v8::JobHandle> CreateJobImpl(v8::TaskPriority priority,
      std::unique_ptr<v8::JobTask> job_task,
      const v8::SourceLocation& location) override {
    return inner.CreateJobImpl(priority, kj::mv(job_task), location);
  }

  double MonotonicallyIncreasingTime() noexcept override {
    return inner.MonotonicallyIncreasingTime();
  }

  // Overridden to return KJ time
  double CurrentClockTimeMillis() noexcept override;

  v8::TracingController* GetTracingController() noexcept override {
    return inner.GetTracingController();
  }

 private:
  v8::Platform& inner;
};

}  // namespace workerd::server
