// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Regression test: when a JavaScript-backed ReadableStream errors while it is
// piped into a WritableStream, the pipe must not report an unhandled rejection.
// The caller handles the pipeTo() promise and every other promise it can see,
// so no unhandledrejection event may fire.
//
// The pipe loops used to return promises that were already rejected from their
// promise continuations. V8 attaches the handler that adopts such a promise
// only in a later microtask, so when unhandled rejections are processed in a
// microtask (see the .wd-test's compatibility flags) the promise was reported
// first. This covers the loop for JavaScript-backed writables (DigestStream is
// one) and the loop for internal ones (IdentityTransformStream), and internal
// sources as well as JavaScript-backed ones.

import { deepStrictEqual, rejects } from 'node:assert';

// A byte source that delivers one chunk and errors on the next pull, so the
// error is seen by the pipe loop after a successful read and write.
function erroringByteSource() {
  let sent = false;
  return new ReadableStream({
    type: 'bytes',
    pull(c) {
      if (sent) {
        c.error(new Error('source failed'));
        return;
      }
      sent = true;
      c.enqueue(new Uint8Array(1024));
    },
  });
}

// Same as above, but a default (non-byte) source.
function erroringDefaultSource() {
  let sent = false;
  return new ReadableStream({
    pull(c) {
      if (sent) {
        c.error(new Error('source failed'));
        return;
      }
      sent = true;
      c.enqueue(new Uint8Array(1024));
    },
  });
}

function newDigestStream() {
  const d = new crypto.DigestStream('MD5');
  d.digest.catch(() => {});
  return d;
}

async function expectNoUnhandledRejection(fn) {
  const reasons = [];
  const handler = (event) => {
    reasons.push(String(event.reason?.stack ?? event.reason));
    event.preventDefault();
  };
  addEventListener('unhandledrejection', handler);
  try {
    await fn();
    // Unhandled rejections are reported once the microtask queue drains, before
    // the event loop's next turn, so one turn is enough.
    await scheduler.wait(0);
    deepStrictEqual(reasons, []);
  } finally {
    removeEventListener('unhandledrejection', handler);
  }
}

export const byteSourceIntoDigestStream = {
  async test() {
    await expectNoUnhandledRejection(async () => {
      await rejects(erroringByteSource().pipeTo(newDigestStream()), {
        message: 'source failed',
      });
    });
  },
};

export const defaultSourceIntoDigestStream = {
  async test() {
    await expectNoUnhandledRejection(async () => {
      await rejects(erroringDefaultSource().pipeTo(newDigestStream()), {
        message: 'source failed',
      });
    });
  },
};

export const byteSourceIntoIdentityTransformStream = {
  async test() {
    await expectNoUnhandledRejection(async () => {
      const { readable, writable } = new IdentityTransformStream();
      const drained = readable.pipeTo(new WritableStream()).catch(() => {});
      await rejects(erroringByteSource().pipeTo(writable), {
        message: 'source failed',
      });
      await drained;
    });
  },
};

export const teedByteSourceIntoDigestStreams = {
  async test() {
    await expectNoUnhandledRejection(async () => {
      const [a, b] = erroringByteSource().tee();
      const results = await Promise.allSettled([
        a.pipeTo(newDigestStream()),
        b.pipeTo(newDigestStream()),
      ]);
      deepStrictEqual(
        results.map((r) => r.status),
        ['rejected', 'rejected']
      );
    });
  },
};

// An internal (native) source that errors while piped into a JavaScript-backed
// writable: the IdentityTransformStream's writable side is aborted mid-pipe.
async function abortedNativeSourceIntoDigestStream(options) {
  const { readable, writable } = new IdentityTransformStream();
  const piped = rejects(readable.pipeTo(newDigestStream(), options), {
    message: 'source aborted',
  });
  const writer = writable.getWriter();
  await writer.write(new Uint8Array(10));
  await writer.abort(new Error('source aborted'));
  await piped;
}

export const nativeSourceIntoDigestStream = {
  async test() {
    await expectNoUnhandledRejection(() =>
      abortedNativeSourceIntoDigestStream()
    );
  },
};

export const nativeSourceIntoDigestStreamPreventAbort = {
  async test() {
    await expectNoUnhandledRejection(() =>
      abortedNativeSourceIntoDigestStream({ preventAbort: true })
    );
  },
};
