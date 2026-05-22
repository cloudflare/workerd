// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { rejects } from 'node:assert';

// Regression test for AUTOVULN-CLOUDFLARE-WORKERD-187.
// When ENABLE_DRAINING_READ_ON_STANDARD_STREAMS autogate is enabled,
// pumpToImpl runs pull() synchronously inside a kj ChainPromiseNode.
// If pull() calls ac.abort() on the request's signal, the canceler
// synchronously destroys the pumpToImpl coroutine frame (including the
// firing ChainPromiseNode), triggering KJ_REQUIRE(!firing) in
// Event::~Event() noexcept → std::terminate.
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
