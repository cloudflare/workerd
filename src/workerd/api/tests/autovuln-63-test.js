// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { ok, rejects } from 'node:assert';

// Regression test for AUTOVULN-CLOUDFLARE-WORKERD-63.
// algorithms.clear() can be called re-entrantly while an algorithm function
// is still executing on the stack (e.g., via toString() re-entry in
// TextEncoderStream, or via controller.error() from inside pull/write/cancel).
// The InUseGuard on each Algorithms struct defers the clear until the
// algorithm invocation returns, preventing the jsg::Function (and its
// captured closure) from being freed mid-execution.

// Transform stream: TextEncoderStream transform algorithm freed via cancel
// during toString() re-entry.
export const transformAlgorithmFreedViaCancelDuringToString = {
  async test() {
    const { writable, readable } = new TextEncoderStream();
    const writer = writable.getWriter();
    const reader = readable.getReader();

    const readPromise = reader.read();

    let toStringCalled = false;

    const writePromise = rejects(
      writer.write({
        toString() {
          toStringCalled = true;
          reader.cancel(new Error('boom'));
          return 'hello after free';
        },
      }),
      {
        message: /The readable side/,
      }
    );

    await Promise.all([readPromise, writePromise]);

    ok(toStringCalled, 'toString should have been called');
  },
};

// Readable stream: pull algorithm calls controller.error() re-entrantly,
// which calls algorithms.clear() while pull is still on the stack.
export const readablePullAlgorithmFreedViaError = {
  async test() {
    let pullCalled = false;

    const rs = new ReadableStream({
      pull(controller) {
        pullCalled = true;
        // Re-entrant error during pull clears algorithms.
        controller.error(new Error('pull-error'));
      },
    });

    const reader = rs.getReader();
    await rejects(reader.read(), {
      message: 'pull-error',
    });

    ok(pullCalled, 'pull should have been called');
  },
};

// Readable stream: cancel algorithm triggers controller.error() which
// clears algorithms while cancel is still executing.
export const readableCancelAlgorithmFreedViaError = {
  async test() {
    let cancelCalled = false;
    let ctrl;

    const rs = new ReadableStream({
      start(c) {
        ctrl = c;
      },
      cancel(_reason) {
        cancelCalled = true;
        // Re-entrant error during cancel clears algorithms.
        ctrl.error(new Error('cancel-error'));
      },
    });

    const reader = rs.getReader();
    await reader.cancel('test');

    ok(cancelCalled, 'cancel should have been called');
  },
};

// Writable stream: write algorithm triggers controller.error() which
// clears algorithms while write is still executing.
export const writableWriteAlgorithmFreedViaError = {
  async test() {
    let writeCalled = false;

    const ws = new WritableStream({
      write(_chunk, controller) {
        writeCalled = true;
        controller.error(new Error('write-error'));
      },
    });

    const writer = ws.getWriter();
    await writer.write('data');
    ok(writeCalled, 'write should have been called');
  },
};

// Writable stream: abort algorithm triggers re-entrant state change
// that clears algorithms while abort is still executing.
export const writableAbortAlgorithmFreedViaError = {
  async test() {
    let abortCalled = false;

    const ws = new WritableStream({
      abort(_reason) {
        abortCalled = true;
        // The abort algorithm itself is executing — if algorithms.clear()
        // is called re-entrantly, the InUseGuard defers it.
      },
    });

    const writer = ws.getWriter();
    await writer.abort(new Error('abort-reason'));

    ok(abortCalled, 'abort should have been called');
  },
};
