// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Tests for async iterator edge cases on ReadableStream.
// These tests focus on iterator protocol methods (return, throw),
// preventCancel option, and iteration over closed/errored streams.
//
// Test inspirations:
// - Bun: test/js/web/streams/streams.test.js (Symbol.asyncIterator tests)
// - Deno: tests/unit/streams_test.ts (async iterator tests)

import { strictEqual, ok, rejects, deepStrictEqual, throws } from 'node:assert';

// Test that breaking out of for-await-of cancels the stream
// Inspired by: Bun test/js/web/streams/streams.test.js
export const asyncIteratorBreakCancels = {
  async test() {
    let cancelled = false;
    let cancelReason = null;

    const rs = new ReadableStream({
      pull(controller) {
        controller.enqueue('chunk');
      },
      cancel(reason) {
        cancelled = true;
        cancelReason = reason;
      },
    });

    const values = [];
    for await (const chunk of rs) {
      values.push(chunk);
      if (values.length === 3) {
        break;
      }
    }

    strictEqual(values.length, 3);
    ok(cancelled, 'stream should be cancelled after break');
    strictEqual(cancelReason, undefined);
  },
};

// Test calling return() explicitly on async iterator
// Inspired by: Deno tests/unit/streams_test.ts
export const asyncIteratorReturnMethod = {
  async test() {
    let cancelled = false;

    const rs = new ReadableStream({
      pull(controller) {
        controller.enqueue('chunk');
      },
      cancel() {
        cancelled = true;
      },
    });

    const iterator = rs[Symbol.asyncIterator]();

    const first = await iterator.next();
    strictEqual(first.value, 'chunk');
    strictEqual(first.done, false);

    const returnResult = await iterator.return('finished');
    strictEqual(returnResult.done, true);

    ok(cancelled, 'stream should be cancelled after return()');
  },
};

// Test that return() followed by next() returns done
// Inspired by: Deno tests/unit/streams_test.ts
export const asyncIteratorReturnThenNext = {
  async test() {
    const rs = new ReadableStream({
      pull(controller) {
        controller.enqueue('chunk');
      },
    });

    const iterator = rs[Symbol.asyncIterator]();

    await iterator.next();
    await iterator.return();

    const result = await iterator.next();
    strictEqual(result.done, true);
    strictEqual(result.value, undefined);
  },
};

// Test values() with preventCancel: true
// Inspired by: Bun test/js/web/streams/streams.test.js
export const asyncIteratorPreventCancel = {
  async test() {
    let cancelled = false;

    const rs = new ReadableStream({
      pull(controller) {
        controller.enqueue('chunk');
      },
      cancel() {
        cancelled = true;
      },
    });

    const values = [];
    for await (const chunk of rs.values({ preventCancel: true })) {
      values.push(chunk);
      if (values.length === 3) {
        break;
      }
    }

    strictEqual(values.length, 3);
    ok(!cancelled, 'stream should NOT be cancelled with preventCancel: true');
    ok(!rs.locked, 'stream should be unlocked');

    const reader = rs.getReader();
    const { value } = await reader.read();
    strictEqual(value, 'chunk');
    reader.releaseLock();
  },
};

// Test iterating over an already-closed stream
// Inspired by: Deno tests/unit/streams_test.ts
export const asyncIteratorOnClosedStream = {
  async test() {
    const rs = new ReadableStream({
      start(controller) {
        controller.enqueue('only-chunk');
        controller.close();
      },
    });

    const values = [];
    for await (const chunk of rs) {
      values.push(chunk);
    }

    deepStrictEqual(values, ['only-chunk']);
  },
};

// Test iterating over an already-errored stream
// Inspired by: Bun test/js/web/streams/streams.test.js
export const asyncIteratorOnErroredStream = {
  async test() {
    const rs = new ReadableStream({
      start(controller) {
        controller.error(new Error('Stream error'));
      },
    });

    const values = [];
    const iterate = async () => {
      for await (const chunk of rs) {
        values.push(chunk);
      }
    };

    await rejects(iterate, { message: 'Stream error' });
    strictEqual(values.length, 0);
  },
};

// Test that getting an async iterator locks the stream
// Inspired by: Bun test/js/web/streams/streams.test.js
export const asyncIteratorLocksStream = {
  async test() {
    const rs = new ReadableStream({
      pull(controller) {
        controller.enqueue('chunk');
      },
    });

    ok(!rs.locked, 'stream should not be locked initially');

    const iterator = rs[Symbol.asyncIterator]();

    ok(rs.locked, 'stream should be locked after getting iterator');

    throws(() => rs[Symbol.asyncIterator](), TypeError);

    await iterator.return();
  },
};
