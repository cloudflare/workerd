// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { rejects, strictEqual } from 'node:assert';

// Regression test for AUTOVULN-CLOUDFLARE-WORKERD-66.
// When a JS-backed ReadableStream is piped to an internal writable,
// Pipe::State::source is a raw PipeController& into the readable's
// lock state OneOf. If controller.error() is called during an in-flight
// write, onError() previously transitioned PipeLocked → Unlocked,
// destroying the PipeLocked. The attacker could then call getReader()
// to overwrite the OneOf with ReaderLocked, corrupting the vtable.
// When the write resolves, pipeLoop's source.tryGetErrored() would
// virtual-call through the corrupted vtable → SIGSEGV.
//
// Fixed by not transitioning PipeLocked → Unlocked in onError().
// The pipe loop detects the error via source.tryGetErrored() and
// releases properly.
export const pipeErrorDestroysPipeLockedDuringWrite = {
  async test() {
    const id = new IdentityTransformStream();
    const idReader = id.readable.getReader();

    let readableController;
    const readable = new ReadableStream({
      start(c) {
        readableController = c;
        c.enqueue(new Uint8Array([1, 2, 3]));
        c.enqueue(new Uint8Array([4, 5, 6]));
      },
    });

    async function doTest() {
      // When the first chunk arrives at id.readable, error the source.
      // Post-fix: PipeLocked stays alive, readable remains locked.
      await idReader.read();
      readableController.error(new Error('boom'));
      strictEqual(
        readable.locked,
        true,
        'readable should remain locked after error while pipe is active'
      );
      // Consume second chunk to unblock write and let pipeLoop continue.
      await rejects(idReader.read(), { message: /boom/ });
    }
    const promise = doTest();

    const pipePromise = rejects(
      readable.pipeTo(id.writable, { preventClose: true }),
      {
        message: /boom/,
      }
    );

    await Promise.all([promise, pipePromise]);
  },
};
