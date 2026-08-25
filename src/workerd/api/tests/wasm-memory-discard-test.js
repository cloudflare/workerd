// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { strictEqual, throws, ok } from 'node:assert';

// A minimal wasm module with a 2-page memory exported as "mem" and a function
// `run(offset, size)` that executes the `memory.discard` opcode (0xfc 0x12).
import discardModule from 'test.wasm';

const kPage = 65536;

export const test = {
  test() {
    // The JS API is installed on the Memory prototype when the compat flag is on.
    ok(
      typeof WebAssembly.Memory.prototype.discard === 'function',
      'WebAssembly.Memory.prototype.discard should exist'
    );

    const mem = new WebAssembly.Memory({ initial: 2 });
    const view = new Uint8Array(mem.buffer);
    view[0] = 0xff;
    view[kPage] = 0xff;

    // Discard the second page: zeroes it and releases the pages.
    strictEqual(mem.discard(kPage, kPage), undefined);
    strictEqual(new Uint8Array(mem.buffer)[kPage], 0);

    // Unaligned offset/length are rejected.
    throws(() => mem.discard(1, kPage), RangeError);
    throws(() => mem.discard(0, 1), RangeError);
    // Out-of-bounds is rejected.
    throws(() => mem.discard(kPage, 2 * kPage), RangeError);

    // The `memory.discard` opcode itself compiles and runs.
    const instance = new WebAssembly.Instance(discardModule);
    const runMem = instance.exports.mem;
    new Uint8Array(runMem.buffer)[kPage] = 0xab;
    instance.exports.run(kPage, kPage);
    strictEqual(new Uint8Array(runMem.buffer)[kPage], 0);

    // Traps on unaligned/out-of-bounds arguments from within wasm.
    throws(() => instance.exports.run(1, kPage), WebAssembly.RuntimeError);
    throws(() => instance.exports.run(0, 3 * kPage), WebAssembly.RuntimeError);
  },
};
