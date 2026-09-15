// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// TransformStream API surface: globals, direct-construction guards, and
// the identity of the bare constructor. WPT transform-streams/
// properties.any.js and general.any.js own the full IDL shape; this
// module carries the workerd-side checks, most notably that a flagged
// `new TransformStream()` is the STANDARD JS-backed stream and not the
// non-standard IdentityTransformStream the unflagged constructor falls
// back to (see the legacy cell).

import { strictEqual, ok, throws } from 'node:assert';

// The transform classes are installed as globals.
export const transformGlobalsExist = {
  test() {
    ok(TransformStream !== undefined);
    ok(TransformStreamDefaultController !== undefined);
  },
};

// The controller is minted only by the stream machinery.
export const controllerNotConstructable = {
  test() {
    throws(() => new TransformStreamDefaultController(), TypeError);
  },
};

// A bare new TransformStream() is a pass-through JS-backed stream, NOT an
// IdentityTransformStream. Migrated from transform-streams-test.js.
export const defaultIdentityTransform = {
  async test() {
    const transform = new TransformStream();
    ok(!(transform instanceof IdentityTransformStream));

    const { readable, writable } = transform;

    const writer = writable.getWriter();
    const reader = readable.getReader();

    const after = await Promise.allSettled([
      writer.write('hello'),
      reader.read(),
    ]);

    strictEqual(after[0].value, undefined);
    strictEqual(after[1].value.value, 'hello');
  },
};

function createSavedController(size) {
  let controller;
  const stream = new TransformStream(
    {
      start(value) {
        controller = value;
      },
    },
    undefined,
    { highWaterMark: 1, size }
  );
  return {
    controller,
    readable: new WeakRef(stream.readable),
    writable: new WeakRef(stream.writable),
  };
}

export const savedControllerRetainsStream = {
  async test() {
    let sizeCalls = 0;
    const { controller, readable, writable } = createSavedController(() => {
      sizeCalls++;
      return 1;
    });

    for (let i = 0; i < 4; i++) {
      await scheduler.wait(0);
      gc();
    }

    const readableStream = readable.deref();
    ok(readableStream !== undefined);
    ok(writable.deref() !== undefined);
    strictEqual(controller.desiredSize, 1);
    controller.enqueue('chunk');
    strictEqual(sizeCalls, 1);
    const result = await readableStream.getReader().read();
    strictEqual(result.value, 'chunk');
    strictEqual(result.done, false);
  },
};
