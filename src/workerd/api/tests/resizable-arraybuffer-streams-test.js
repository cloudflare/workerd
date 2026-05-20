// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { ok, rejects, strictEqual, throws } from 'node:assert';

function createResizableBuffer(size, maxSize = size) {
  return new ArrayBuffer(size, { maxByteLength: maxSize || size });
}

function installThenableTrap(fn) {
  let armed = true;
  Object.defineProperty(Object.prototype, 'then', {
    configurable: true,
    get() {
      if (armed) {
        armed = false;
        fn();
      }
      return undefined;
    },
  });
  return () => {
    delete Object.prototype.then;
  };
}

export const byobResizeToZeroInPullBeforeRespond = {
  async test() {
    const rab = createResizableBuffer(1024, 1024);

    let pullCalled = false;

    const rs = new ReadableStream({
      type: 'bytes',
      start() {},
      pull(c) {
        pullCalled = true;
        // The original rab is detached by reader.read() (compat flag).
        // Access the transferred buffer via byobRequest.view.buffer.
        const buf = c.byobRequest.view.buffer;
        ok(buf.resizable, 'Transferred buffer should be resizable');

        // Resize the transferred buffer to 0 before responding
        buf.resize(0);

        // Attempt to respond — should throw due to resized buffer, not SIGSEGV
        throws(() => c.byobRequest.respond(10), {
          message: 'Cannot respond with a zero-length or detached view',
        });

        c.close();
      },
    });

    const reader = rs.getReader({ mode: 'byob' });
    await reader.read(new Uint8Array(rab));
    ok(pullCalled, 'pull was called');
  },
};

export const byobResizeSmallerThanFilledInPull = {
  async test() {
    const rab = createResizableBuffer(1024, 1024);
    let pullCount = 0;

    const rs = new ReadableStream({
      type: 'bytes',
      start() {},
      pull(c) {
        pullCount++;
        if (pullCount === 1) {
          // First pull: enqueue some bytes to partially fill
          c.enqueue(new Uint8Array(100).fill(0x41));
        } else if (pullCount === 2) {
          // Second pull: resize transferred buffer smaller than what's been filled.
          // The original rab is detached; access via byobRequest.view.buffer.
          const buf = c.byobRequest.view.buffer;
          buf.resize(50); // Smaller than the 100 bytes already filled

          // Attempt to respond — should throw, not SIGSEGV
          throws(() => c.byobRequest.respond(10), {
            message: 'Cannot respond with a zero-length or detached view',
          });

          c.close();
        }
      },
    });

    const reader = rs.getReader({ mode: 'byob' });
    await reader.read(new Uint8Array(rab), { min: 200 });
    ok(pullCount >= 2, 'pull was called at least twice');
  },
};

export const byobResizeViaThenableDuringRespond = {
  async test() {
    const rab = createResizableBuffer(1024, 1024);
    let resizeBlocked = false;

    const rs = new ReadableStream({
      type: 'bytes',
      pull(c) {
        const view = c.byobRequest.view;
        view.fill(0x42);

        // Capture the transferred buffer before installing the trap.
        // The original rab is detached; view.buffer is the live one.
        const buf = view.buffer;
        ok(buf.resizable);

        // Install thenable trap that attempts resize during resolve.
        // The fix detaches the view's buffer before resolving, so
        // resize() should throw TypeError. We must catch inside the
        // getter — errors escaping thenable getters corrupt V8's
        // promise resolution.
        const cleanup = installThenableTrap(() => {
          throws(() => buf.resize(0), {
            message: /detached/,
          });
          resizeBlocked = true;
        });

        try {
          c.byobRequest.respond(view.byteLength);
        } finally {
          cleanup();
        }
      },
    });

    const reader = rs.getReader({ mode: 'byob' });
    await reader.read(new Uint8Array(rab));
    ok(resizeBlocked, 'Resize was blocked by detach');
  },
};

export const byobTeeResizeViaThenableDuringPush = {
  async test() {
    const rab = createResizableBuffer(1024, 1024);

    const rs = new ReadableStream({
      type: 'bytes',
      start() {},
      pull(c) {
        if (!c.byobRequest) return;
        const view = c.byobRequest.view;
        view.fill(0x43);

        // Capture the transferred buffer before respond.
        const buf = view.buffer;
        ok(buf.resizable);

        // Install thenable trap that attempts resize during respond.
        // The resize may or may not be blocked depending on whether
        // the thenable fires before or after our preResolve detach.
        const cleanup = installThenableTrap(() => {
          throws(() => buf.resize(0), {
            message: /detached/,
          });
        });

        try {
          c.byobRequest.respond(Math.min(view.byteLength, 16));
        } finally {
          cleanup();
        }
      },
    });

    // Tee creates two consumers — respond() will push to the other branch
    const [branch1, branch2] = rs.tee();

    const reader1 = branch1.getReader({ mode: 'byob' });
    const reader2 = branch2.getReader();

    // Start reads on both branches — either may throw or succeed.
    // The key assertion: no SIGSEGV regardless of thenable timing.
    await Promise.all([reader1.read(new Uint8Array(rab)), reader2.read()]);
  },
};

export const byobResizeAfterSuccessfulReadThenReadAgain = {
  async test() {
    const rab = createResizableBuffer(1024, 1024);

    const rs = new ReadableStream({
      type: 'bytes',
      start() {},
      pull(c) {
        const view = c.byobRequest.view;
        if (view.byteLength > 0) {
          view.fill(0x44);
          c.byobRequest.respond(view.byteLength);
        } else {
          c.close();
        }
      },
    });

    const reader = rs.getReader({ mode: 'byob' });
    // First read succeeds. The original rab is detached by reader.read().
    // The result contains a view over the transferred buffer.
    const first = await reader.read(new Uint8Array(rab));
    ok(!first.done, 'First read returned data');

    // Resize the result's buffer to 0. This is the transferred buffer,
    // not the original rab (which is detached).
    const resultBuf = first.value.buffer;
    resultBuf.resize(0);

    // Second read with a new resizable buffer. The previous result's
    // buffer was resized to 0 but that doesn't affect this new read.
    // Verify no crash.
    const rab2 = createResizableBuffer(64, 64);
    const second = await reader.read(new Uint8Array(rab2));
    ok(!second.done, 'Second read completed');
    ok(second.value.byteLength > 0, 'Second read returned data');
  },
};

export const byobResizeDuringHandlePushResolve = {
  async test() {
    const rab = createResizableBuffer(1024, 1024);
    let ctrl;

    const pullCalled = Promise.withResolvers();

    const rs = new ReadableStream({
      type: 'bytes',
      start(c) {
        ctrl = c;
      },
      pull(c) {
        // Don't respond — let the read stay pending
        pullCalled.resolve();
      },
    });

    const reader = rs.getReader({ mode: 'byob' });
    const readPromise = reader.read(new Uint8Array(rab));

    // Let pull() be called
    await pullCalled.promise;

    // Install thenable trap that attempts resize during the read's
    // resolution. The original rab is detached, so resize throws.
    const cleanup = installThenableTrap(() => {
      throws(() => rab.resize(0), {
        message: /detached/,
      });
    });

    try {
      // Enqueue enough data to fulfill the read. handlePush will:
      // 1. Copy data from entry to BYOB view
      // 2. Resolve the read promise → thenable check → resize attempt
      ctrl.enqueue(new Uint8Array(1024).fill(0x45));
    } finally {
      cleanup();
    }

    const result = await readPromise;
    ok(!result.done, 'Read completed with data');
  },
};

export const respondWithNewViewResizeViaThenable = {
  async test() {
    const rab = createResizableBuffer(1024, 1024);

    const rs = new ReadableStream({
      type: 'bytes',
      start() {},
      pull(c) {
        const view = c.byobRequest.view;
        const buf = view.buffer;

        const newView = new Uint8Array(buf, view.byteOffset, view.byteLength);
        newView.fill(0x4b);

        // respondWithNewView detaches buf via detachAndTake before resolve.
        // The thenable trap attempts resize on the now-detached buffer.
        const cleanup = installThenableTrap(() => {
          throws(() => buf.resize(0), {
            message: /detached/,
          });
        });

        try {
          c.byobRequest.respondWithNewView(newView);
        } finally {
          cleanup();
        }
      },
    });

    const reader = rs.getReader({ mode: 'byob' });
    await reader.read(new Uint8Array(rab));
  },
};

export const enqueueResizableBufferDetachesCorrectly = {
  async test() {
    let ctrl;
    const rs = new ReadableStream({
      type: 'bytes',
      start(c) {
        ctrl = c;
      },
      pull() {},
    });

    const reader = rs.getReader();

    const rab = createResizableBuffer(64, 128);
    const view = new Uint8Array(rab);
    view.fill(0x4c);

    ctrl.enqueue(view);

    strictEqual(rab.byteLength, 0, 'Buffer should be detached after enqueue');

    // Resizing a detached buffer must throw TypeError
    throws(() => rab.resize(128), TypeError);

    const result = await reader.read();
    ok(!result.done, 'Read should return data');
    ok(true, 'No SIGSEGV');
  },
};

export const byobResizeToZeroWhileReadPending = {
  async test() {
    const rab = createResizableBuffer(1024, 1024);

    const rs = new ReadableStream({
      type: 'bytes',
      async pull(c) {
        // The original rab is detached by reader.read(). Access the
        // pending read's buffer via byobRequest.
        const buf = c.byobRequest.view.buffer;
        ok(buf.resizable, 'Pending read buffer is resizable');
        buf.resize(0);

        // Enqueue data — handlePush should detect the resized buffer
        // and not crash. The enqueue may throw due to the resized buffer.
        throws(() => c.enqueue(new Uint8Array(64).fill(0x4d)), {
          message: /The byobRequest.view is zero-length or was detached/,
        });
        c.close();
      },
    });

    const reader = rs.getReader({ mode: 'byob' });
    const readPromise = reader.read(new Uint8Array(rab));
    throws(() => rab.resize(0), {
      message: /detached/,
    });

    await Promise.allSettled([readPromise]);
    ok(true, 'No SIGSEGV');
  },
};

export const byobCloseResizeViaThenableOnClose = {
  async test() {
    const rab = createResizableBuffer(65536, 65536);
    let ctrl;
    let buf;

    const rs = new ReadableStream({
      type: 'bytes',
      start(c) {
        ctrl = c;
      },
      pull(c) {
        if (c.byobRequest) {
          // Capture the transferred buffer for the thenable trap
          buf = c.byobRequest.view.buffer;
        }
      },
    });

    const reader = rs.getReader({ mode: 'byob' });
    const readPromise = reader.read(new Uint8Array(rab), { min: 65536 });

    await Promise.resolve();
    await Promise.resolve();

    ok(buf !== undefined, 'pull was called with byobRequest');
    ok(buf.resizable, 'Transferred buffer is resizable');

    // Partially fill
    ctrl.enqueue(new Uint8Array(100).fill(0x4e));

    // Install thenable trap that resizes the transferred buffer
    // during close's resolve path
    const cleanup = installThenableTrap(() => {
      buf.resize(0);
    });

    try {
      ctrl.close();
    } finally {
      cleanup();
    }

    await rejects(readPromise, {
      message: /Cannot perform ArrayBuffer.prototype.resize/,
    });
    ok(true, 'No SIGSEGV');
  },
};
