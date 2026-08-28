// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Re-entrancy edges: user code that runs in the middle of digest
// processing and re-enters the API. Write resolutions carry undefined (no
// thenable check), and the digest promise resolves with an ArrayBuffer —
// an object — so a patched Object.prototype.then getter fires exactly once
// against it in both implementations, but at DIFFERENT stages: TypeScript
// resolves the digest deferred inside close() (the check fires while close
// settles), while C++ surfaces it when the digest promise is awaited. The
// constructor's options bag is the one user hook on the construction path.

import { strictEqual, deepStrictEqual } from 'node:assert';
import { usingTsImpl } from 'which-impl';
import { digestOf } from 'digest-vectors';

async function withThenInterceptor(onGet, fn) {
  Object.defineProperty(Object.prototype, 'then', {
    get() {
      onGet();
      return undefined;
    },
    configurable: true,
  });
  try {
    await fn();
  } finally {
    delete Object.prototype.then;
  }
  strictEqual('then' in {}, false, 'interceptor must be removed');
}

export const thenInterceptionDuringDigestResolution = {
  async test() {
    let fired = 0;
    await withThenInterceptor(
      () => fired++,
      async () => {
        const stream = new crypto.DigestStream('md5');
        const writer = stream.getWriter();
        await writer.write('ab');
        strictEqual(fired, 0, 'write resolutions carry undefined');
        await writer.close();
        strictEqual(fired, usingTsImpl ? 1 : 0, 'close-time check');
        fired = 0;
        strictEqual((await stream.digest).byteLength, 16);
        strictEqual(fired, usingTsImpl ? 0 : 1, 'await-time check');
      }
    );
  },
};

export const writeFromWriteContinuation = {
  async test() {
    // Issuing the next write from inside the previous write's continuation
    // re-enters the sink mid-settlement; accumulation order must hold.
    const stream = new crypto.DigestStream('md5');
    const writer = stream.getWriter();
    let closePromise;
    await writer.write('hel').then(() =>
      writer.write('lo').then(() => {
        closePromise = writer.close();
      })
    );
    await closePromise;
    deepStrictEqual(
      new Uint8Array(await stream.digest),
      await digestOf('md5', 'hello')
    );
    strictEqual(stream.bytesWritten, 5n);
  },
};

export const optionGetterReentersConstructor = {
  async test() {
    // The options bag getter runs user code inside the constructor;
    // constructing (and disposing) another DigestStream from there is safe
    // and the outer construction still observes the returned value.
    let reentered = false;
    const stream = new crypto.DigestStream('md5', {
      get toWellFormed() {
        const inner = new crypto.DigestStream('md5');
        inner[Symbol.dispose]();
        reentered = true;
        return true;
      },
    });
    strictEqual(reentered, true);
    // The outer stream honors the getter's value (U+FFFD substitution).
    const writer = stream.getWriter();
    await writer.write('\uD800');
    await writer.close();
    deepStrictEqual(
      new Uint8Array(await stream.digest),
      await digestOf('md5', new TextEncoder().encode('\uD800'))
    );
  },
};
