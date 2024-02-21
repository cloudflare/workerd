// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/common.h>
#include <v8-locker.h>
#include <v8-platform.h>

namespace workerd::jsg {

class V8PlatformWrapper : public v8::Platform {
public:
  explicit V8PlatformWrapper(v8::Platform& inner) : inner(inner) {}

  v8::PageAllocator* GetPageAllocator() override {
    return inner.GetPageAllocator();
  }

  v8::ZoneBackingAllocator* GetZoneBackingAllocator() override {
    return inner.GetZoneBackingAllocator();
  }

  void OnCriticalMemoryPressure() override {
    return inner.OnCriticalMemoryPressure();
  }

  int NumberOfWorkerThreads() override {
    return inner.NumberOfWorkerThreads();
  }

  std::shared_ptr<v8::TaskRunner> GetForegroundTaskRunner(v8::Isolate* isolate) override {
    return inner.GetForegroundTaskRunner(isolate);
  }

  void PostTaskOnWorkerThreadImpl(v8::TaskPriority priority, std::unique_ptr<v8::Task> task,
                                  const v8::SourceLocation& location) override {
    inner.PostTaskOnWorkerThreadImpl(priority, std::make_unique<TaskWrapper>(kj::mv(task)),
                                     location);
  }

  void PostDelayedTaskOnWorkerThreadImpl(v8::TaskPriority priority, std::unique_ptr<v8::Task> task,
                                         double delay_in_seconds,
                                         const v8::SourceLocation& location) override {
    inner.PostDelayedTaskOnWorkerThreadImpl(priority, std::make_unique<TaskWrapper>(kj::mv(task)),
                                            delay_in_seconds, location);
  }

  std::unique_ptr<v8::JobHandle> CreateJobImpl(v8::TaskPriority priority,
                                               std::unique_ptr<v8::JobTask> job_task,
                                               const v8::SourceLocation& location) override {
    return inner.CreateJobImpl(priority, std::make_unique<JobTaskWrapper>(kj::mv(job_task)),
                               location);
  }

  bool IdleTasksEnabled(v8::Isolate* isolate) override {
    return inner.IdleTasksEnabled(isolate);
  }

  std::unique_ptr<v8::ScopedBlockingCall>
  CreateBlockingScope(v8::BlockingType blocking_type) override {
    return inner.CreateBlockingScope(blocking_type);
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

  StackTracePrinter GetStackTracePrinter() override {
    return inner.GetStackTracePrinter();
  }

  v8::TracingController* GetTracingController() override {
    return inner.GetTracingController();
  }

  void DumpWithoutCrashing() override {
    return inner.DumpWithoutCrashing();
  }

  v8::HighAllocationThroughputObserver* GetHighAllocationThroughputObserver() override {
    return inner.GetHighAllocationThroughputObserver();
  }

private:
  v8::Platform& inner;

  class TaskWrapper : public v8::Task {
  public:
    TaskWrapper(std::unique_ptr<v8::Task> inner);

    void Run() override;

  private:
    std::unique_ptr<v8::Task> inner;
    v8::PointerCageContext cageCtx;
  };

  class JobTaskWrapper : public v8::JobTask {
  public:
    JobTaskWrapper(std::unique_ptr<v8::JobTask> inner);

    void Run(v8::JobDelegate*) override;

    size_t GetMaxConcurrency(size_t worker_count) const override {
      return inner->GetMaxConcurrency(worker_count);
    }

  private:
    std::unique_ptr<v8::JobTask> inner;
    v8::PointerCageContext cageCtx;
  };
};

} // namespace workerd::jsg
