// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

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
// Fulfilled when an await_io_and_run() callback runs, if set.
extern kj::Maybe<kj::Own<kj::PromiseFulfiller<void>>> onAwaitCallback;

}  // namespace workerd::io_context::test
