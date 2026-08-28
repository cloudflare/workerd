// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// controller.terminate(): closes the readable, errors the writable, and
// interacts with queued chunks and late error() calls. Complements WPT
// transform-streams/terminate.any.js.

import { strictEqual, ok, rejects } from 'node:assert';
import { usingTsImpl } from 'which-impl';

// terminate(): the readable reads done; the writable rejects writes and
// closed with the same TypeError message in both implementations.
export const terminateClosesReadableErrorsWritable = {
  async test() {
    let ctrl;
    const ts = new TransformStream({
      start(c) {
        ctrl = c;
      },
    });
    const reader = ts.readable.getReader();
    const writer = ts.writable.getWriter();
    ctrl.terminate();

    const r = await reader.read();
    ok(r.done);

    const expected = {
      name: 'TypeError',
      message: 'The transform stream has been terminated',
    };
    await rejects(writer.write('x'), expected);
    await rejects(writer.closed, expected);
  },
};

// DIVERGENCE (the WPT terminate.any expectedFailure): controller.error()
// AFTER terminate() with a chunk still queued. The spec (TypeScript)
// lets the late error win: reads reject. C++ ignores the error after
// terminate: the queued chunk drains and the stream reads done.
export const errorAfterTerminateWithQueuedChunk = {
  async test() {
    let ctrl;
    const ts = new TransformStream(
      {
        start(c) {
          ctrl = c;
        },
      },
      undefined,
      { highWaterMark: 2 }
    );
    ctrl.enqueue('queued');
    ctrl.terminate();
    ctrl.error(new Error('late-error'));

    const reader = ts.readable.getReader();
    if (usingTsImpl) {
      await rejects(reader.read(), { message: 'late-error' });
      await rejects(reader.read(), { message: 'late-error' });
    } else {
      const r1 = await reader.read();
      strictEqual(r1.done, false);
      strictEqual(r1.value, 'queued');
      const r2 = await reader.read();
      strictEqual(r2.done, true);
    }
  },
};
