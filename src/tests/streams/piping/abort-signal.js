// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// How a pipe observes its AbortSignal: only a real abort stops it (the
// spec's abort algorithm), not the 'abort' event, and nothing a user's
// 'abort' listener does can prevent it.

import { strictEqual, deepStrictEqual } from 'node:assert';

function recordingPipe(signal) {
  let rc;
  const rs = new ReadableStream({
    start(c) {
      rc = c;
    },
  });
  const written = [];
  const ws = new WritableStream({
    write(chunk) {
      written.push(chunk);
    },
  });
  const pipe = rs.pipeTo(ws, { signal });
  return { rs, ws, rc, written, pipe };
}

// A synthetic 'abort' event on a signal that is not aborted leaves the
// pipe running.
export const syntheticAbortEventIgnored = {
  async test() {
    const ac = new AbortController();
    const { rs, ws, rc, written, pipe } = recordingPipe(ac.signal);
    await scheduler.wait(0);
    ac.signal.dispatchEvent(new Event('abort'));
    rc.enqueue('a');
    rc.close();
    await pipe;
    deepStrictEqual(written, ['a']);
    strictEqual(rs.locked, false);
    strictEqual(ws.locked, false);
  },
};

// A listener registered before the pipe that stops immediate propagation
// does not keep the abort from the pipe.
export const stopImmediatePropagationDoesNotBlockAbort = {
  async test() {
    const ac = new AbortController();
    const reason = new Error('boom');
    ac.signal.addEventListener('abort', (e) => e.stopImmediatePropagation());
    const { rs, ws, rc, pipe } = recordingPipe(ac.signal);
    await scheduler.wait(0);
    ac.abort(reason);
    // The C++ pipe notices the abort on its next step.
    rc.enqueue('a');
    const outcome = await Promise.race([
      pipe.then(
        () => 'fulfilled',
        (e) => e
      ),
      scheduler.wait(1000).then(() => 'pending'),
    ]);
    strictEqual(outcome, reason);
    strictEqual(rs.locked, false);
    strictEqual(ws.locked, false);
  },
};

// Aborting after the pipe has settled has no effect on either stream.
export const abortAfterPipeSettled = {
  async test() {
    const ac = new AbortController();
    const { rs, ws, rc, written, pipe } = recordingPipe(ac.signal);
    rc.enqueue('a');
    rc.close();
    await pipe;
    ac.abort(new Error('late'));
    await scheduler.wait(0);
    deepStrictEqual(written, ['a']);
    const writer = ws.getWriter();
    await writer.closed;
    strictEqual(rs.locked, false);
  },
};
