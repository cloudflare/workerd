// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Regression test for AUTOVULN-CLOUDFLARE-WORKERD-262.
// DrainingReader owns GC-participating state (jsg::Ref, promises) but
// is destroyed off-lock when pumpToImpl's coroutine frame is torn down
// on the KJ event loop. The off-lock destruction may cause cppgc
// invariant violations (CheckMemoryIsInaccessible) during the next
// GC sweep.
export default {
  async test() {
    for (let iter = 0; iter < 200; iter++) {
      let n = 0;
      const stream = new ReadableStream({
        pull(c) {
          n++;
          if (n < 3) c.enqueue(new Uint8Array(8));
          else c.close();
        },
      });
      const out = new HTMLRewriter().transform(
        new Response(stream, {
          headers: { 'content-type': 'text/html' },
        })
      );
      await out.text();
      gc();
    }
  },
};
