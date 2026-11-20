// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "v8-platform-wrapper.h"

#include "jsg.h"

#include <v8-platform.h>

namespace workerd::jsg {

void ForegroundTaskSignal::notify() const {
  currentEpoch.fetch_add(1, std::memory_order_seq_cst);
  if (waiterCount.load(std::memory_order_seq_cst) == 0) return;
  auto lock = state.lockExclusive();
  for (auto& waiter: lock->waiters) {
    waiter->fulfill();
  }
  lock->waiters.clear();
  waiterCount.store(0, std::memory_order_seq_cst);
}

kj::Promise<void> ForegroundTaskSignal::waitAfter(uint64_t seen) const {
  auto lock = state.lockExclusive();
  if (lock->detached) return kj::NEVER_DONE;
  waiterCount.store(lock->waiters.size() + 1, std::memory_order_seq_cst);
  if (currentEpoch.load(std::memory_order_seq_cst) != seen) {
    waiterCount.store(lock->waiters.size(), std::memory_order_seq_cst);
    return kj::READY_NOW;
  }
  auto paf = kj::newPromiseAndCrossThreadFulfiller<void>();
  lock->waiters.add(kj::mv(paf.fulfiller));
  return kj::mv(paf.promise);
}

void ForegroundTaskSignal::detach() const {
  auto lock = state.lockExclusive();
  lock->detached = true;
  lock->waiters.clear();
  waiterCount.store(0, std::memory_order_seq_cst);
}

std::shared_ptr<v8::TaskRunner> V8PlatformWrapper::GetForegroundTaskRunner(
    v8::Isolate* isolate, v8::TaskPriority priority) {
  if (auto runner = getNotifyingForegroundTaskRunner(isolate, priority)) {
    return runner;
  }
  return inner.GetForegroundTaskRunner(isolate, priority);
}

void V8PlatformWrapper::NotifyingTaskRunner::PostTaskImpl(
    std::unique_ptr<v8::Task> task, const v8::SourceLocation& location) {
  inner->PostTask(kj::mv(task), location);
  signal->notify();
}

void V8PlatformWrapper::NotifyingTaskRunner::PostNonNestableTaskImpl(
    std::unique_ptr<v8::Task> task, const v8::SourceLocation& location) {
  inner->PostNonNestableTask(kj::mv(task), location);
  signal->notify();
}

void V8PlatformWrapper::NotifyingTaskRunner::PostDelayedTaskImpl(
    std::unique_ptr<v8::Task> task, double delay_in_seconds, const v8::SourceLocation& location) {
  // The pump doesn't wait, so a delayed task runs on the first pump after it is due. Nothing
  // waits on these today, so they don't notify.
  inner->PostDelayedTask(kj::mv(task), delay_in_seconds, location);
}

void V8PlatformWrapper::NotifyingTaskRunner::PostNonNestableDelayedTaskImpl(
    std::unique_ptr<v8::Task> task, double delay_in_seconds, const v8::SourceLocation& location) {
  inner->PostNonNestableDelayedTask(kj::mv(task), delay_in_seconds, location);
}

void V8PlatformWrapper::NotifyingTaskRunner::PostIdleTaskImpl(
    std::unique_ptr<v8::IdleTask> task, const v8::SourceLocation& location) {
  inner->PostIdleTask(kj::mv(task), location);
}

V8PlatformWrapper::JobTaskWrapper::JobTaskWrapper(std::unique_ptr<v8::JobTask> inner)
    : inner(kj::mv(inner)) {}

void V8PlatformWrapper::JobTaskWrapper::Run(v8::JobDelegate* delegate) {
  runInV8Stack([&](jsg::V8StackScope& stackScope) { inner->Run(delegate); });
}

}  // namespace workerd::jsg
