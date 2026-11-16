// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Either implementation writes a Blob in the C++ Blob's format (tag 15: the
// type, then the bytes), so a reader running either reads it; the bytes are
// pinned in api/blob-bootstrap-test.c++.

import { strictEqual, throws } from 'node:assert';
import { env } from 'cloudflare:workers';

const dataCloneError = (type) => ({
  name: 'DataCloneError',
  message: `Could not serialize object of type "${type}". This type does not support serialization.`,
});

async function checkClone(clone) {
  strictEqual(Object.getPrototypeOf(clone), Blob.prototype);
  strictEqual(clone.size, 2);
  strictEqual(clone.type, 'a/b');
  strictEqual(await clone.text(), 'xy');
}

export const structuredCloneRoundTrip = {
  async test() {
    const blob = new Blob(['xy'], { type: 'A/B' });
    await checkClone(structuredClone(blob));
    await checkClone(structuredClone(new Blob(['axyz']).slice(1, 3, 'a/b')));
    const empty = structuredClone(new Blob([], { type: 't/x' }));
    strictEqual(empty.size, 0);
    strictEqual(empty.type, 't/x');
    // Nested, and twice: each reference to it reads as the same Blob.
    const [a, b] = structuredClone([blob, blob]);
    strictEqual(a, b);
    await checkClone(a);
  },
};

export const filesDoNotSerialize = {
  test() {
    throws(
      () => structuredClone(new File(['ab'], 'f')),
      dataCloneError('File')
    );
    throws(
      () => structuredClone({ file: new File([], 'f') }),
      dataCloneError('File')
    );
  },
};

export const subclassInstancesCloneAsBlob = {
  async test() {
    class Sub extends Blob {}
    const clone = structuredClone(new Sub(['xy'], { type: 'a/b' }));
    await checkClone(clone);
  },
};

export const posted = {
  async test() {
    const { port1, port2 } = new MessageChannel();
    const { promise, resolve } = Promise.withResolvers();
    port2.onmessage = (event) => resolve(event.data);
    port1.postMessage(new Blob(['xy'], { type: 'a/b' }));
    await checkClone(await promise);
    port1.close();
    port2.close();
  },
};

export const overRpc = {
  async test() {
    await checkClone(await env.RPC.echo(new Blob(['xy'], { type: 'a/b' })));
    await checkClone(await env.RPC.makeBlob());
    strictEqual(
      await env.RPC.describe(new Blob(['xy'], { type: 'a/b' })),
      'Blob,2,a/b,xy'
    );
  },
};
