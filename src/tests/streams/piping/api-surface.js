// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// pipeTo/pipeThrough argument plumbing: brand checks, option getter
// evaluation, and validation failures — all BEFORE any locks are taken.

import { strictEqual, rejects, deepStrictEqual, throws } from 'node:assert';
import { usingTsImpl } from 'which-impl';

// The four pipe options are read once each, in spec order, before the
// pipe starts.
export const optionGetterReadOrder = {
  async test() {
    const reads = [];
    const opts = {};
    for (const k of [
      'preventAbort',
      'preventCancel',
      'preventClose',
      'signal',
    ]) {
      Object.defineProperty(opts, k, {
        get() {
          reads.push(k);
          return undefined;
        },
        enumerable: true,
      });
    }
    const rs = new ReadableStream({
      start(c) {
        c.close();
      },
    });
    await rs.pipeTo(new WritableStream({}), opts);
    deepStrictEqual(reads, [
      'preventAbort',
      'preventCancel',
      'preventClose',
      'signal',
    ]);
  },
};

// A throwing option getter rejects the pipe with that very error and
// leaves both streams unlocked.
export const throwingOptionGetter = {
  async test() {
    const gerr = new Error('getter-err');
    const rs = new ReadableStream({});
    const ws = new WritableStream({});
    let caught;
    await rs
      .pipeTo(ws, {
        get preventAbort() {
          throw gerr;
        },
      })
      .catch((e) => (caught = e));
    strictEqual(caught, gerr);
    strictEqual(rs.locked, false);
    strictEqual(ws.locked, false);
  },
};

// A non-AbortSignal signal option rejects TypeError without locking.
export const invalidSignalRejected = {
  async test() {
    const rs = new ReadableStream({});
    await rejects(rs.pipeTo(new WritableStream({}), { signal: 'nope' }), {
      name: 'TypeError',
    });
    strictEqual(rs.locked, false);
  },
};

// Brand checks: pipeTo on a non-stream this, a non-WritableStream
// destination, and pipeThrough with a bad pair. DIVERGENCE (the WPT
// general.any brand seed): C++ THROWS the TypeError synchronously from
// pipeTo, TypeScript returns a rejected promise (spec).
export const brandChecks = {
  async test() {
    const ws = new WritableStream({});
    const rs = new ReadableStream({});
    if (usingTsImpl) {
      await rejects(ReadableStream.prototype.pipeTo.call({}, ws), {
        name: 'TypeError',
      });
      await rejects(rs.pipeTo({}), { name: 'TypeError' });
    } else {
      // Even with capture_async_api_throws pinned, the broken-brand
      // `this` fails before the capture wrapper: synchronous throw. A
      // real stream with a bad destination rejects.
      throws(() => ReadableStream.prototype.pipeTo.call({}, ws), {
        name: 'TypeError',
      });
      await rejects(rs.pipeTo({}), { name: 'TypeError' });
    }
    strictEqual(rs.locked, false);
    throws(() => rs.pipeThrough({}), { name: 'TypeError' });
    strictEqual(rs.locked, false);
  },
};

// pipeThrough throws synchronously on locked endpoints without
// disturbing the other side.
export const pipeThroughLockedEndpoints = {
  test() {
    const rs = new ReadableStream({});
    rs.getReader();
    const t = new TransformStream();
    throws(() => rs.pipeThrough(t), { name: 'TypeError' });
    const rs2 = new ReadableStream({});
    const t2 = new TransformStream();
    t2.writable.getWriter();
    throws(() => rs2.pipeThrough(t2), { name: 'TypeError' });
    strictEqual(rs2.locked, false);
  },
};

// A TypeError without internal names (`#writer`), and no lock taken.
const cleanTypeError = (e) => e.name === 'TypeError' && !/#/.test(e.message);

// A non-WritableStream destination fails before the source is locked.
export const badDestinationLeavesSourceUnlocked = {
  async test() {
    const rs = new ReadableStream({});
    throws(
      () => rs.pipeThrough({ readable: new ReadableStream(), writable: {} }),
      cleanTypeError
    );
    strictEqual(rs.locked, false);
    await rejects(rs.pipeTo({}), cleanTypeError);
    strictEqual(rs.locked, false);
  },
};

// Options are read before the locked checks, so an option getter that
// locks the destination fails the pipe without locking the source.
export const optionGetterLocksDestination = {
  async test() {
    const rs = new ReadableStream({});
    const t = new TransformStream();
    throws(
      () =>
        rs.pipeThrough(t, {
          get preventAbort() {
            t.writable.getWriter();
            return false;
          },
        }),
      cleanTypeError
    );
    strictEqual(rs.locked, false);
    const ws = new WritableStream({});
    await rejects(
      rs.pipeTo(ws, {
        get preventAbort() {
          ws.getWriter();
          return false;
        },
      }),
      cleanTypeError
    );
    strictEqual(rs.locked, false);
  },
};

// pipeThrough checks the writable's lock internally, not through its
// user-visible `locked`.
export const shadowedWritableLocked = {
  test() {
    const rs = new ReadableStream({});
    const t = new TransformStream();
    t.writable.getWriter();
    Object.defineProperty(t.writable, 'locked', { value: false });
    throws(() => rs.pipeThrough(t), cleanTypeError);
    strictEqual(rs.locked, false);
  },
};
