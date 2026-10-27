// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "ffi.h"

#include <workerd/io/io-context/ffi.rs.h>

#include <capnp/message.h>
#include <capnp/serialize.h>

#include <bit>
#include <type_traits>

namespace workerd::io_context {

namespace {

// IoContext::Id holds only the process-unique uint64_t that Rust stores.
static_assert(sizeof(IoContext::Id) == sizeof(uint64_t));
static_assert(std::is_trivially_copyable_v<IoContext::Id>);

// The current IoContext, which must be the one `id` identifies.
IoContext& currentWithId(uint64_t id) {
  auto& context = KJ_REQUIRE_NONNULL(IoContext::tryCurrent(), "there is no current request");
  KJ_REQUIRE(io_context_id(context) == id, "the IoContext is not the one of the current request");
  return context;
}

}  // namespace

kj::Maybe<IoContext&> io_context_try_current() {
  return IoContext::tryCurrent();
}

uint64_t io_context_id(const IoContext& context) {
  return std::bit_cast<uint64_t>(context.getId());
}

int64_t io_context_now(uint64_t id) {
  return (currentWithId(id).now() - kj::UNIX_EPOCH) / kj::NANOSECONDS;
}

void io_context_write_logfwdr(uint64_t id, uint32_t channel, ::rust::Slice<const uint8_t> message) {
  auto& context = currentWithId(id);

  // Rust does not align the bytes to words, so they are copied.
  auto bytes = kj::arrayPtr(message.data(), message.size());
  KJ_REQUIRE(bytes.size() > 0, "the serialized message is empty");
  KJ_REQUIRE(bytes.size() % sizeof(capnp::word) == 0,
      "the serialized message is not a whole number of words");
  auto words = kj::heapArray<capnp::word>(bytes.size() / sizeof(capnp::word));
  words.asBytes().copyFrom(bytes);
  capnp::FlatArrayMessageReader reader(words);
  KJ_REQUIRE(reader.getEnd() == words.end(), "bytes follow the serialized message");

  KJ_IF_SOME(outputLocks, context.waitForOutputLocksIfNecessary()) {
    // In a Durable Object, the message must not leave before the storage writes that precede it
    // are confirmed, as in AnalyticsEngine::writeDataPoint(). The message is copied now, so that
    // an invalid one fails this call rather than the write.
    auto copy = kj::heap<capnp::MallocMessageBuilder>();
    copy->getRoot<capnp::AnyPointer>().set(reader.getRoot<capnp::AnyPointer>());
    context.addWaitUntil(
        outputLocks.then([weakRef = context.getWeakRef(), channel, copy = kj::mv(copy)]() mutable {
      // The IoContext owns its waitUntil tasks, so it is alive while one runs.
      auto& context = KJ_ASSERT_NONNULL(weakRef->tryGet());
      // IoChannelFactory::writeLogfwdr() needs the current IoContext, so the write runs in it.
      return context.run([channel, copy = kj::mv(copy)](Worker::Lock&, IoContext& context) mutable {
        context.writeLogfwdr(channel, [&](capnp::AnyPointer::Builder builder) {
          builder.set(copy->getRoot<capnp::AnyPointer>().asReader());
        });
      });
    }));
  } else {
    context.writeLogfwdr(channel, [&](capnp::AnyPointer::Builder builder) {
      builder.set(reader.getRoot<capnp::AnyPointer>());
    });
  }
}

void io_context_await_io(
    uint64_t id, ::rust::Box<IoFuture> future, ::rust::Box<IoCallback> callback) {
  kj::Promise<::rust::Box<IoOutput>> output = io_future_wait(kj::mv(future));
  currentWithId(id).awaitIoAndRun(kj::mv(output),
      [callback = kj::mv(callback)](
          Worker::Lock& lock, IoContext::ExceptionOr<::rust::Box<IoOutput>>&& result) mutable {
    // io_future_wait() produces a value and does not fail.
    auto output = kj::mv(KJ_REQUIRE_NONNULL(result.tryGet<::rust::Box<IoOutput>>(),
        "the future that IoContext::await_io() waited for failed"));
    io_callback_run(
        reinterpret_cast<uintptr_t>(lock.getIsolate()), kj::mv(callback), kj::mv(output));
  });
}

kj::Own<IoOwnedObject> io_context_add_object(uint64_t id, ::rust::Box<IoOwnedSlot> slot) {
  auto& context = currentWithId(id);
  auto object = context.addObject(kj::heap<::rust::Box<IoOwnedSlot>>(kj::mv(slot)));
  return kj::heap<IoOwnedObject>(kj::heap(kj::mv(object)));
}

}  // namespace workerd::io_context
