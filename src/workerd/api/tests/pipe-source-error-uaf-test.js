// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Regression test for a use-after-free in WritableStreamJsController::pipeLoop().
//
// When a JS ReadableStream is piped to a JS WritableStream, the destination's
// pipe lock (WritableLockImpl::PipeLocked) holds a raw reference `source` to the
// readable controller's PipeLocked object. That object lives *inline* in a
// kj::OneOf inside the readable controller (which stays alive via the pipe's
// readableStreamRef), so it is never heap-freed -- which is why ASAN never saw
// this and only the production cfi-vcall check caught it.
//
// The bug: if the source errors while a write is pending (no read in flight),
// ReadableStreamJsController::doError() transitions immediately and
// ReadableLockImpl::onError() runs transitionFromTo<PipeLocked, Unlocked>(),
// destroying the PipeController that `source` still points at. When the pending
// write resolves, pipeLoop() dereferences the dangling `source` via
// source.tryGetErrored() -- a virtual call on a destroyed object.
//
// The invariant that guards against this: while a pipe is in progress the pipe
// must keep the source locked until the pipe settles. This test observes that
// invariant directly (readable.locked), because the underlying corruption is
// only detectable under CFI, not in a normal/ASAN build.

import { strictEqual } from 'node:assert';
import { rejects } from 'node:assert';

export const pipeSourceErrorDuringPendingWrite = {
  async test() {
    let sourceController;
    let resolveWrite;
    let signalWriteStarted;
    const writeStarted = new Promise((resolve) => {
      signalWriteStarted = resolve;
    });

    const readable = new ReadableStream({
      start(controller) {
        sourceController = controller;
        // Enqueue a single chunk so the pipe performs one read (satisfied from
        // the queue) and then attempts a write. After the read resolves there
        // is no read operation in flight, so a later error() transitions the
        // readable controller synchronously.
        controller.enqueue('chunk');
      },
    });

    const writable = new WritableStream({
      write() {
        // Park the write by returning a promise we resolve manually. While it
        // is pending, no read is in flight on the source.
        return new Promise((resolve) => {
          resolveWrite = resolve;
          signalWriteStarted();
        });
      },
    });

    const pipePromise = readable.pipeTo(writable);

    // Wait until the destination's write() has been entered and parked.
    await writeStarted;

    // The pipe owns the source: it must be locked.
    strictEqual(readable.locked, true);

    // Error the source while the write is pending.
    sourceController.error(new Error('boom'));
    await scheduler.wait(0);

    // The pipe is still in progress (the parked write hasn't resolved), so the
    // pipe must still own the source. Pre-fix, onError() released the lock here
    // (readable.locked === false) and destroyed the PipeController that the
    // destination's pipe lock still references.
    strictEqual(readable.locked, true);

    // Resolving the write drives pipeLoop() again. Pre-fix this dereferenced the
    // dangling `source`; post-fix it observes the errored source and rejects.
    resolveWrite();

    await rejects(pipePromise, { message: 'boom' });

    // The pipe has settled, so the source is released.
    strictEqual(readable.locked, false);
  },
};
