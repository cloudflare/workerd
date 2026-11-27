// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// A Worker that feeds HTMLRewriter's input by hand can deadlock itself: here an element handler
// waits on a write that only the rewriter can consume, and the rewriter waits on the handler. The
// runtime must notice that nothing is left that could make progress and fail the request, rather
// than leaving it hanging forever.

import { rejects } from 'node:assert';

// Reads all of `body` through a reader, rather than collecting it in one go as arrayBuffer() does.
async function readThroughReader(body) {
  const reader = body.getReader();
  let done = false;
  while (!done) {
    ({ done } = await reader.read());
  }
}

export default {
  async fetch(request) {
    const { readable, writable } = new IdentityTransformStream();
    const writer = writable.getWriter();
    const encoder = new TextEncoder();

    const response = new HTMLRewriter()
      .on('body', {
        async element(element) {
          element.prepend('foo ');
          // The rewriter reads no further until this handler returns, so this write never
          // completes.
          await writer.write(encoder.encode('</html>'));
          await writer.close();
        },
      })
      .transform(new Response(readable));

    writer.write(encoder.encode('<html>'));
    writer.write(encoder.encode('<body>'));
    writer.write(encoder.encode('bar'));
    writer.write(encoder.encode('</body>'));

    if (new URL(request.url).pathname === '/reader') {
      await readThroughReader(response.body);
    } else {
      await response.arrayBuffer();
    }
    return new Response('unreachable');
  },
};

export const deadlockWhileCollectingTheOutput = {
  async test(ctrl, env) {
    await rejects(env.SELF.fetch('http://example.com/array-buffer'), {
      message: /never generate a response/,
    });
  },
};

export const deadlockWhileReadingTheOutput = {
  async test(ctrl, env) {
    await rejects(env.SELF.fetch('http://example.com/reader'), {
      message: /never generate a response/,
    });
  },
};
