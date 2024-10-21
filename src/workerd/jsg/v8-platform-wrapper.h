// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <v8-platform.h>

#include <kj/common.h>

namespace workerd::jsg {

class V8PlatformWrapper: public v8::Platform {
public:
  explicit V8PlatformWrapper(v8::Platform& inner): inner(inner) {}

  v8::PageAllocator* GetPageAllocator() override {
    return inner.GetPageAllocator();
  }

  int NumberOfWorkerThreads() override {
    return inner.NumberOfWorkerThreads();
  }

  std::shared_ptr<v8::TaskRunner> GetForegroundTaskRunner(
      v8::Isolate* isolate, v8::TaskPriority priority) override {
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

  std::unique_ptr<v8::JobHandle> CreateJobImpl(v8::TaskPriority priority,
      std::unique_ptr<v8::JobTask> job_task,
      const v8::SourceLocation& location) override {
    return inner.CreateJobImpl(
        priority, std::make_unique<JobTaskWrapper>(kj::mv(job_task)), location);
  }

  bool IdleTasksEnabled(v8::Isolate* isolate) override {
    return inner.IdleTasksEnabled(isolate);
  }

  double MonotonicallyIncreasingTime() override {
    return inner.MonotonicallyIncreasingTime();
  }

  int64_t CurrentClockTimeMilliseconds() override {
    return inner.CurrentClockTimeMilliseconds();
  }

  double CurrentClockTimeMillis() override {
    return inner.CurrentClockTimeMillis();
  }

  double CurrentClockTimeMillisecondsHighResolution() override {
    return inner.CurrentClockTimeMillisecondsHighResolution();
  }

  v8::TracingController* GetTracingController() override {
    return inner.GetTracingController();
  }

private:
  v8::Platform& inner;

  class JobTaskWrapper: public v8::JobTask {
  public:
    JobTaskWrapper(std::unique_ptr<v8::JobTask> inner);

    void Run(v8::JobDelegate*) override;

    size_t GetMaxConcurrency(size_t worker_count) const override {
      return inner->GetMaxConcurrency(worker_count);
    }

  private:
    std::unique_ptr<v8::JobTask> inner;
  };
};

}  // namespace workerd::jsg
