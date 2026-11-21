// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/io/io-channels.h>
#include <workerd/jsg/async-context.h>

#include <kj/async.h>
#include <kj/string.h>

// What the functions of test-bridge.h record, for io-context-test.c++ to check.
namespace workerd::io_context::test {

// The value of TEST_KEY in the frame record_async_context() last saw, or "none".
extern kj::String recordedAsyncContext;
extern kj::Arc<jsg::AsyncContextFrame::StorageKey> TEST_KEY;

// The IoContext the last await_io_and_run() callback ran in, if any.
extern kj::Maybe<uint64_t> awaitContextId;
// Once the data of register_reaction() has been dropped, the IoContext that was current then, if
// any.
extern kj::Maybe<kj::Maybe<uint64_t>> reactionDataDropContextId;
// Fulfilled when an await_io_and_run() callback runs, if set.
extern kj::Maybe<kj::Own<kj::PromiseFulfiller<void>>> onAwaitCallback;

// The channel keep_channel() last kept, if any.
extern kj::Maybe<kj::Rc<IoChannelFactory::SubrequestChannel>> keptChannel;

}  // namespace workerd::io_context::test
