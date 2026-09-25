// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Input buffer lifecycle: the bytes the codec consumes are a snapshot
// taken synchronously inside write() in both implementations (the C++
// adapter copies in its write path; the TypeScript pair snapshots in its
// strategy size callback). Mutating, detaching, or resizing the buffer
// after write() returns cannot change what gets compressed — the opposite
// of the encoding streams, whose standard transform reads the buffer when
// it runs.

import { strictEqual, deepStrictEqual } from 'node:assert';
import { pump } from 'round-trip';

const enc = new TextEncoder();
const dec = new TextDecoder();

// Compresses with the given write-time mutation hook, then round-trips
// through a DecompressionStream and returns the decoded text.
async function roundTripWithMutation(view, mutate) {
  const cs = new CompressionStream('gzip');
  const writer = cs.writable.getWriter();
  const writePromise = writer.write(view);
  mutate();
  await writePromise;
  await writer.close();
  const chunks = [];
  for await (const chunk of cs.readable) {
    chunks.push(chunk);
  }
  const restored = await pump(new DecompressionStream('gzip'), chunks);
  return dec.decode(restored);
}

export const mutationAfterWriteIsInvisible = {
  async test() {
    const view = enc.encode('AAAA');
    strictEqual(
      await roundTripWithMutation(view, () => view.set(enc.encode('BBBB'))),
      'AAAA'
    );
  },
};

export const detachAfterWriteStillDelivers = {
  async test() {
    const view = enc.encode('CCCC');
    strictEqual(
      await roundTripWithMutation(view, () => view.buffer.transfer()),
      'CCCC'
    );
  },
};

export const resizableShrinkAfterWriteStillDelivers = {
  async test() {
    const rab = new ArrayBuffer(4, { maxByteLength: 8 });
    const view = new Uint8Array(rab);
    view.set(enc.encode('DDDD'));
    strictEqual(await roundTripWithMutation(view, () => rab.resize(0)), 'DDDD');
  },
};

export const alreadyDetachedChunkIsNoop = {
  async test() {
    // A chunk already detached at write() time contributes zero bytes;
    // the stream still closes into a valid empty member.
    const detached = new ArrayBuffer(4);
    detached.transfer();
    const compressed = await pump(new CompressionStream('gzip'), [detached]);
    const restored = await pump(new DecompressionStream('gzip'), [compressed]);
    strictEqual(restored.byteLength, 0);
  },
};

export const lyingMetadataNeverConsulted = {
  async test() {
    // Buffer metadata comes from internal slots; shadowing own getters are
    // never invoked.
    const view = enc.encode('real');
    for (const key of ['byteLength', 'byteOffset', 'buffer']) {
      Object.defineProperty(view, key, {
        get() {
          throw new Error(`${key} getter must not be called`);
        },
      });
    }
    const compressed = await pump(new CompressionStream('gzip'), [view]);
    const restored = await pump(new DecompressionStream('gzip'), [compressed]);
    deepStrictEqual(dec.decode(restored), 'real');
  },
};
