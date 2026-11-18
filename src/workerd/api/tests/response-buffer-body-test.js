// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { strictEqual, rejects } from 'assert';

// The Response that the fetch handler last returned, so that tests can inspect it after it was
// sent.
let lastResponse;

export default {
  fetch(request) {
    const path = new URL(request.url).pathname;
    if (path === '/error') {
      return Response.error();
    }
    if (path === '/async') {
      return Promise.resolve(new Response('async'));
    }
    lastResponse = new Response('sync', { headers: { 'x-test': '1' } });
    return lastResponse;
  },
};

export const syncResponseIsSent = {
  async test(_, env) {
    const response = await env.self.fetch('http://example.org/');
    strictEqual(response.status, 200);
    strictEqual(response.headers.get('x-test'), '1');
    strictEqual(await response.text(), 'sync');

    // Sending the body without constructing its stream still leaves it used, and a stream
    // constructed afterwards is locked, as if the body had been pumped from a stream.
    strictEqual(lastResponse.bodyUsed, true);
    strictEqual(lastResponse.body.locked, true);
    strictEqual(lastResponse.bodyUsed, true);
  },
};

export const asyncResponseIsSent = {
  async test(_, env) {
    const response = await env.self.fetch('http://example.org/async');
    strictEqual(await response.text(), 'async');
  },
};

export const syncErrorResponseIsRejected = {
  async test(_, env) {
    await rejects(env.self.fetch('http://example.org/error'), {
      message: /must not be an error response/,
    });
  },
};

export const unsentBufferBodyIsReadable = {
  async test() {
    strictEqual(await new Response('text').text(), 'text');

    const response = new Response('streamed');
    strictEqual(response.bodyUsed, false);
    strictEqual(response.body.locked, false);
    const clone = response.clone();
    strictEqual(await new Response(response.body).text(), 'streamed');
    strictEqual(response.bodyUsed, true);
    strictEqual(await clone.text(), 'streamed');

    const bytes = new Uint8Array(await new Response('bytes').arrayBuffer());
    strictEqual(new TextDecoder().decode(bytes), 'bytes');
  },
};
