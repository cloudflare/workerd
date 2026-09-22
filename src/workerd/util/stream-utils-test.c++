// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "abortable.h"
#include "stream-utils.h"

#include <kj/async-io.h>
#include <kj/test.h>

namespace workerd {
namespace {

KJ_TEST("MemoryInputStream tryReadSync") {
  auto stream = newMemoryInputStream("foobar"_kj);

  kj::byte buf[4]{};
  KJ_EXPECT(KJ_ASSERT_NONNULL(stream->tryReadSync(kj::arrayPtr(buf), 1)) == 4);
  KJ_EXPECT(kj::arrayPtr(buf) == "foob"_kjb);
  KJ_EXPECT(KJ_ASSERT_NONNULL(stream->tryReadSync(kj::arrayPtr(buf), 1)) == 2);
  KJ_EXPECT(kj::arrayPtr(buf).first(2) == "ar"_kjb);

  // EOF is a valid synchronous answer.
  KJ_EXPECT(KJ_ASSERT_NONNULL(stream->tryReadSync(kj::arrayPtr(buf), 1)) == 0);
}

KJ_TEST("NeuterableInputStream tryReadSync") {
  kj::EventLoop loop;
  kj::WaitScope ws(loop);

  auto pipe = kj::newOneWayPipe();
  auto neuterable = newNeuterableInputStream(*pipe.in);

  kj::byte buf[8]{};

  // Nothing is available on the pipe.
  KJ_EXPECT(neuterable->tryReadSync(kj::arrayPtr(buf), 1) == kj::none);

  // A blocked write is served synchronously through the wrapper.
  auto writePromise = pipe.out->write("foo"_kjb);
  KJ_EXPECT(KJ_ASSERT_NONNULL(neuterable->tryReadSync(kj::arrayPtr(buf), 1)) == 3);
  KJ_EXPECT(kj::arrayPtr(buf).first(3) == "foo"_kjb);
  writePromise.wait(ws);

  // After neutering, synchronous reads decline; the async path surfaces the exception.
  neuterable->neuter(KJ_EXCEPTION(DISCONNECTED, "test neuter"));
  KJ_EXPECT(neuterable->tryReadSync(kj::arrayPtr(buf), 1) == kj::none);
}

KJ_TEST("NeuterableIoStream tryReadSync/tryWriteSync") {
  kj::EventLoop loop;
  kj::WaitScope ws(loop);

  auto pipe = kj::newTwoWayPipe();
  auto neuterable = newNeuterableIoStream(*pipe.ends[0]);

  // Write side: no reader waiting means no synchronous write.
  KJ_EXPECT(!neuterable->tryWriteSync("foo"_kjb));

  kj::byte buf[3]{};
  {
    auto readPromise = pipe.ends[1]->tryRead(buf, 3, 3);
    KJ_EXPECT(neuterable->tryWriteSync("foo"_kjb));
    KJ_EXPECT(readPromise.wait(ws) == 3);
    KJ_EXPECT(kj::arrayPtr(buf) == "foo"_kjb);
  }

  // Gather write.
  {
    auto readPromise = pipe.ends[1]->tryRead(buf, 3, 3);
    kj::ArrayPtr<const kj::byte> pieces[] = {"ba"_kjb, "r"_kjb};
    KJ_EXPECT(neuterable->tryWriteSync(kj::arrayPtr(pieces, 2)));
    KJ_EXPECT(readPromise.wait(ws) == 3);
    KJ_EXPECT(kj::arrayPtr(buf) == "bar"_kjb);
  }

  // Read side.
  kj::byte buf2[8]{};
  KJ_EXPECT(neuterable->tryReadSync(kj::arrayPtr(buf2), 1) == kj::none);
  auto writePromise = pipe.ends[1]->write("baz"_kjb);
  KJ_EXPECT(KJ_ASSERT_NONNULL(neuterable->tryReadSync(kj::arrayPtr(buf2), 1)) == 3);
  KJ_EXPECT(kj::arrayPtr(buf2).first(3) == "baz"_kjb);
  writePromise.wait(ws);

  // After neutering, both sides decline; the async paths surface the exception.
  neuterable->neuter(KJ_EXCEPTION(DISCONNECTED, "test neuter"));
  KJ_EXPECT(neuterable->tryReadSync(kj::arrayPtr(buf2), 1) == kj::none);
  KJ_EXPECT(!neuterable->tryWriteSync("qux"_kjb));
}

KJ_TEST("AbortableInputStream tryReadSync") {
  kj::EventLoop loop;
  kj::WaitScope ws(loop);

  auto pipe = kj::newOneWayPipe();
  auto canceler = kj::heap<ReleasingCanceler>();
  // The stream takes ownership of the canceler; it remains valid for cancel() below
  // because `stream` outlives this scope's use of it.
  auto& cancelerRef = *canceler;
  auto stream = kj::refcounted<AbortableInputStream>(kj::mv(pipe.in), kj::mv(canceler));

  kj::byte buf[8]{};

  // Nothing is available on the pipe.
  KJ_EXPECT(stream->tryReadSync(kj::arrayPtr(buf), 1) == kj::none);

  // A blocked write is served synchronously through the wrapper.
  auto writePromise = pipe.out->write("foo"_kjb);
  KJ_EXPECT(KJ_ASSERT_NONNULL(stream->tryReadSync(kj::arrayPtr(buf), 1)) == 3);
  KJ_EXPECT(kj::arrayPtr(buf).first(3) == "foo"_kjb);
  writePromise.wait(ws);

  // After cancellation, synchronous reads decline; the async path surfaces the exception.
  cancelerRef.cancel(KJ_EXCEPTION(DISCONNECTED, "aborted"));
  KJ_EXPECT(stream->tryReadSync(kj::arrayPtr(buf), 1) == kj::none);
}

}  // namespace
}  // namespace workerd
