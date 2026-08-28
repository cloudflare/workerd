// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// ReadableByteStreamController and its byobRequest: auto-allocation,
// request lifecycle around enqueue(), and close() interactions with
// partially-filled BYOB reads.

import { strictEqual, ok, throws } from 'node:assert';
import { usingTsImpl } from 'which-impl';
import { rejectionOf } from 'helpers';

// DIVERGENCE (the subject of the streams_no_default_auto_allocate_
// chunk_size flag): with a DEFAULT reader and no autoAllocateChunkSize,
// C++ auto-allocates anyway — pull() sees a byobRequest with a real
// view; TypeScript follows the spec and reports byobRequest null. The
// C++ default allocation is 4096 bytes, or 16384 with the
// UPDATED_AUTO_ALLOCATE_CHUNK_SIZE autogate on (the @all-autogates
// variant), so both exact sizes are pinned.
export const byobRequestOnDefaultRead = {
  async test() {
    let seen = 'not-pulled';
    const rs = new ReadableStream({
      type: 'bytes',
      pull(c) {
        seen =
          c.byobRequest === null
            ? 'null'
            : `view(${c.byobRequest.view.byteLength})`;
        c.enqueue(new Uint8Array([1]));
      },
    });
    strictEqual((await rs.getReader().read()).value[0], 1);
    if (usingTsImpl) {
      strictEqual(seen, 'null');
    } else {
      ok(
        seen === 'view(4096)' || seen === 'view(16384)',
        `unexpected allocation: ${seen}`
      );
    }
  },
};

// enqueue() discards the outstanding (auto-allocated) byobRequest: it is
// null right after the enqueue (pinned per implementation; under
// TypeScript there was no request to begin with — see
// byobRequestOnDefaultRead).
export const enqueueDiscardsByobRequest = {
  async test() {
    let states;
    const rs = new ReadableStream({
      type: 'bytes',
      pull(c) {
        const before = c.byobRequest === null ? 'null' : 'req';
        c.enqueue(new Uint8Array([7]));
        const after = c.byobRequest === null ? 'null' : 'req';
        states = `${before},${after}`;
      },
    });
    strictEqual((await rs.getReader().read()).value[0], 7);
    strictEqual(states, usingTsImpl ? 'null,null' : 'req,null');
  },
};

// DIVERGENCE (the WPT general.any close-with-partial seed): closing with
// a partially-filled pending read(view) must error the stream under the
// spec — TypeScript throws from close() and rejects the read and closed
// with the same TypeError. C++ lets close() succeed, resolves the read
// with an EMPTY view and done=false, and fulfills closed.
export const closeWithPartiallyFilledView = {
  async test() {
    let controller;
    const rs = new ReadableStream({
      type: 'bytes',
      start(c) {
        controller = c;
      },
    });
    const reader = rs.getReader({ mode: 'byob' });
    const readP = reader.read(new Uint16Array(1));
    controller.enqueue(new Uint8Array([1])); // 1 of 2 bytes: partial
    if (usingTsImpl) {
      const expected = {
        name: 'TypeError',
        message: 'Insufficient bytes to fill elements in the given view',
      };
      throws(() => controller.close(), expected);
      const readErr = await rejectionOf(readP);
      strictEqual(readErr.message, expected.message);
      const closedErr = await rejectionOf(reader.closed);
      strictEqual(closedErr.message, expected.message);
    } else {
      controller.close();
      const r = await readP;
      strictEqual(r.done, false);
      ok(r.value instanceof Uint16Array);
      strictEqual(r.value.byteLength, 0);
      strictEqual(await reader.closed, undefined);
    }
  },
};

// read(view) against a closed stream resolves done with an EMPTY view
// over the same-sized buffer (parity under the pinned
// internal_stream_byob_return_view flag).
export const readAfterCloseReturnsEmptyView = {
  async test() {
    let controller;
    const rs = new ReadableStream({
      type: 'bytes',
      start(c) {
        controller = c;
      },
    });
    controller.close();
    const { value, done } = await rs
      .getReader({ mode: 'byob' })
      .read(new Uint8Array(4));
    ok(done);
    ok(value instanceof Uint8Array);
    strictEqual(value.byteLength, 0);
    strictEqual(value.buffer.byteLength, 4);
  },
};

// read(view) transfers the caller's buffer immediately (the pinned
// streams_byob_reader_detaches_buffer behavior) and delivers the bytes
// in a fresh view over the transferred buffer (parity).
export const readDetachesCallerBuffer = {
  async test() {
    let controller;
    const rs = new ReadableStream({
      type: 'bytes',
      start(c) {
        controller = c;
      },
    });
    const reader = rs.getReader({ mode: 'byob' });
    const view = new Uint8Array(4);
    const readP = reader.read(view);
    strictEqual(view.byteLength, 0); // detached at call time
    controller.enqueue(new Uint8Array([1, 2]));
    const { value, done } = await readP;
    strictEqual(done, false);
    strictEqual(value.byteLength, 2);
    strictEqual(value[0], 1);
    strictEqual(value[1], 2);
  },
};

// close() while an UNFILLED BYOB read is pending. DIVERGENCE: C++
// resolves the read done with an empty view; the TypeScript read PENDS
// FOREVER while close() itself succeeds (bounded observation — the
// close-below-min defect family, without any min involved).
export const closeWithPendingUnfilledByobRead = {
  async test() {
    let controller;
    const rs = new ReadableStream({
      type: 'bytes',
      start(c) {
        controller = c;
      },
    });
    const reader = rs.getReader({ mode: 'byob' });
    const readP = reader.read(new Uint8Array(16));
    controller.close();
    const outcome = await Promise.race([
      readP.then(
        (r) => ({ state: 'fulfilled', r }),
        () => ({ state: 'rejected' })
      ),
      scheduler.wait(250).then(() => ({ state: 'pending' })),
    ]);
    if (usingTsImpl) {
      strictEqual(outcome.state, 'pending');
    } else {
      strictEqual(outcome.state, 'fulfilled');
      strictEqual(outcome.r.done, true);
      strictEqual(outcome.r.value.byteLength, 0);
    }
    await reader.closed;
  },
};
