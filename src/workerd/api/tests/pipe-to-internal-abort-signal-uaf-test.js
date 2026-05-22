// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Regression test for AUTOVULN-CLOUDFLARE-WORKERD-261:
// UAF of ReadableStreamJsController via dangling PipeController& in
// WritableStreamInternalController::Pipe::State after AbortSignal
// with preventAbort. The fix pops the Pipe event from the queue in
// checkSignal's preventAbort branch before releasing the source.
//
// The dangling reference is into in-place state-machine union storage (the
// source's ReadLockState OneOf). Whether ASAN catches this depends on
// whether the ReadableStream itself is freed before the stale deref runs,
// which is architecture- and timing-dependent. The companion C++ unit test
// in streams-test.c++ provides a deterministic ASAN signal by controlling
// the source's jsg::Ref lifetime directly.

import { ok, rejects } from 'node:assert';

// pipeTo with signal + preventAbort:true must not crash when signal
// fires during pull. Pre-patch: UAF (SIGSEGV under ASAN on some
// platforms). Post-patch: pipe promise rejects cleanly, writable unlocked.
export const pipeToInternalAbortSignalPreventAbortRegression = {
  async test() {
    const ac = new AbortController();
    const enc = new TextEncoder();

    const rs = new ReadableStream({
      pull(controller) {
        controller.enqueue(enc.encode('hello'));
        ac.abort(new Error('signal-aborted'));
      },
    });

    const { writable } = new IdentityTransformStream();

    const pipePromise = rs.pipeTo(writable, {
      signal: ac.signal,
      preventAbort: true,
    });

    await rejects(pipePromise, { message: 'signal-aborted' });

    ok(
      !writable.locked,
      'writable should be unlocked after pipe abort with preventAbort'
    );

    const writer = writable.getWriter();
    writer.releaseLock();
  },
};

// pipeTo with signal + preventAbort:false also handles abort cleanly.
// Exercises the !preventAbort branch of checkSignal (drain path).
export const pipeToInternalAbortSignalNoDrainRegression = {
  async test() {
    const ac = new AbortController();
    const enc = new TextEncoder();

    const rs = new ReadableStream({
      pull(controller) {
        controller.enqueue(enc.encode('world'));
        ac.abort(new Error('signal-aborted-no-prevent'));
      },
    });

    const { writable } = new IdentityTransformStream();

    const pipePromise = rs.pipeTo(writable, {
      signal: ac.signal,
      preventAbort: false,
    });

    await rejects(pipePromise, {
      message: 'signal-aborted-no-prevent',
    });
  },
};
