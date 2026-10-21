// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The stream machinery keeps a pending write alive even when the user
// drops every reachable reference between turns; gc() must not sever the
// controller/writer/stream linkage. Requires --expose-gc (set in both
// cell configs). Migrated from streams-js-test.js.

import { strictEqual } from 'node:assert';

async function collectGarbage() {
  for (let i = 0; i < 3; i++) {
    gc();
    await scheduler.wait(5);
  }
}

export const writableStreamGc = {
  async test() {
    let controller;
    let writer;
    let write;

    {
      const ws = new WritableStream({
        start(c) {
          controller = c;
        },
      });
      writer = ws.getWriter();
    }

    await scheduler.wait(10);
    gc();

    {
      write = writer.write(1);
      writer = undefined;
    }

    await scheduler.wait(10);
    gc();

    {
      await write;
      strictEqual(controller.signal.aborted, false);
    }
  },
};

// A bare WritableStream survives a gc() sweep without crashing the GC
// tracing path (migrated from streams-test.js).
export const writableStreamGcTraceFinishes = {
  test() {
    const _ws = new WritableStream();
    gc();
  },
};

// Made outside the sink: an error created inside a sink method holds the
// sink through its stack trace (the frame's receiver), on both
// implementations.
const kSinkError = new Error('sink');

// A stream that has closed, been aborted or errored (by its controller, or
// by a rejected write) releases its underlying sink (spec ClearAlgorithms
// drops the algorithms, its only path to the sink), while the stream and
// its controller stay reachable; a stream still open keeps it (parity).
// Errored by its controller while start() is still pending, it releases
// the sink at once, without waiting for start() to settle.
export const sinkReleasedOnceEnded = {
  async test() {
    const desiredSizes = {
      open: 1,
      close: 0,
      abort: null,
      error: null,
      'error-while-starting': null,
      'write-rejects': null,
    };
    const ends = [
      'open',
      'close',
      'abort',
      'error',
      'error-while-starting',
      'write-rejects',
    ];
    for (const end of ends) {
      let controller;
      let ref;
      let resolveStart;
      const stream = (() => {
        const sink = {
          start(c) {
            controller = c;
            return end === 'error-while-starting'
              ? new Promise((resolve) => {
                  resolveStart = resolve;
                })
              : undefined;
          },
          write() {
            throw kSinkError;
          },
          close() {},
          abort() {},
        };
        ref = new WeakRef(sink);
        return new WritableStream(sink);
      })();
      await scheduler.wait(1);
      if (end === 'close') await stream.close();
      if (end === 'abort') await stream.abort();
      if (end === 'error' || end === 'error-while-starting') {
        controller.error(new Error('boom'));
      }
      if (end === 'write-rejects') {
        const writer = stream.getWriter();
        strictEqual(await writer.write('chunk').catch((e) => e), kSinkError);
        writer.releaseLock();
      }
      await collectGarbage();
      strictEqual(ref.deref() !== undefined, end === 'open', `${end}: sink`);
      resolveStart?.();
      await scheduler.wait(1);
      strictEqual(
        stream.getWriter().desiredSize,
        desiredSizes[end],
        `${end}: desiredSize`
      );
    }
  },
};
