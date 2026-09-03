// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "common.h"

#include "../util.h"

#include <workerd/util/use-perfetto-categories.h>

namespace workerd::api {

namespace {

class PerfettoStreamWrite final {
 public:
  explicit PerfettoStreamWrite(size_t bytes) {
    TRACE_EVENT_BEGIN(WORKERD_TRACE_CATEGORY("io"), "WritableStream asynchronous write",
        PERFETTO_TRACK_FROM_POINTER(this), PERFETTO_FLOW_FROM_POINTER(this), "bytes", bytes);
  }

  ~PerfettoStreamWrite() noexcept {
    TRACE_EVENT_END(WORKERD_TRACE_CATEGORY("io"), PERFETTO_TRACK_FROM_POINTER(this),
        PERFETTO_TERMINATING_FLOW_FROM_POINTER(this));
  }

  KJ_DISALLOW_COPY_AND_MOVE(PerfettoStreamWrite);
};

template <typename GetByteCount, typename Func>
kj::Promise<void> traceStreamWrite(GetByteCount&& getByteCount, Func&& func) {
  kj::Own<PerfettoStreamWrite> trace;
  if (TRACE_EVENT_CATEGORY_ENABLED(WORKERD_TRACE_CATEGORY("io"))) {
    trace = kj::heap<PerfettoStreamWrite>(getByteCount());
  }

  auto promise = func();
  if (trace.get() != nullptr) {
    return kj::mv(promise).attach(kj::mv(trace));
  }
  return kj::mv(promise);
}

}  // namespace

kj::Promise<void> WritableStreamSink::writeWithBackpressureTracing(
    kj::ArrayPtr<const byte> buffer) {
  return traceStreamWrite([&]() { return buffer.size(); }, [&]() { return write(buffer); });
}

kj::Promise<void> WritableStreamSink::writeWithBackpressureTracing(
    kj::ArrayPtr<const kj::ArrayPtr<const byte>> pieces) {
  return traceStreamWrite([&]() {
    size_t bytes = 0;
    for (auto piece: pieces) {
      bytes += piece.size();
    }
    return bytes;
  }, [&]() { return write(pieces); });
}

WritableStreamController::PendingAbort::PendingAbort(
    jsg::Lock& js, jsg::PromiseResolverPair<void> prp, jsg::JsValue reason, bool reject)
    : resolver(kj::mv(prp.resolver)),
      promise(kj::mv(prp.promise)),
      reason(reason.addRef(js)),
      reject(reject) {}

WritableStreamController::PendingAbort::PendingAbort(
    jsg::Lock& js, jsg::JsValue reason, bool reject)
    : WritableStreamController::PendingAbort(js, js.newPromiseAndResolver<void>(), reason, reject) {
}

void WritableStreamController::PendingAbort::complete(jsg::Lock& js) {
  if (reject) {
    fail(js, reason.getHandle(js));
  } else {
    maybeResolvePromise(js, resolver);
  }
}

void WritableStreamController::PendingAbort::fail(jsg::Lock& js, jsg::JsValue reason) {
  maybeRejectPromise<void>(js, resolver, reason);
}

// =======================================================================================
// MemoryInputStream

namespace {

// A ReadableStreamSource backed by in-memory data that does NOT support deferred proxying.
// This is critical when the backing memory may have V8 heap provenance - if we allowed
// deferred proxying, the IoContext could complete and V8 GC could free the memory while
// the deferred pump is still running, causing a use-after-free.
class MemoryInputStream final: public ReadableStreamSource {
 public:
  MemoryInputStream(kj::ArrayPtr<const kj::byte> bytes, kj::Maybe<kj::Own<void>> backing)
      : backing(kj::mv(backing)),
        unread(bytes) {}

  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    size_t amount = kj::min(maxBytes, unread.size());
    if (amount > 0) {
      memcpy(buffer, unread.begin(), amount);
      unread = unread.slice(amount, unread.size());
    }
    return amount;
  }

  kj::Maybe<uint64_t> tryGetLength(StreamEncoding encoding) override {
    if (encoding == StreamEncoding::IDENTITY) {
      return unread.size();
    }
    return kj::none;
  }

  kj::Promise<DeferredProxy<void>> pumpTo(kj::Ptr<WritableStreamSink> output, bool end) override {
    // Explicitly NOT using KJ_CO_MAGIC BEGIN_DEFERRED_PROXYING here!
    // The backing memory may be tied to V8 heap (e.g., ArrayBuffer, Blob data),
    // so we must complete all I/O before the IoContext can be released.
    if (unread.size() > 0) {
      auto data = unread;
      unread = nullptr;
      co_await output->writeWithBackpressureTracing(data);
    }
    if (end) {
      co_await output->end();
    }
    co_return;
  }

  void cancel(kj::Exception reason) override {
    // Nothing to do - we're just reading from memory.
    unread = nullptr;
  }

 private:
  kj::Maybe<kj::Own<void>> backing;
  kj::ArrayPtr<const kj::byte> unread;
};

// An AsyncInputStream wrapper that translates tee-related kj::Exceptions from read
// operations into jsg::Exceptions.
// TODO(later): We might be able to get rid of this and use a KJ exception detail instead.
class TeeErrorAdapter final: public kj::AsyncInputStream {
 public:
  static kj::Own<kj::AsyncInputStream> wrap(kj::Own<kj::AsyncInputStream> inner) {
    // We make a best effort to avoid double-wrapping.
    if (dynamic_cast<TeeErrorAdapter*>(inner.get()) == nullptr) {
      return kj::heap<TeeErrorAdapter>(kj::mv(inner));
    } else {
      return kj::mv(inner);
    }
  }

  explicit TeeErrorAdapter(kj::Own<AsyncInputStream> inner): inner(kj::mv(inner)) {}

  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    return translateErrors([&] { return inner->tryRead(buffer, minBytes, maxBytes); });
  }

  kj::Maybe<uint64_t> tryGetLength() override {
    return inner->tryGetLength();
  };

  kj::Promise<uint64_t> pumpTo(kj::AsyncOutputStream& output, uint64_t amount) override {
    return translateErrors([&] { return inner->pumpTo(output, amount); });
  }

  kj::Maybe<kj::Own<kj::AsyncInputStream>> tryTee(uint64_t limit) override {
    return inner->tryTee(limit);
  }

 private:
  kj::Own<AsyncInputStream> inner;

  template <typename Func>
  static auto translateErrors(Func&& f) -> decltype(kj::fwd<Func>(f)()) {
    try {
      co_return co_await f();
    } catch (...) {
      auto exception = kj::getCaughtExceptionAsKj();
      KJ_IF_SOME(translated,
          translateKjException(exception,
              {
                {"tee buffer size limit exceeded"_kj,
                  "ReadableStream.tee() buffer limit exceeded. This error usually occurs "
                  "when a Request or Response with a large body is cloned, then only one "
                  "of the clones is read, forcing the Workers runtime to buffer the entire "
                  "body in memory. To fix this issue, remove unnecessary calls to "
                  "Request/Response.clone() and ReadableStream.tee(), and always read "
                  "clones/tees in parallel."_kj},
              })) {
        kj::throwFatalException(kj::mv(translated));
      } else {
        kj::throwFatalException(kj::mv(exception));
      }
    }
  }
};

}  // namespace

kj::Own<ReadableStreamSource> newMemorySource(
    kj::ArrayPtr<const kj::byte> bytes, kj::Maybe<kj::Own<void>> maybeBacking) {
  KJ_IF_SOME(backing, maybeBacking) {
    return kj::heap<MemoryInputStream>(bytes, kj::mv(backing));
  }
  // No backing provided - make a copy of the bytes.
  auto copy = kj::heapArray<kj::byte>(bytes);
  auto ptr = copy.asPtr();
  return kj::heap<MemoryInputStream>(ptr, kj::heap(kj::mv(copy)));
}

kj::Own<kj::AsyncInputStream> wrapTeeBranch(kj::Own<kj::AsyncInputStream> branch) {
  return TeeErrorAdapter::wrap(kj::mv(branch));
}

}  // namespace workerd::api
