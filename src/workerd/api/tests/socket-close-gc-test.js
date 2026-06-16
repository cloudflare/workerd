// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// Regression test for AUTOVULN-EW-EDGEWORKER-17:
// Socket::close() bare-this UAF in jsg::Promise .then()/.catch_() continuations.
//
// Strategy: write a large buffer to the socket WITHOUT awaiting, then immediately
// call close(). The WritableStream's internal queue still holds the data, so the
// flush() inside close() must wait for it to drain to the underlying stream. While
// flush pends across event loop turns, we drop the Socket reference and force GC.
// When flush eventually resolves, the .then() continuations run — pre-fix, they
// dereference freed memory.
// Note that this test is not deterministic and may pass even without the fix.

import { ok } from 'assert';

let connectHandlerCalled = false;

export default {
  async connect(socket) {
    connectHandlerCalled = true;
  },
};

export const socketCloseGcRegression = {
  async test(ctrl, env) {
    let socket = env.SELF.connect('localhost:1');
    await socket.opened;
    ok(connectHandlerCalled, 'connect handler must have been called');

    // Queue a large write WITHOUT awaiting — data sits in the WritableStream's
    // JS-side queue. This ensures close()'s internal flush() actually pends.
    const writer = socket.writable.getWriter();
    writer.write(new Uint8Array(1 << 20)); // 1 MiB, fire-and-forget
    writer.releaseLock();

    // close() starts the four-continuation .then() chain. flush() cannot resolve
    // instantly because the write queue is still draining.
    const closePromise = socket.close();
    socket = null;

    // Force GC while flush is pending. Without JSG_THIS in the lambda captures,
    // the Socket wrapper is invisible to V8's GC tracer and gets collected.
    gc();
    await scheduler.wait(1);
    gc();
    await scheduler.wait(1);
    gc();

    // When flush completes, the .then() continuations fire. Pre-fix: UAF.
    await closePromise;
  },
};
