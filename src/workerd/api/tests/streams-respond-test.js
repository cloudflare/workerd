// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { strictEqual } from 'node:assert';

// Test Response body methods with JS-backed BYOB ReadableStream
export const responseBodyMethodsJsByob = {
  async test() {
    const enc = new TextEncoder();
    const dec = new TextDecoder();

    {
      const rs = new ReadableStream({
        type: 'bytes',
        async pull(c) {
          if (c.byobRequest) {
            enc.encodeInto('hello', c.byobRequest.view);
            c.byobRequest.respond(5);
            c.close();
          }
        },
      });

      const resp = new Response(rs);

      strictEqual(dec.decode(await resp.arrayBuffer()), 'hello');
    }

    {
      const rs = new ReadableStream({
        type: 'bytes',
        async pull(c) {
          if (c.byobRequest) {
            enc.encodeInto('hello', c.byobRequest.view);
            c.byobRequest.respond(5);
            c.close();
          }
        },
      });

      const resp = new Response(rs);

      strictEqual(await resp.text(), 'hello');
    }
  },
};

// Test Request body methods with JS-backed ReadableStream
export const requestBodyMethodsJsByob = {
  async test() {
    const enc = new TextEncoder();
    const wrapped = new ReadableStream({
      type: 'bytes',
      async pull(c) {
        c.enqueue(enc.encode('hello'));
        c.close();
      },
    });

    const req = new Request('http://example.com', {
      method: 'POST',
      body: wrapped,
    });
    const text = await req.text();

    strictEqual(text, 'hello');
  },
};

// Test basic JS ReadableStream as Response body
export const jsSource = {
  async test() {
    const enc = new TextEncoder();
    const rs = new ReadableStream({
      start(c) {
        c.enqueue(enc.encode('hello'));
        c.close();
      },
    });

    const response = new Response(rs);
    strictEqual(await response.text(), 'hello');
  },
};

// Test JS ReadableStream with async pull as Response body
export const jsSourceAsyncPull = {
  async test() {
    const enc = new TextEncoder();
    const chunks = [enc.encode('hello'), enc.encode('there')];
    const rs = new ReadableStream({
      async pull(c) {
        await scheduler.wait(10);
        if (chunks.length > 0) c.enqueue(chunks.shift());
        if (chunks.length === 0) c.close();
      },
    });

    const response = new Response(rs);
    strictEqual(await response.text(), 'hellothere');
  },
};

// Test BYOB ReadableStream as Response body
export const jsByteSource = {
  async test() {
    const enc = new TextEncoder();
    const rs = new ReadableStream({
      type: 'bytes',
      pull(c) {
        const request = c.byobRequest;
        if (request != null) {
          enc.encodeInto('hello', request.view);
          request.respond(5);
          c.close();
        }
      },
    });

    const response = new Response(rs);
    strictEqual(await response.text(), 'hello');
  },
};

// Test BYOB ReadableStream with multiple chunks
export const jsByteSourceMultipleChunks = {
  async test() {
    const enc = new TextEncoder();
    const chunks = ['hello', 'there', 'this', 'is', 'a', 'test'];

    const rs = new ReadableStream({
      type: 'bytes',
      async pull(c) {
        await scheduler.wait(10);
        const request = c.byobRequest;
        if (request != null) {
          const chunk = chunks.shift();
          if (chunk !== undefined) {
            const { written } = enc.encodeInto(chunk, request.view);
            request.respond(written);
          } else {
            c.close();
            request.respond(0);
          }
        }
      },
    });

    const response = new Response(rs);
    strictEqual(await response.text(), 'hellotherethisisatest');
  },
};

// Test teed JS ReadableStream as Response body
export const jsTeeSource = {
  async test() {
    const enc = new TextEncoder();
    const dec = new TextDecoder();
    const readable = new ReadableStream({
      start(c) {
        c.enqueue(enc.encode('hello'));
        c.close();
      },
    });

    const [branch1, branch2] = readable.tee();
    const reader = branch2.getReader();

    // By reading first, we ensure that the branch1 will have queued
    // read data before the Response is actually transmitted. Both tee
    // branches should still resolve to the same data.

    const result = await reader.read();
    strictEqual(dec.decode(result.value), 'hello');

    const response = new Response(branch1);
    strictEqual(await response.text(), 'hello');
  },
};

// Test closed teed BYOB stream
export const jsTeeClose = {
  async test() {
    const rs = new ReadableStream({
      type: 'bytes',
      async pull(c) {
        if (c.byobRequest) {
          c.close();
          c.byobRequest.respond(0);
        }
      },
    });

    const [branch] = rs.tee();
    const response = new Response(branch);
    strictEqual(await response.text(), '');
  },
};

// Test large enqueue with synchronous close (default stream)
export const bigEnqueue = {
  async test() {
    const a = 'a'.repeat(4096 * 2);
    const enc = new TextEncoder();
    const rs = new ReadableStream({
      start(c) {
        c.enqueue(enc.encode(a));
        c.close();
      },
    });

    const response = new Response(rs);
    const text = await response.text();
    strictEqual(text.length, 8192);
    strictEqual(text, a);
  },
};

// Test large enqueue with synchronous close (bytes stream)
export const bigEnqueueBytes = {
  async test() {
    const a = 'a'.repeat(4096 * 2);
    const enc = new TextEncoder();
    const rs = new ReadableStream({
      type: 'bytes',
      start(c) {
        c.enqueue(enc.encode(a));
        c.close();
      },
    });

    const response = new Response(rs);
    const text = await response.text();
    strictEqual(text.length, 8192);
    strictEqual(text, a);
  },
};

// Test large enqueue via IdentityTransformStream (sync close)
export const bigEnqueueViaIdentityTransform = {
  async test() {
    const a = 'a'.repeat(4096 * 2 + 345);
    const enc = new TextEncoder();

    const rs = new ReadableStream({
      start(c) {
        c.enqueue(enc.encode(a));
        c.close();
      },
    });

    const transform = new IdentityTransformStream();
    const response = new Response(rs.pipeThrough(transform));

    const text = await response.text();
    strictEqual(text.length, 8537);
    strictEqual(text, a);
  },
};

// Test large enqueue via IdentityTransformStream (async close)
export const bigEnqueueViaIdentityTransformAsync = {
  async test() {
    const a = 'a'.repeat(4096 * 2 + 345);
    const enc = new TextEncoder();

    const rs = new ReadableStream({
      async start(c) {
        c.enqueue(enc.encode(a));
        await scheduler.wait(10);
        c.close();
      },
    });

    const transform = new IdentityTransformStream();
    const response = new Response(rs.pipeThrough(transform));

    const text = await response.text();
    strictEqual(text.length, 8537);
    strictEqual(text, a);
  },
};

// Test large enqueue via JS TransformStream (sync close)
export const bigEnqueueViaJsTransform = {
  async test() {
    const a = 'a'.repeat(4096 * 2 + 345);
    const enc = new TextEncoder();

    const rs = new ReadableStream({
      start(c) {
        c.enqueue(enc.encode(a));
        c.close();
      },
    });

    const transform = new TransformStream();
    const response = new Response(rs.pipeThrough(transform));

    const text = await response.text();
    strictEqual(text.length, 8537);
    strictEqual(text, a);
  },
};

// Test large enqueue via JS TransformStream (async close)
export const bigEnqueueViaJsTransformAsync = {
  async test() {
    const a = 'a'.repeat(4096 * 2 + 345);
    const enc = new TextEncoder();

    const rs = new ReadableStream({
      async start(c) {
        c.enqueue(enc.encode(a));
        await scheduler.wait(10);
        c.close();
      },
    });

    const transform = new TransformStream();
    const response = new Response(rs.pipeThrough(transform));

    const text = await response.text();
    strictEqual(text.length, 8537);
    strictEqual(text, a);
  },
};

// Test enqueue same chunk multiple times (default stream)
export const enqueueChunkMultipleTimes = {
  async test() {
    const chunk = new TextEncoder().encode('ping!');
    const rs = new ReadableStream({
      start(controller) {
        controller.enqueue(chunk);
        controller.enqueue(chunk);
        controller.enqueue(chunk);
        controller.enqueue(chunk);
        controller.close();
      },
    });

    const response = new Response(rs);
    strictEqual(await response.text(), 'ping!ping!ping!ping!');
  },
};

// Test enqueue same chunk multiple times errors in bytes stream
export const enqueueChunkMultipleTimesBytes = {
  async test() {
    const chunk = new TextEncoder().encode('ping!');
    const rs = new ReadableStream({
      type: 'bytes',
      start(controller) {
        controller.enqueue(chunk);
        try {
          controller.enqueue(chunk);
          throw new Error('this should have failed because chunk is size 0');
        } catch (err) {
          if (err.message !== 'Cannot enqueue a zero-length ArrayBuffer.') {
            throw new Error('Incorrect error: ' + err.message);
          }
          controller.close();
        }
      },
    });

    const response = new Response(rs);
    strictEqual(await response.text(), 'ping!');
  },
};

// In this test, we write data into an IdentityTransformStream
// We then read from that in a JS ReadableStream
// We then pipe that through a JS TransformStream
// We then respond with the TransformStream's readable.
// We use parallel writes to simulate waitUntil() behavior.
export const multistepTransform = {
  async test() {
    const enc = new TextEncoder();
    const dec = new TextDecoder();

    const { readable, writable } = new IdentityTransformStream();
    const reader = readable.getReader({ mode: 'byob' });

    const rs = new ReadableStream({
      type: 'bytes',
      start(c) {
        c.enqueue(enc.encode('bbbb'));
      },
      async pull(c) {
        const buffer = new Uint8Array(4096);
        const result = await reader.read(buffer);
        if (result.done) {
          c.enqueue(enc.encode('bye'));
          c.close();
        } else {
          const view = c.byobRequest.view;
          const toCopy = Math.min(result.value.byteLength, view.byteLength);
          new Uint8Array(view.buffer, view.byteOffset, toCopy).set(
            result.value.subarray(0, toCopy)
          );
          c.byobRequest.respond(toCopy);
        }
      },
    });

    const transform = new TransformStream({
      transform(chunk, controller) {
        controller.enqueue(enc.encode(dec.decode(chunk).toUpperCase()));
      },
    });

    const response = new Response(rs.pipeThrough(transform));

    const writer = writable.getWriter();
    const writePromise = (async () => {
      await writer.write(enc.encode('a'.repeat(4090)));
      await writer.write(enc.encode('a'.repeat(4096)));
      await writer.write(enc.encode('a'.repeat(345)));
      await writer.close();
    })();

    const [text] = await Promise.all([response.text(), writePromise]);

    strictEqual(text.startsWith('BBBB'), true);
    strictEqual(text.endsWith('BYE'), true);
    strictEqual(text.length, 4 + 4090 + 4096 + 345 + 3);
  },
};
