// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { rejects } from 'node:assert';

// Regression test for AUTOVULN-CLOUDFLARE-WORKERD-88.
// In the standard controller pipe path, checkSignal() calls
// source.release() (destroying the readable's PipeLocked), then
// self.abort() which triggers the abort signal listener. The listener
// calls rs.getReader() overwriting the OneOf with ReaderLocked.
// Later, doError() tries pipeLocked.source.release() through the
// now-corrupted reference → SIGSEGV.
export const preAbortedSignalPipeSourceReleaseThenRelock = {
  async test() {
    let wsCtrl;
    const ws = new WritableStream({
      start(c) {
        wsCtrl = c;
      },
    });
    await Promise.resolve();

    const rs = new ReadableStream({});

    wsCtrl.signal.addEventListener('abort', () => {
      // Post-fix: rs.locked should still be true because the readable's
      // PipeLocked should not have been destroyed yet.
      // Pre-fix: rs.locked is false, getReader() succeeds, vtable corrupted.
      rs.getReader();
    });

    const ac = new AbortController();
    ac.abort(new Error('pipe-abort'));
    await rejects(rs.pipeTo(ws, { signal: ac.signal }), {
      message: 'pipe-abort',
    });
  },
};
