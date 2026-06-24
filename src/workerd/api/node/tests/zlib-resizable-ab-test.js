// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// Regression test for AUTOVULN-CLOUDFLARE-WORKERD-73:
// SIGSEGV via resizable ArrayBuffer shrunk during JSG
// options unwrap (TOCTOU) in brotli/zlib/zstd sync APIs.
import assert from 'node:assert';
import zlib from 'node:zlib';
import { promisify } from 'node:util';

const brotliCompressAsync = promisify(zlib.brotliCompress);

function makeResizableInput(fillByte) {
  const ab = new ArrayBuffer(1024, { maxByteLength: 1024 });
  const u8 = new Uint8Array(ab);
  u8.fill(fillByte);
  return { ab, u8 };
}

function makeShrinkingOpts(ab) {
  const opts = {};
  Object.defineProperty(opts, 'flush', {
    get() {
      ab.resize(0);
      return 0;
    },
  });
  return opts;
}

export const resizableAbBrotliTest = {
  async test() {
    const { ab, u8 } = makeResizableInput(0x41);
    const opts = makeShrinkingOpts(ab);
    const compressed = await brotliCompressAsync(u8, opts);
    assert(compressed instanceof Buffer);
    assert(compressed.length > 0);
  },
};

export const resizableAbZlibTest = {
  test() {
    const { ab, u8 } = makeResizableInput(0x42);
    const opts = makeShrinkingOpts(ab);
    const compressed = zlib.deflateSync(u8, opts);
    assert(compressed instanceof Buffer);
    assert(compressed.length > 0);
  },
};

export const resizableAbZstdTest = {
  test() {
    const { ab, u8 } = makeResizableInput(0x43);
    const opts = makeShrinkingOpts(ab);
    const compressed = zlib.zstdCompressSync(u8, opts);
    assert(compressed instanceof Buffer);
    assert(compressed.length > 0);
  },
};
