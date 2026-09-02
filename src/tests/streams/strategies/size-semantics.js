// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// size() semantics. CountQueuingStrategy's size is a constant 1 for any
// input (or none). ByteLengthQueuingStrategy's size diverges in HOW it
// obtains byteLength: the TypeScript implementation performs the spec's
// property read (throws on null/undefined, honors shadowing getters on
// views); C++ uses internal slots for real BufferSources (shadowing
// getters ignored) and returns undefined for null/undefined/non-objects.
// Plain objects carrying a byteLength property are read by BOTH.

import { strictEqual, throws } from 'node:assert';
import { usingTsImpl } from 'which-impl';

const blqsSize = () => new ByteLengthQueuingStrategy({ highWaterMark: 1 }).size;
const cqsSize = () => new CountQueuingStrategy({ highWaterMark: 1 }).size;

export const countSizeIsAlwaysOne = {
  test() {
    const size = cqsSize();
    for (const input of [undefined, null, 'x', 123, {}, new Uint8Array(9)]) {
      strictEqual(size(input), 1);
    }
    strictEqual(size(), 1);
  },
};

export const byteLengthSizeOnBufferSources = {
  test() {
    const size = blqsSize();
    strictEqual(size(new ArrayBuffer(4)), 4);
    strictEqual(size(new Uint8Array(8).subarray(2)), 6);
    strictEqual(size(new DataView(new ArrayBuffer(8), 1, 3)), 3);
    strictEqual(size(new Float64Array(2)), 16);
    // Detached buffers report zero through either mechanism.
    const detached = new ArrayBuffer(4);
    detached.transfer();
    strictEqual(size(detached), 0);
    // Length-tracking view over a resized buffer reports the current size.
    const rab = new ArrayBuffer(4, { maxByteLength: 8 });
    const view = new Uint8Array(rab);
    rab.resize(6);
    strictEqual(size(view), 6);
  },
};

export const byteLengthSizeReadsPlainObjects = {
  test() {
    // Not a BufferSource check: any object's byteLength property counts.
    const size = blqsSize();
    strictEqual(size({ byteLength: 42 }), 42);
    strictEqual(size({}), undefined);
    strictEqual(size(123), undefined);
    strictEqual(size('str'), undefined);
  },
};

export const byteLengthSizeNullishDiverges = {
  test() {
    const size = blqsSize();
    if (usingTsImpl) {
      // The spec's property read throws on nullish input.
      throws(() => size(null), { name: 'TypeError' });
      throws(() => size(undefined), { name: 'TypeError' });
      throws(() => size(), { name: 'TypeError' });
    } else {
      strictEqual(size(null), undefined);
      strictEqual(size(undefined), undefined);
      strictEqual(size(), undefined);
    }
  },
};

export const byteLengthSizeShadowingGetterDiverges = {
  test() {
    // A shadowing byteLength on a REAL view: honored by the spec's
    // property read (TypeScript), ignored by the C++ internal-slot path.
    const size = blqsSize();
    const view = new Uint8Array(4);
    Object.defineProperty(view, 'byteLength', { value: 999 });
    strictEqual(size(view), usingTsImpl ? 999 : 4);
  },
};
