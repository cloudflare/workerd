// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Transformer hook invocation: start/transform/flush ordering, sync and
// async hooks, and chunk-type freedom. Migrated from
// transform-streams-test.js.

import { strictEqual, ok } from 'node:assert';
import { consume, consumeBytes } from 'helpers';

// start/transform/flush enqueues arrive in order.
export const simpleTransform = {
  async test() {
    const transform = new TransformStream({
      start(controller) {
        controller.enqueue('<');
      },
      transform(value, controller) {
        controller.enqueue(value.toUpperCase());
      },
      flush(controller) {
        controller.enqueue('>');
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

    strictEqual(res[3].value, '<HELLOTHERE>');
  },
};

// Async hooks are awaited; output order is unchanged.
export const delayTransform = {
  async test() {
    const transform = new TransformStream({
      async start(controller) {
        await scheduler.wait(1);
        controller.enqueue('<');
      },
      async transform(value, controller) {
        await scheduler.wait(1);
        controller.enqueue(value.toUpperCase());
      },
      async flush(controller) {
        await scheduler.wait(1);
        controller.enqueue('>');
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

    strictEqual(res[3].value, '<HELLOTHERE>');
  },
};

// The writable and readable sides may carry different chunk types.
export const differentTypesTransform = {
  async test() {
    const enc = new TextEncoder();

    const transform = new TransformStream({
      async start(controller) {
        await scheduler.wait(1);
        controller.enqueue(enc.encode('<'));
      },
      async transform(value, controller) {
        await scheduler.wait(1);
        controller.enqueue(enc.encode(value.toUpperCase()));
      },
      async flush(controller) {
        await scheduler.wait(1);
        controller.enqueue(enc.encode('>'));
      },
    });
    ok(!(transform instanceof IdentityTransformStream));

    const { readable, writable } = transform;

    const writer = writable.getWriter();

    const res = await Promise.allSettled([
      writer.write('hello'),
      writer.write('there'),
      writer.close(),
      consumeBytes(readable),
    ]);

    strictEqual(res[3].value, '<HELLOTHERE>');
  },
};
