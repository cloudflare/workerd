// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The adapters under prototype pollution. Unlike Node's, which use
// primordials, these adapters call the live Promise.prototype.then (and
// Promise.all, withResolvers, finally): a patched then sees every hop, a
// thenable getter on Object.prototype is consulted when plain objects are
// assimilated. What is guaranteed: data still flows through a transparent
// patch, and a then that throws during construction leaves the web stream
// unlocked — the caller gets the throw, not a stream nobody can use.

import { Readable, Writable, Duplex } from 'node:stream';
import { Buffer } from 'node:buffer';
import { strictEqual, deepStrictEqual, throws, ok } from 'node:assert';
import { withUncaughtGuard } from 'helpers';

// Runs fn with Promise.prototype.then replaced, restoring it afterwards.
async function withPatchedThen(patch, fn) {
  const original = Promise.prototype.then;
  const state = { calls: 0 };
  Promise.prototype.then = function (onFulfilled, onRejected) {
    state.calls++;
    return patch.call(this, original, onFulfilled, onRejected);
  };
  try {
    return await fn(state);
  } finally {
    Promise.prototype.then = original;
  }
}

// A transparent then (counting its calls): every adapter still moves its
// data, and each hop is visible to the patch.
export const patchedThenPassthroughKeepsData = {
  async test() {
    await withPatchedThen(
      function (original, onFulfilled, onRejected) {
        return original.call(this, onFulfilled, onRejected);
      },
      async (state) => {
        const rs = new ReadableStream({
          start(c) {
            c.enqueue(new Uint8Array([1, 2]));
            c.enqueue(new Uint8Array([3]));
            c.close();
          },
        });
        const chunks = [];
        for await (const chunk of Readable.fromWeb(rs)) chunks.push(...chunk);
        deepStrictEqual(chunks, [1, 2, 3]);

        const sunk = [];
        const w = Writable.fromWeb(
          new WritableStream({
            write(chunk) {
              sunk.push(...chunk);
            },
          })
        );
        await new Promise((resolve, reject) => {
          w.on('finish', resolve);
          w.on('error', reject);
          w.write(new Uint8Array([9]));
          w.end(new Uint8Array([8]));
        });
        deepStrictEqual(sunk, [9, 8]);

        const text = await new Response(
          Readable.toWeb(Readable.from([Buffer.from('xy')]))
        ).text();
        strictEqual(text, 'xy');
        ok(state.calls > 0, 'the patched then was never called');
      }
    );
  },
};

// A then that throws once: the constructor's own use of it throws out of
// fromWeb — with the web stream left unlocked, so the caller can still
// use it — for each of the three adapters.
export const hostileThenDuringFromWebLeavesStreamUnlocked = {
  async test() {
    const hostile = () => {
      let armed = true;
      return function (original, onFulfilled, onRejected) {
        if (armed) {
          armed = false;
          throw new Error('hostile then');
        }
        return original.call(this, onFulfilled, onRejected);
      };
    };

    const rs = new ReadableStream({
      start(c) {
        c.enqueue(new Uint8Array([1]));
        c.close();
      },
    });
    await withPatchedThen(hostile(), async () => {
      throws(() => Readable.fromWeb(rs), { message: 'hostile then' });
    });
    strictEqual(rs.locked, false);
    const chunks = [];
    for await (const chunk of Readable.fromWeb(rs)) chunks.push(...chunk);
    deepStrictEqual(chunks, [1]);

    const sunk = [];
    const ws = new WritableStream({
      write(chunk) {
        sunk.push(...chunk);
      },
    });
    await withPatchedThen(hostile(), async () => {
      throws(() => Writable.fromWeb(ws), { message: 'hostile then' });
    });
    strictEqual(ws.locked, false);
    const w = Writable.fromWeb(ws);
    await new Promise((resolve) => w.end(new Uint8Array([2]), resolve));
    deepStrictEqual(sunk, [2]);

    const pair = {
      readable: new ReadableStream({
        start(c) {
          c.enqueue(new Uint8Array([3]));
          c.close();
        },
      }),
      writable: new WritableStream({
        write(chunk) {
          sunk.push(...chunk);
        },
      }),
    };
    await withPatchedThen(hostile(), async () => {
      throws(() => Duplex.fromWeb(pair), { message: 'hostile then' });
    });
    strictEqual(pair.readable.locked, false);
    strictEqual(pair.writable.locked, false);
    const d = Duplex.fromWeb(pair);
    d.end(new Uint8Array([4]));
    for await (const chunk of d) chunks.push(...chunk);
    deepStrictEqual(chunks, [1, 3]);
    deepStrictEqual(sunk, [2, 4]);
  },
};

// A then that registers the handlers it was given and THEN throws: the
// throw still comes out of fromWeb and the web stream is left unlocked and
// usable — and the node stream nobody received is destroyed quietly before
// the lock is released, so the release, which rejects the reader's or
// writer's closed promise into the handlers just registered, has nothing
// left to destroy: no 'error' escapes from a stream the caller could never
// listen to. For each of the three adapters.
export const hostileThenAfterRegisteringLeavesNothingBehind = {
  async test() {
    await withUncaughtGuard(async () => {
      const registering = () => {
        let armed = true;
        return function (original, onFulfilled, onRejected) {
          if (armed) {
            armed = false;
            original.call(this, onFulfilled, onRejected);
            throw new Error('hostile then');
          }
          return original.call(this, onFulfilled, onRejected);
        };
      };

      const rs = new ReadableStream({
        start(c) {
          c.enqueue(new Uint8Array([1]));
          c.close();
        },
      });
      await withPatchedThen(registering(), async () => {
        throws(() => Readable.fromWeb(rs), { message: 'hostile then' });
      });
      strictEqual(rs.locked, false);
      const chunks = [];
      for await (const chunk of Readable.fromWeb(rs)) chunks.push(...chunk);
      deepStrictEqual(chunks, [1]);

      const sunk = [];
      const ws = new WritableStream({
        write(chunk) {
          sunk.push(...chunk);
        },
      });
      await withPatchedThen(registering(), async () => {
        throws(() => Writable.fromWeb(ws), { message: 'hostile then' });
      });
      strictEqual(ws.locked, false);
      const w = Writable.fromWeb(ws);
      await new Promise((resolve) => w.end(new Uint8Array([2]), resolve));
      deepStrictEqual(sunk, [2]);

      const pair = {
        readable: new ReadableStream({
          start(c) {
            c.enqueue(new Uint8Array([3]));
            c.close();
          },
        }),
        writable: new WritableStream({
          write(chunk) {
            sunk.push(...chunk);
          },
        }),
      };
      await withPatchedThen(registering(), async () => {
        throws(() => Duplex.fromWeb(pair), { message: 'hostile then' });
      });
      strictEqual(pair.readable.locked, false);
      strictEqual(pair.writable.locked, false);
      const d = Duplex.fromWeb(pair);
      d.end(new Uint8Array([4]));
      for await (const chunk of d) chunks.push(...chunk);
      deepStrictEqual(chunks, [1, 3]);
      deepStrictEqual(sunk, [2, 4]);
    });
  },
};

// A `then` getter on Object.prototype: consulted whenever a plain object
// is assimilated into a promise (the reader's { value, done } results
// among them); returning undefined, it changes nothing about the data.
export const objectPrototypeThenGetterIsConsultedNotObeyed = {
  async test() {
    let fired = 0;
    Object.defineProperty(Object.prototype, 'then', {
      get() {
        fired++;
        return undefined;
      },
      configurable: true,
    });
    try {
      const rs = new ReadableStream({
        start(c) {
          c.enqueue(new Uint8Array([7]));
          c.close();
        },
      });
      const chunks = [];
      for await (const chunk of Readable.fromWeb(rs)) chunks.push(...chunk);
      deepStrictEqual(chunks, [7]);
      ok(fired > 0, 'the getter was never consulted');
    } finally {
      delete Object.prototype.then;
    }
  },
};
