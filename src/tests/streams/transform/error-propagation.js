// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Transformer hook errors: how sync throws and async rejections from
// start/transform/flush fan out across pending writes, close, and the
// readable side, plus controller.error() surfacing on the reader.
// Migrated from transform-streams-test.js and
// streams-error-edge-cases-test.js.

import { strictEqual, ok, throws, rejects } from 'node:assert';
import { usingTsImpl } from 'which-impl';
import { consume } from 'helpers';

// DIVERGENCE (same family as the writable suite's ledger #5): a sync
// start() throw escapes `new TransformStream()` under TypeScript (spec;
// the WPT errors.any "constructor should throw when start does" case)
// but is captured by C++, which errors both sides so writes and close
// reject with the thrown error.
export const syncErrorDuringStart = {
  async test() {
    if (usingTsImpl) {
      throws(
        () =>
          new TransformStream({
            start() {
              throw new Error('boom');
            },
          }),
        { message: 'boom' }
      );
      return;
    }

    const transform = new TransformStream({
      start() {
        throw new Error('boom');
      },
    });
    ok(!(transform instanceof IdentityTransformStream));

    const { readable, writable } = transform;

    const writer = writable.getWriter();

    const res = await Promise.allSettled([
      writer.write('hello'),
      writer.write('there'),
      writer.close(),
      consume(readable),
    ]);

    strictEqual(res[0].status, 'rejected');
    strictEqual(res[1].status, 'rejected');
    strictEqual(res[2].status, 'rejected');
    strictEqual(res[0].reason.message, 'boom');
    strictEqual(res[1].reason.message, 'boom');
    strictEqual(res[2].reason.message, 'boom');
  },
};

// An async start() rejection dooms writes, close, AND the readable.
export const asyncErrorDuringStart = {
  async test() {
    const transform = new TransformStream({
      async start() {
        throw new Error('boom');
      },
    });
    ok(!(transform instanceof IdentityTransformStream));

    const { readable, writable } = transform;

    const writer = writable.getWriter();

    const res = await Promise.allSettled([
      writer.write('hello'),
      writer.write('there'),
      writer.close(),
      consume(readable),
    ]);

    strictEqual(res[0].status, 'rejected');
    strictEqual(res[1].status, 'rejected');
    strictEqual(res[2].status, 'rejected');
    strictEqual(res[3].status, 'rejected');
    strictEqual(res[0].reason.message, 'boom');
    strictEqual(res[1].reason.message, 'boom');
    strictEqual(res[2].reason.message, 'boom');
    strictEqual(res[3].reason.message, 'boom');
  },
};

// A sync transform() throw dooms everything including the readable.
export const syncErrorDuringTransform = {
  async test() {
    const transform = new TransformStream({
      transform() {
        throw new Error('boom');
      },
    });
    ok(!(transform instanceof IdentityTransformStream));

    const { readable, writable } = transform;

    const writer = writable.getWriter();

    const res = await Promise.allSettled([
      writer.write('hello'),
      writer.write('there'),
      writer.close(),
      consume(readable),
    ]);

    strictEqual(res[0].status, 'rejected');
    strictEqual(res[1].status, 'rejected');
    strictEqual(res[2].status, 'rejected');
    strictEqual(res[3].status, 'rejected');
    strictEqual(res[0].reason.message, 'boom');
    strictEqual(res[1].reason.message, 'boom');
    strictEqual(res[2].reason.message, 'boom');
    strictEqual(res[3].reason.message, 'boom');
  },
};

// An async transform() rejection dooms everything.
export const asyncErrorDuringTransform = {
  async test() {
    const transform = new TransformStream({
      async transform() {
        await scheduler.wait(1);
        throw new Error('boom');
      },
    });
    ok(!(transform instanceof IdentityTransformStream));

    const { readable, writable } = transform;

    const writer = writable.getWriter();

    const res = await Promise.allSettled([
      writer.write('hello'),
      writer.write('there'),
      writer.close(),
      consume(readable),
    ]);

    strictEqual(res[0].status, 'rejected');
    strictEqual(res[1].status, 'rejected');
    strictEqual(res[2].status, 'rejected');
    strictEqual(res[3].status, 'rejected');
    strictEqual(res[0].reason.message, 'boom');
    strictEqual(res[1].reason.message, 'boom');
    strictEqual(res[2].reason.message, 'boom');
    strictEqual(res[3].reason.message, 'boom');
  },
};

// A sync flush() throw: the writes succeed, close and the readable fail.
export const syncErrorDuringFlush = {
  async test() {
    const transform = new TransformStream({
      flush() {
        throw new Error('boom');
      },
    });
    ok(!(transform instanceof IdentityTransformStream));

    const { readable, writable } = transform;

    const writer = writable.getWriter();

    const res = await Promise.allSettled([
      writer.write('hello'),
      writer.write('there'),
      writer.close(),
      consume(readable),
    ]);

    // The two writes will succeed.
    strictEqual(res[0].status, 'fulfilled');
    strictEqual(res[1].status, 'fulfilled');

    // The close and the consume will reject.
    strictEqual(res[2].status, 'rejected');
    strictEqual(res[3].status, 'rejected');
    strictEqual(res[2].reason.message, 'boom');
    strictEqual(res[3].reason.message, 'boom');
  },
};

// An async flush() rejection: same fan-out as the sync case.
export const asyncErrorDuringFlush = {
  async test() {
    const transform = new TransformStream({
      async flush() {
        await scheduler.wait(1);
        throw new Error('boom');
      },
    });
    ok(!(transform instanceof IdentityTransformStream));

    const { readable, writable } = transform;

    const writer = writable.getWriter();

    const res = await Promise.allSettled([
      writer.write('hello'),
      writer.write('there'),
      writer.close(),
      consume(readable),
    ]);

    // The two writes will succeed.
    strictEqual(res[0].status, 'fulfilled');
    strictEqual(res[1].status, 'fulfilled');

    // The close and the consume will reject.
    strictEqual(res[2].status, 'rejected');
    strictEqual(res[3].status, 'rejected');
    strictEqual(res[2].reason.message, 'boom');
    strictEqual(res[3].reason.message, 'boom');
  },
};

// controller.error() from outside any hook rejects pending reads.
// Migrated from streams-error-edge-cases-test.js.
export const errorInTransformFlush = {
  async test() {
    let transformController;

    const ts = new TransformStream({
      start(controller) {
        transformController = controller;
      },
      transform(chunk, controller) {
        controller.enqueue(chunk);
      },
    });

    const reader = ts.readable.getReader();

    transformController.error(new Error('Transform error'));

    await rejects(
      async () => {
        await reader.read();
      },
      { message: 'Transform error' }
    );
  },
};
