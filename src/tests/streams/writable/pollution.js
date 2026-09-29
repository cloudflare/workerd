// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Prototype pollution the writable machinery must not observe:
// Object.prototype resolvers around the writer's ready/closed promises, and
// Object.prototype members standing in for omitted dictionary arguments. The
// C++ implementation reads none of these; the TypeScript one must not
// either. Every mutation is undone before any assertion runs.

import { strictEqual, deepStrictEqual, ok } from 'node:assert';

function pollute(entries) {
  for (const key of Object.keys(entries)) {
    Object.defineProperty(Object.prototype, key, {
      value: entries[key],
      configurable: true,
      writable: true,
    });
  }
  return () => {
    for (const key of Object.keys(entries)) {
      Reflect.deleteProperty(Object.prototype, key);
    }
  };
}

// The writer's ready and closed promises alternate between a pending
// resolvers record and a settled promise; Object.prototype resolvers must
// never be mistaken for the record's. Releasing a writer whose ready and
// closed promises have both settled exercises the settled branch.
export const pollutedResolversDoNotReachWriterPromises = {
  async test() {
    let polluted = 0;
    const restore = pollute({
      resolve() {
        polluted++;
      },
      reject() {
        polluted++;
      },
      promise: 'polluted',
    });
    const desired = [];
    let readyAfterRelease;
    let closedAfterRelease;
    try {
      let releaseWrite;
      const ws = new WritableStream({
        __proto__: null,
        write() {
          return new Promise((resolve) => {
            releaseWrite = resolve;
          });
        },
      });
      const writer = ws.getWriter();
      await writer.ready;
      for (let i = 0; i < 2; i++) {
        const write = writer.write('x');
        desired.push(writer.desiredSize);
        releaseWrite();
        await write;
        await writer.ready;
        desired.push(writer.desiredSize);
      }
      await writer.close();
      writer.releaseLock();
      readyAfterRelease = writer.ready;
      closedAfterRelease = writer.closed;
    } finally {
      restore();
    }
    strictEqual('promise' in {}, false, 'pollution must be removed');
    strictEqual(polluted, 0);
    deepStrictEqual(desired, [0, 1, 0, 1]);
    ok(readyAfterRelease instanceof Promise);
    ok(closedAfterRelease instanceof Promise);
    await Promise.allSettled([readyAfterRelease, closedAfterRelease]);
  },
};

// An omitted dictionary argument is an empty dictionary: nothing is read
// from Object.prototype for it.
export const omittedDictionariesReadNothing = {
  async test() {
    let pollutedStarts = 0;
    const restore = pollute({
      type: 'bytes',
      start() {
        pollutedStarts++;
      },
      size() {
        return 100;
      },
      highWaterMark: 7,
    });
    let bareLocked;
    let desiredBefore;
    let desiredAfter;
    try {
      bareLocked = new WritableStream().locked;

      let releaseWrite;
      const ws = new WritableStream({
        __proto__: null,
        write() {
          return new Promise((resolve) => {
            releaseWrite = resolve;
          });
        },
      });
      const writer = ws.getWriter();
      await writer.ready;
      desiredBefore = writer.desiredSize;
      const write = writer.write('x');
      desiredAfter = writer.desiredSize;
      releaseWrite();
      await write;
    } finally {
      restore();
    }
    strictEqual('type' in {}, false, 'pollution must be removed');
    strictEqual(pollutedStarts, 0);
    strictEqual(bareLocked, false);
    strictEqual(desiredBefore, 1);
    strictEqual(desiredAfter, 0);
  },
};
