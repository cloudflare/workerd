// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <v8-platform.h>

#include <kj/async.h>
#include <kj/common.h>
#include <kj/mutex.h>
#include <kj/refcount.h>
#include <kj/vector.h>

#include <atomic>

namespace workerd::jsg {

// Wakes event loops that wait for V8 to post a foreground task for an isolate. V8 posts these from
// its background threads, for example to finish an asynchronous WebAssembly compilation, and
// nothing else wakes the thread that will run them.
//
// Every post advances an epoch. A waiter reads epoch() before it starts the work that may post,
// then waits with waitAfter(), which resolves at once if the epoch has already moved, so no post
// is missed. notify() takes no lock while nothing waits. It may be called from any thread, and
// after the isolate is gone.
class ForegroundTaskSignal final: public kj::AtomicRefcounted {
 public:
  uint64_t epoch() const {
    return currentEpoch.load(std::memory_order_seq_cst);
  }

  // Advances the epoch and fulfills every promise waitAfter() returned so far.
  void notify() const;

  // Resolves once the epoch is past `seen`. Must be called on a thread with a KJ event loop.
  kj::Promise<void> waitAfter(uint64_t seen) const;

  // Drops the waiters and makes waitAfter() never resolve. Called before the isolate is disposed.
  void detach() const;

 private:
  mutable std::atomic<uint64_t> currentEpoch = 0;
  // How many waiters are registered. notify() reads it after advancing the epoch, and waitAfter()
  // writes it before reading the epoch, so either notify() sees the waiter or the waiter sees the
  // new epoch.
  mutable std::atomic<size_t> waiterCount = 0;

  struct State {
    bool detached = false;
    kj::Vector<kj::Own<kj::CrossThreadPromiseFulfiller<void>>> waiters;
  };
  kj::MutexGuarded<State> state;
};

// Returns the cached foreground task runner of the IsolateBase that owns `isolate`, or null if the
// isolate has no IsolateBase yet. Defined in setup.c++.
std::shared_ptr<v8::TaskRunner> getNotifyingForegroundTaskRunner(
    v8::Isolate* isolate, v8::TaskPriority priority);

class V8PlatformWrapper: public v8::Platform {
 public:
  explicit V8PlatformWrapper(v8::Platform& inner): inner(inner) {}

  v8::Platform& getInner() {
    return inner;
  }

  v8::PageAllocator* GetPageAllocator() override {
    return inner.GetPageAllocator();
  }

  int NumberOfWorkerThreads() override {
    return inner.NumberOfWorkerThreads();
  }

  std::shared_ptr<v8::TaskRunner> GetForegroundTaskRunner(
      v8::Isolate* isolate, v8::TaskPriority priority) override;

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

  v8::ThreadIsolatedAllocator* GetThreadIsolatedAllocator() override {
    // Forward to the inner platform so that V8's ThreadIsolation can use PKU
    // (Memory Protection Keys) to enforce W^X on JIT code pages and
    // write-protect the code pointer tables.  Without this, the
    // DefaultPlatform's allocator (which calls pkey_alloc) was silently
    // dropped and ThreadIsolation was disabled.
    return inner.GetThreadIsolatedAllocator();
  }

  // Forwards to the platform's runner for one isolate and priority. Immediate posts also notify the
  // isolate's ForegroundTaskSignal. IsolateBase creates one per priority once its isolate exists, so
  // runners V8 fetched before that, which V8 uses for some of its own tasks, don't notify.
  class NotifyingTaskRunner final: public v8::TaskRunner {
   public:
    NotifyingTaskRunner(
        std::shared_ptr<v8::TaskRunner> inner, kj::Arc<const ForegroundTaskSignal> signal)
        : inner(kj::mv(inner)),
          signal(kj::mv(signal)) {}
    ~NotifyingTaskRunner() noexcept override {}

    bool IdleTasksEnabled() override {
      return inner->IdleTasksEnabled();
    }
    bool NonNestableTasksEnabled() const override {
      return inner->NonNestableTasksEnabled();
    }
    bool NonNestableDelayedTasksEnabled() const override {
      return inner->NonNestableDelayedTasksEnabled();
    }

   protected:
    void PostTaskImpl(std::unique_ptr<v8::Task> task, const v8::SourceLocation& location) override;
    void PostNonNestableTaskImpl(
        std::unique_ptr<v8::Task> task, const v8::SourceLocation& location) override;
    void PostDelayedTaskImpl(std::unique_ptr<v8::Task> task,
        double delay_in_seconds,
        const v8::SourceLocation& location) override;
    void PostNonNestableDelayedTaskImpl(std::unique_ptr<v8::Task> task,
        double delay_in_seconds,
        const v8::SourceLocation& location) override;
    void PostIdleTaskImpl(
        std::unique_ptr<v8::IdleTask> task, const v8::SourceLocation& location) override;

   private:
    std::shared_ptr<v8::TaskRunner> inner;
    kj::Arc<const ForegroundTaskSignal> signal;
  };

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
