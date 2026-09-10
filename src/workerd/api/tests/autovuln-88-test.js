// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Regression test for AUTOVULN-CLOUDFLARE-WORKERD-88.
//
// The destination's pipe lock (WritableLockImpl::PipeLocked) holds a raw
// reference `source` to the readable controller's PipeController. That object
// lives inline in a kj::OneOf inside the readable controller, so destroying it
// never frees heap memory -- the reference is left pointing at live storage
// holding some other state. ASAN therefore cannot see this; only the production
// cfi-vcall check can. See pipe-source-error-uaf-test.js for a sibling bug.
//
// The bug: checkSignal() releases the source (destroying that PipeController),
// then calls self.abort() *before* its caller in pipeLoop() gets a chance to run
// lock.releasePipeLock(). Everything abort() reaches -- including the JS abort
// event listener and, synchronously after it, doError() -- therefore observes a
// destination still in the PipeLocked state whose `source` already dangles.
// doError() then calls source.release() on it.
//
// Every other release site in pipeLoop() pairs source.release() with
// lock.releasePipeLock(); the fix closes this one remaining window by doing the
// same inside checkSignal(), before any JS runs.

import { rejects, strictEqual } from 'node:assert';

// Observes the dangling reference deterministically, without relying on a crash.
//
// The abort listener re-pipes the (now unlocked) readable to a second
// destination. That constructs a fresh PipeController in the exact same inline
// storage the stale `source` still points at, so the stale reference silently
// aliases the *second* pipe. Pre-fix, doError() then calls release() through it
// and unlocks a pipe it has nothing to do with. Post-fix, doError() finds no pipe
// lock and leaves it alone.
//
// preventCancel matters here: without it the first release() also *cancels* the
// readable, so the second pipe would immediately run to completion on a closed
// stream and release the lock on its own -- making rs.locked false either way and
// the assertion useless. preventCancel keeps the readable open, so the only thing
// that can unlock it is the stale reference.
export const abortedSignalPipeMustNotReleaseUnrelatedPipe = {
  async test() {
    let wsCtrl;
    const ws = new WritableStream({
      start(c) {
        wsCtrl = c;
      },
    });
    await Promise.resolve();

    const rs = new ReadableStream({});
    const ws2 = new WritableStream({});

    wsCtrl.signal.addEventListener('abort', () => {
      // checkSignal() has already released the source, so rs is unlocked here.
      // It is still open, so this pipe parks on a read that never completes.
      rs.pipeTo(ws2).catch(() => {});
    });

    const ac = new AbortController();
    ac.abort(new Error('pipe-abort'));
    await rejects(rs.pipeTo(ws, { signal: ac.signal, preventCancel: true }), {
      message: 'pipe-abort',
    });

    // The second pipe is still in progress and owns the readable. Pre-fix,
    // doError() released it through the stale reference and this is false.
    strictEqual(rs.locked, true);
  },
};

// The same window, observed the way it actually crashes in production: the
// listener re-locks the readable with a reader, so the stale `source` ends up
// pointing at a ReaderLocked -- a non-polymorphic type. doError()'s virtual
// release() call then reads a garbage vptr and jumps through it.
export const abortedSignalPipeSourceReleaseThenRelock = {
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
      rs.getReader();
    });

    const ac = new AbortController();
    ac.abort(new Error('pipe-abort'));
    await rejects(rs.pipeTo(ws, { signal: ac.signal }), {
      message: 'pipe-abort',
    });
  },
};
