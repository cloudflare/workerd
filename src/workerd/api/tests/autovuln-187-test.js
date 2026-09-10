// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { rejects } from 'node:assert';

// Regression test for AUTOVULN-CLOUDFLARE-WORKERD-187.
//
// A draining read delivers its result to pumpToImpl through a kj event, and it
// runs the stream's pull() callback along the way. A pull() that aborts the
// request's signal makes the canceler destroy the pump's coroutine frame from
// inside that event, so the event is destroyed while still firing and kj aborts
// the process with "Promise callback destroyed itself".
export default {
  async test(_ctrl, env) {
    const ac = new AbortController();
    let n = 0;
    const rs = new ReadableStream({
      pull(c) {
        if (++n === 2) ac.abort();
        c.enqueue(new Uint8Array([65]));
      },
    });
    await rejects(
      env.ECHO.fetch('http://x/', {
        method: 'POST',
        body: rs,
        signal: ac.signal,
        duplex: 'half',
      }),
      {
        message: 'The operation was aborted',
      }
    );
  },
};
