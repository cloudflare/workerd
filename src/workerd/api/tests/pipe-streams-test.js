// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { strictEqual, ok, rejects } from 'node:assert';

// Test pipeThrough from JavaScript readable to internal writable
export const pipeThroughJsToInternal = {
  async test() {
    const enc = new TextEncoder();
    const dec = new TextDecoder();
    const chunks = [enc.encode('hello'), enc.encode('there'), 'hello'];
    const rs = new ReadableStream({
      pull(c) {
        c.enqueue(chunks.shift());
        if (chunks.length === 0) c.close();
      },
    });
    const transform = new IdentityTransformStream();
    const readable = rs.pipeThrough(transform);

    const output = [];
    let errored = false;
    try {
      for await (const chunk of readable) {
        output.push(dec.decode(chunk));
      }
    } catch (err) {
      // The 'hello' string at the end of chunks will cause an error to be thrown.
      strictEqual(
        err.message,
        'This WritableStream only supports writing byte types.'
      );
      errored = true;
    }

    strictEqual(output[0], 'hello');
    strictEqual(output[1], 'there');
    strictEqual(output.length, 2);
    ok(errored);
  },
};

// Test pipeThrough error in JS Readable aborts Internal Writable when preventAbort = false
export const pipeThroughJsToInternalErroredSource = {
  async test() {
    const enc = new TextEncoder();
    const rs = new ReadableStream({
      async pull() {
        throw new Error('boom');
      },
    });
    const transform = new IdentityTransformStream();
    const readable = rs.pipeThrough(transform);

    ok(transform.writable.locked);

    const reader = readable.getReader();

    await rejects(reader.read(), { message: 'boom' });

    ok(!transform.writable.locked);

    // Attempts to use the writable from here on will fail with the same error.
    const writer = transform.writable.getWriter();
    await rejects(writer.write(enc.encode('hello')), { message: 'boom' });
  },
};

// Test pipeTo error in JS Readable aborts Internal Writable when preventAbort = false
export const pipeToJsToInternalErroredSource = {
  async test() {
    const enc = new TextEncoder();
    const rs = new ReadableStream({
      async pull() {
        throw new Error('boom');
      },
    });
    const { readable, writable } = new IdentityTransformStream();
    const pipe = rs.pipeTo(writable);

    ok(writable.locked);

    const reader = readable.getReader();

    await rejects(reader.read(), { message: 'boom' });

    ok(!writable.locked);

    // Attempts to use the writable from here on will fail with the same error.
    const writer = writable.getWriter();
    await rejects(writer.write(enc.encode('hello')), { message: 'boom' });

    await rejects(pipe, { message: 'boom' });
  },
};

// Test pipeThrough from internal readable to JS writable
export const pipeThroughInternalToJs = {
  async test(_ctrl, env) {
    const response = await env.SERVICE.fetch('http://test/stream');
    const dec = new TextDecoder();

    const output = [];
    const ws = new WritableStream({
      write(chunk) {
        output.push(dec.decode(chunk));
      },
    });

    const ts = new TransformStream();
    const pipePromise = response.body.pipeThrough(ts).pipeTo(ws);

    await pipePromise;

    strictEqual(output.join(''), 'hello world '.repeat(100));
  },
};

// Test pipeTo from internal readable to JS writable
export const pipeToInternalToJs = {
  async test(_ctrl, env) {
    const response = await env.SERVICE.fetch('http://test/stream');
    const dec = new TextDecoder();

    const output = [];
    const ws = new WritableStream({
      write(chunk) {
        output.push(dec.decode(chunk));
      },
    });

    await response.body.pipeTo(ws);

    strictEqual(output.join(''), 'hello world '.repeat(100));
  },
};

// Test pipeThrough from JS readable to JS writable
export const pipeThroughJsToJs = {
  async test() {
    const enc = new TextEncoder();
    const dec = new TextDecoder();

    const chunks = [enc.encode('hello'), enc.encode('there')];
    const rs = new ReadableStream({
      pull(c) {
        const chunk = chunks.shift();
        if (chunk) {
          c.enqueue(chunk);
        } else {
          c.close();
        }
      },
    });

    const ts = new TransformStream({
      transform(chunk, controller) {
        controller.enqueue(dec.decode(chunk).toUpperCase());
      },
    });

    const output = [];
    const ws = new WritableStream({
      write(chunk) {
        output.push(chunk);
      },
    });

    await rs.pipeThrough(ts).pipeTo(ws);

    strictEqual(output.join(''), 'HELLOTHERE');
  },
};

// Default fetch handler for service binding requests
export default {
  async fetch(request) {
    if (request.url.includes('/stream')) {
      const data = 'hello world '.repeat(100);
      return new Response(data);
    }
    return new Response('Not found', { status: 404 });
  },
};

// =============================================================================
// Porting status from edgeworker/src/edgeworker/api-tests/streams/pipe.ew-test
//
// PORTED (can be deleted from ew-test):
// - pipethrough-js-to-internal: Ported as pipeThroughJsToInternal
// - pipethrough-js-to-internal-errored-source: Ported as pipeThroughJsToInternalErroredSource
// - pipeto-js-to-internal-errored-source: Ported as pipeToJsToInternalErroredSource
// - pipethrough-internal-to-js: Ported as pipeThroughInternalToJs
// - pipeto-internal-to-js: Ported as pipeToInternalToJs
// - pipethrough-js-to-js: Ported as pipeThroughJsToJs
//
// TODO: The following tests remain to be ported:
//
// - simple-pipeto-js-to-internal: Tests pipeTo from JS ReadableStream to internal
//   IdentityTransformStream writable when a non-byte chunk causes an error.
//
// - pipethrough-js-to-internal-errored-source-prevent-abort: Tests that preventAbort=true
//   keeps the writable usable after source errors.
//
// - pipeto-js-to-internal-errored-source-prevent-abort: Same as above but with pipeTo.
//
// - pipethrough-internal-to-js-errored-dest: Tests error propagation when JS writable errors.
//
// - pipeto-internal-to-js-errored-dest: Same as above but with pipeTo.
//
// - Various preventClose/preventCancel option tests.
// =============================================================================
