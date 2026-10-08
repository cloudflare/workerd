// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "async-trace.h"

#include <workerd/rust/async-trace/ffi.rs.h>

namespace workerd {

namespace _ {
thread_local const AsyncTracker* trackerInTurn = nullptr;
}  // namespace _

namespace {

// The address of a thread-local identifies the current thread cheaply.
thread_local const char threadMarker = 0;

const void* currentThread() {
  return &threadMarker;
}

// Strings cross to Rust as bytes; Rust decodes them lossily, so this cannot throw.
::rust::Slice<const uint8_t> toRust(kj::StringPtr str) {
  return ::rust::Slice<const uint8_t>(str.asBytes().begin(), str.size());
}

}  // namespace

// =======================================================================================
// AsyncTraceIsolate

AsyncTraceIsolate::AsyncTraceIsolate(): impl(rust::async_trace::new_isolate()) {}
AsyncTraceIsolate::~AsyncTraceIsolate() noexcept(false) {}

// =======================================================================================
// AsyncTraceWriter

kj::Own<AsyncTraceWriter> AsyncTraceWriter::open(
    kj::StringPtr path, kj::StringPtr producerVersion) {
  return kj::heap<AsyncTraceWriter>(
      rust::async_trace::open_ndjson_writer(toRust(path), toRust(producerVersion)));
}

AsyncTraceWriter::AsyncTraceWriter(::rust::Box<rust::async_trace::Writer> impl)
    : impl(kj::mv(impl)) {}
AsyncTraceWriter::~AsyncTraceWriter() noexcept(false) {}

bool AsyncTraceWriter::failed() const {
  return impl->failed();
}

// =======================================================================================
// AsyncTraceSinks

void AsyncTraceSinks::add(kj::Own<AsyncTraceListener> listener) {
  listeners.add(kj::mv(listener));
}

void AsyncTraceSinks::addNdjson(const AsyncTraceWriter& writer) {
  writers.add(&writer);
}

// =======================================================================================
// AsyncTracker

kj::Maybe<kj::Arc<AsyncTracker>> AsyncTracker::tryCreate(const AsyncTraceIsolate& isolate,
    AsyncTraceSinks&& sinks,
    kj::StringPtr worker,
    kj::Maybe<kj::StringPtr> actor) {
  if (sinks.empty()) return kj::none;

  auto impl =
      rust::async_trace::new_tracker(*isolate.impl, toRust(worker), toRust(actor.orDefault(""_kj)));
  for (auto& listener: sinks.listeners) {
    impl->add_cpp_sink(kj::mv(listener));
  }
  for (auto writer: sinks.writers) {
    impl->add_ndjson_sink(*writer->impl);
  }
  return kj::arc<AsyncTracker>(kj::mv(impl));
}

AsyncTracker::AsyncTracker(::rust::Box<rust::async_trace::Tracker> impl)
    : impl(kj::mv(impl)),
      ownerThread(currentThread()) {}

AsyncTracker::~AsyncTracker() noexcept(false) {}

bool AsyncTracker::onOwnerThread() const {
  if (currentThread() == ownerThread) return true;
  foreignThreadCalls.fetch_add(1, std::memory_order_relaxed);
  return false;
}

AsyncResource AsyncTracker::create(AsyncKind kind, kj::StringPtr name, AsyncId trigger) const {
  if (!onOwnerThread()) return {};
  AsyncId id = impl->create(static_cast<uint8_t>(kind), toRust(name), trigger, 0);
  if (id == 0) return {};
  return AsyncResource(addRefToThis(), id);
}

AsyncId AsyncTracker::current() const {
  if (!onOwnerThread()) return 0;
  return impl->current();
}

AsyncResource AsyncTracker::adoptOrCreate(AsyncKind kind, kj::StringPtr name) const {
  if (!onOwnerThread()) return {};
  AsyncId id = impl->adopt_operation();
  if (id == 0) return create(kind, name);
  return AsyncResource(addRefToThis(), id);
}

void AsyncTracker::markBound(AsyncId id) const {
  if (!onOwnerThread()) return;
  impl->mark_bound(id);
}

void AsyncTracker::close() const {
  if (!onOwnerThread()) return;
  impl->count_foreign_thread(foreignThreadCalls.exchange(0, std::memory_order_relaxed));
  impl->close();
}

void AsyncTracker::settle(AsyncId id, AsyncOutcome outcome) const {
  if (!onOwnerThread()) return;
  impl->settle(id, static_cast<uint8_t>(outcome));
}

void AsyncTracker::annotate(AsyncId id, kj::StringPtr key, kj::StringPtr value) const {
  if (!onOwnerThread()) return;
  impl->annotate(id, toRust(key), toRust(value));
}

void AsyncTracker::destroy(AsyncId id) const {
  if (!onOwnerThread()) return;
  impl->destroy(id);
}

void AsyncTracker::enter(AsyncId id) const {
  if (!onOwnerThread()) return;
  impl->enter(id);
}

void AsyncTracker::exit(AsyncId id) const {
  if (!onOwnerThread()) return;
  impl->exit(id);
}

void AsyncTracker::setTurnCause(AsyncId id) const {
  if (!onOwnerThread()) return;
  impl->set_turn_cause(id);
}

void AsyncTracker::turnBegin(AsyncId defaultCause) const {
  if (!onOwnerThread()) return;
  impl->turn_begin(defaultCause);
}

void AsyncTracker::turnLocked() const {
  if (!onOwnerThread()) return;
  impl->turn_locked();
}

void AsyncTracker::turnEnd() const {
  if (!onOwnerThread()) return;
  impl->turn_end();
}

// =======================================================================================
// OwnedAsyncTracker

OwnedAsyncTracker::OwnedAsyncTracker(kj::Maybe<kj::Arc<AsyncTracker>> maybeTracker) {
  KJ_IF_SOME(t, maybeTracker) {
    tracker = kj::mv(t);
  }
}

OwnedAsyncTracker::~OwnedAsyncTracker() noexcept(false) {
  if (tracker != nullptr) tracker->close();
}

}  // namespace workerd
