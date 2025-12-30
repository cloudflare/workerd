// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { strictEqual, ok } from 'node:assert';

async function consume(readable) {
  let data = '';
  for await (const chunk of readable) {
    data += chunk;
  }
  return data;
}

// Test default identity transform
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

// Test simple transform with start/transform/flush
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

// Test async transform with delays
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

// Test transform with different types (string input, Uint8Array output)
export const differentTypesTransform = {
  async test() {
    const enc = new TextEncoder();

    async function consumeBytes(readable) {
      const dec = new TextDecoder();
      let data = '';
      for await (const chunk of readable) {
        data += dec.decode(chunk, { stream: true });
      }
      data += dec.decode();
      return data;
    }

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

// Test sync error during start
export const syncErrorDuringStart = {
  async test() {
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

// Test async error during start
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

// Test sync error during transform
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

// Test async error during transform
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

// Test sync error during flush
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

// Test async error during flush
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

// Test write backpressure
export const writeBackpressure = {
  async test() {
    let expectedReadSize = 2;
    const transform = new TransformStream(
      {
        transform(chunk, controller) {
          strictEqual(controller.desiredSize, expectedReadSize--);
          controller.enqueue(chunk);
        },
      },
      { highWaterMark: 2 },
      { highWaterMark: 2 }
    );

    const writer = transform.writable.getWriter();
    strictEqual(writer.desiredSize, 2);

    const promises = [writer.write('hello'), writer.write('there')];

    strictEqual(writer.desiredSize, 0);

    await Promise.allSettled(promises);

    strictEqual(writer.desiredSize, 2);
  },
};

// Test that piping from a JS-backed TransformStream through an
// IdentityTransformStream does not result in a hung pipeTo promise.
export const transformRoundtrip = {
  async test(ctrl, env, ctx) {
    const enc = new TextEncoder();
    const dec = new TextDecoder();

    const testData = 'hello world test data';
    const compressedStream = new ReadableStream({
      start(controller) {
        controller.enqueue(enc.encode(testData));
        controller.close();
      },
    }).pipeThrough(new CompressionStream('gzip'));

    const compressedChunks = [];
    const compressedReader = compressedStream.getReader();
    for (;;) {
      const { done, value } = await compressedReader.read();
      if (done) break;
      compressedChunks.push(value);
    }
    const compressedData = new Uint8Array(
      compressedChunks.reduce((acc, chunk) => acc + chunk.length, 0)
    );
    let offset = 0;
    for (const chunk of compressedChunks) {
      compressedData.set(chunk, offset);
      offset += chunk.length;
    }

    const inputStream = new ReadableStream({
      start(controller) {
        controller.enqueue(compressedData);
        controller.close();
      },
    });

    const decompression = new DecompressionStream('gzip');
    const ts = new TransformStream({
      transform(chunk, controller) {
        controller.enqueue(chunk);
      },
    });
    const { readable, writable } = new IdentityTransformStream();

    ctx.waitUntil(
      inputStream.pipeThrough(decompression).pipeThrough(ts).pipeTo(writable)
    );

    const outputChunks = [];
    const reader = readable.getReader();
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      outputChunks.push(value);
    }

    const output = dec.decode(
      new Uint8Array(
        outputChunks.reduce((acc, chunk) => [...acc, ...chunk], [])
      )
    );
    strictEqual(output, testData);
  },
};
