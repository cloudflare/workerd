// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Round trips through the SELF service binding to this worker's fetch
// handler, which runs in the same isolate under the same implementation:
// request bodies streamed in and echoed back, response bodies read with a
// reader, and a response streamed as short text chunks through
// TextEncoderStream, the shape of a worker relaying generated tokens.

import {
  SHAPES,
  byteStream,
  bytesOf,
  drainBytes,
  expectEqual,
  shapeLabel,
  sizeLabel,
  valueStream,
} from 'helpers';

const TOKEN = 'word ';

function tokenStream(count) {
  let remaining = count;
  return new ReadableStream({
    pull(controller) {
      controller.enqueue(TOKEN);
      if (--remaining === 0) controller.close();
    },
  });
}

// The callee: POST echoes the request body; GET /bytes/<size> answers with
// <size> bytes, and GET /tokens/<count> with <count> tokens.
export default {
  fetch(request) {
    if (request.method === 'POST') return new Response(request.body);
    const [, route, arg] = new URL(request.url).pathname.split('/');
    if (route === 'bytes') return new Response(bytesOf(Number(arg)));
    if (route === 'tokens') {
      return new Response(
        tokenStream(Number(arg)).pipeThrough(new TextEncoderStream())
      );
    }
    return new Response(null, { status: 404 });
  },
};

async function echo(self, body, size) {
  const response = await self.fetch('http://self/echo', {
    method: 'POST',
    body,
  });
  const read = await response.arrayBuffer();
  expectEqual(read.byteLength, size, 'bytes echoed');
}

export const serviceBinding = {
  bench(b, env) {
    for (const size of [1024, 65536, 1048576]) {
      const bytes = bytesOf(size);
      b.run(`POST a Uint8Array, echoed, ${sizeLabel(size)}`, () =>
        echo(env.SELF, bytes, size)
      );
    }
    for (const [count, size] of SHAPES) {
      b.run(`POST a value stream, echoed, ${shapeLabel(count, size)}`, () =>
        echo(env.SELF, valueStream(count, size), count * size)
      );
    }
    const [count, size] = SHAPES[1];
    b.run(`POST a byte stream, echoed, ${shapeLabel(count, size)}`, () =>
      echo(env.SELF, byteStream(count, size), count * size)
    );
    for (const size of [65536, 1048576]) {
      b.run(`GET, read with a reader, ${sizeLabel(size)}`, async () => {
        const response = await env.SELF.fetch(`http://self/bytes/${size}`);
        expectEqual(
          await drainBytes(response.body.getReader()),
          size,
          'bytes read'
        );
      });
    }
    b.run('GET, 256 tokens through TextEncoderStream', async () => {
      const response = await env.SELF.fetch('http://self/tokens/256');
      expectEqual(
        await drainBytes(response.body.getReader()),
        256 * TOKEN.length,
        'bytes read'
      );
    });
  },
};
