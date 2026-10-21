// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { deepStrictEqual, rejects, strictEqual, throws } from 'node:assert';

async function reenter(env, options) {
  const query = new URLSearchParams(options);
  const origin = await env.REENTRY.fetch(`https://reentry/origin?${query}`);
  strictEqual(await origin.text(), 'origin complete');
  const response = await env.REENTRY.fetch(`https://reentry/settle?${query}`);
  return response.json();
}

export const cancellationOwnsPendingReads = {
  async test(_controller, env) {
    for (const kind of ['values', 'bytes', 'byob']) {
      const actions =
        kind === 'byob'
          ? ['retain', 'release', 'respond']
          : ['retain', 'release'];
      for (const action of actions) {
        deepStrictEqual(
          await reenter(env, { operation: 'cancel', kind, action }),
          {
            entries: 1,
            nestedErrors: [],
            outerError: null,
            current: { status: 'fulfilled', done: true },
          }
        );
      }
    }
  },
};

export const readerReleasePreservesReplacementPipe = {
  async test(_controller, env) {
    for (const kind of ['values', 'bytes', 'byob']) {
      for (const action of ['retain', 'release']) {
        deepStrictEqual(
          await reenter(env, { operation: 'release', kind, action }),
          {
            entries: 1,
            nestedErrors: [],
            outerError: null,
            lockedDuringPipe: true,
            lockedAfterPipe: false,
            values: kind === 'values' ? ['healthy'] : [[1, 2, 3]],
          }
        );
      }
    }
  },
};

export const writerReleasePreservesReplacementRequests = {
  async test(_controller, env) {
    for (const action of ['retain', 'nested', 'replace']) {
      const expected = {
        entries: 1,
        nestedErrors: [],
        outerError: null,
        locked: action === 'replace',
        beforeCleanup: action === 'replace' ? 'Promise { <pending> }' : null,
      };
      if (action === 'replace') expected.afterCleanup = 'rejected';
      deepStrictEqual(
        await reenter(env, { operation: 'writer', action }),
        expected
      );
    }
  },
};

export const byobResponseSurvivesSiblingSettlement = {
  async test(_controller, env) {
    for (const method of ['respond', 'new-view']) {
      for (const action of ['retain', 'release', 'cancel', 'close', 'error']) {
        deepStrictEqual(
          await reenter(env, {
            operation: 'respond',
            kind: 'byob',
            method,
            action,
          }),
          { entries: 1, nestedErrors: [], outerError: null }
        );
      }
    }
  },
};

export const partialByobResponseRejectsNestedProgress = {
  async test(_controller, env) {
    for (const method of ['respond', 'new-view']) {
      for (const completion of ['manual', 'pull']) {
        deepStrictEqual(
          await reenter(env, {
            operation: 'partial-respond',
            method,
            completion,
          }),
          {
            entries: 1,
            nestedError: 'TypeError',
            nestedViewLength: 1,
            progress: { offset: 1, length: 7 },
            values: [
              [17, 34],
              [17, 34],
            ],
            done: [false, false],
            closed: [true, true],
          }
        );
      }
    }
  },
};

export const readerQueueRemainsReusable = {
  async test() {
    for (const kind of ['values', 'bytes', 'byob']) {
      for (const pending of [false, true]) {
        let controller;
        const source = {
          start(value) {
            controller = value;
          },
        };
        if (kind !== 'values') source.type = 'bytes';
        const stream = new ReadableStream(source);
        const options = kind === 'byob' ? { mode: 'byob' } : undefined;
        const read = (reader) =>
          kind === 'byob' ? reader.read(new Uint8Array(8)) : reader.read();
        const original = stream.getReader(options);
        original.closed.catch(() => {});
        const rejected = pending
          ? rejects(read(original), TypeError)
          : undefined;
        original.releaseLock();
        strictEqual(stream.locked, false);
        const replacement = stream.getReader(options);
        const result = read(replacement);
        controller.enqueue(
          kind === 'values' ? 'healthy' : new Uint8Array([1, 2, 3])
        );
        const { value, done } = await result;
        strictEqual(done, false);
        deepStrictEqual(
          kind === 'values' ? value : Array.from(value),
          kind === 'values' ? 'healthy' : [1, 2, 3]
        );
        await rejected;
        await replacement.cancel();
      }
    }
  },
};

export const typedPartialResponsePreservesTail = {
  async test() {
    let controller;
    const stream = new ReadableStream({
      type: 'bytes',
      start(value) {
        controller = value;
      },
    });
    const reader = stream.getReader({ mode: 'byob' });
    const pending = reader.read(new Uint16Array(new ArrayBuffer(16), 4, 4));
    controller.byobRequest.view[0] = 17;
    controller.byobRequest.respond(1);
    controller.byobRequest.view.set([34, 51, 68, 85]);
    controller.byobRequest.respond(4);
    const first = await pending;
    strictEqual(first.done, false);
    deepStrictEqual(
      Array.from(
        new Uint8Array(
          first.value.buffer,
          first.value.byteOffset,
          first.value.byteLength
        )
      ),
      [17, 34, 51, 68]
    );
    deepStrictEqual(
      Array.from((await reader.read(new Uint8Array(8))).value),
      [85]
    );
    controller.close();
    strictEqual((await reader.read(new Uint8Array(8))).done, true);
  },
};

export const nativeReaderReleasePrecondition = {
  async test() {
    const stream = new FixedLengthStream(1);
    const reader = stream.readable.getReader();
    const writer = stream.writable.getWriter();
    const pending = reader.read();
    throws(() => reader.releaseLock(), TypeError);
    strictEqual(stream.readable.locked, true);
    await writer.write(new Uint8Array([42]));
    const result = await pending;
    strictEqual(result.done, false);
    deepStrictEqual(Array.from(result.value), [42]);
    reader.releaseLock();
    strictEqual(stream.readable.locked, false);
    await writer.close();
  },
};

export const writerQueueRemainsReusable = {
  async test() {
    for (const pending of [false, true]) {
      let unblock, began;
      const started = new Promise((resolve) => {
        began = resolve;
      });
      const seen = [];
      const stream = new WritableStream({
        write(value) {
          seen.push(value);
          if (value === 'first')
            return new Promise((resolve) => {
              unblock = resolve;
              began();
            });
        },
      });
      const original = stream.getWriter();
      await original.ready;
      const closed = rejects(original.closed, TypeError);
      let first, canceled, ready;
      if (pending) {
        first = original.write('first');
        canceled = rejects(original.write('canceled'), TypeError);
        ready = rejects(original.ready, TypeError);
        await started;
      }
      original.releaseLock();
      original.releaseLock();
      strictEqual(stream.locked, false);
      await rejects(original.ready, TypeError);
      await closed;
      await ready;
      await canceled;
      const replacement = stream.getWriter();
      const completed = replacement.write('healthy');
      if (unblock) unblock();
      await first;
      await completed;
      await replacement.ready;
      strictEqual(replacement.desiredSize, 1);
      deepStrictEqual(seen, pending ? ['first', 'healthy'] : ['healthy']);
      await replacement.close();
      await replacement.closed;
    }
  },
};

// Attached locks remain reachable until isolate teardown, where no JS lock is available.
const retained = [];
export const attachedLockShutdown = {
  test() {
    const native = new FixedLengthStream(1);
    for (const stream of [new ReadableStream(), native.readable]) {
      const reader = stream.getReader();
      reader.closed.catch(() => {});
      strictEqual(stream.locked, true);
      retained.push(stream, reader);
    }
    for (const stream of [new WritableStream(), native.writable]) {
      const writer = stream.getWriter();
      writer.closed.catch(() => {});
      writer.ready.catch(() => {});
      strictEqual(stream.locked, true);
      retained.push(stream, writer);
    }
    gc();
  },
};
