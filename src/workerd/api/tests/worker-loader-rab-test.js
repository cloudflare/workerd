// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Regression test for AUTOVULN-CLOUDFLARE-WORKERD-70:
// WorkerLoader::extractSource() must copy data/wasm module bytes out of
// V8's BackingStore before going async, because a resizable ArrayBuffer
// can have its committed pages revoked via resize(0) between load()
// returning and the deferred compileDataGlobal() memcpy.

import assert from 'node:assert';

// Minimal valid WASM module that exports an add(i32, i32) -> i32 function.
const WASM_ADD_BYTES = new Uint8Array([
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x07, 0x01, 0x60, 0x02,
  0x7f, 0x7f, 0x01, 0x7f, 0x03, 0x02, 0x01, 0x00, 0x07, 0x07, 0x01, 0x03, 0x61,
  0x64, 0x64, 0x00, 0x00, 0x0a, 0x09, 0x01, 0x07, 0x00, 0x20, 0x00, 0x20, 0x01,
  0x6a, 0x0b,
]);

// Test that a resizable ArrayBuffer used as a data module body can be
// resized to zero after load() without crashing the process. Pre-fix
// this would SIGSEGV in compileDataGlobal(); post-fix the bytes are
// copied eagerly so the child worker compiles and runs normally.
export let resizableArrayBufferDataModule = {
  async test(ctrl, env, ctx) {
    // Create a resizable ArrayBuffer and fill it with known content.
    const rab = new ArrayBuffer(64, { maxByteLength: 128 });
    const view = new Uint8Array(rab);
    const expected = 'Hello from resizable ArrayBuffer!';
    new TextEncoder().encodeInto(expected, view);

    // load() synchronously captures the bytes via jsg::asBytes().
    let worker = env.loader.load({
      compatibilityDate: '2025-01-01',
      mainModule: 'main.js',
      modules: {
        'main.js': {
          js: `
            import {WorkerEntrypoint} from "cloudflare:workers";
            import dataModule from "./data.bin";
            export default class extends WorkerEntrypoint {
              getData() {
                return new TextDecoder().decode(dataModule.slice(0, ${expected.length}));
              }
            }
          `,
        },
        'data.bin': {
          data: rab,
        },
      },
    });

    // Shrink the resizable ArrayBuffer to zero. This mprotect()s the
    // previously-committed pages to PROT_NONE. If extractSource() did
    // not copy, the deferred compilation will SIGSEGV.
    rab.resize(0);

    // Force compilation and exercise the child worker.
    let result = await worker.getEntrypoint().getData();
    assert.strictEqual(result, expected);
  },
};

// Same test but for wasm modules -- the fix must also copy wasm bytes.
export let resizableArrayBufferWasmModule = {
  async test(ctrl, env, ctx) {
    // Copy the WASM bytes into a resizable ArrayBuffer.
    const rab = new ArrayBuffer(WASM_ADD_BYTES.byteLength, {
      maxByteLength: WASM_ADD_BYTES.byteLength * 2,
    });
    new Uint8Array(rab).set(WASM_ADD_BYTES);

    let worker = env.loader.load({
      compatibilityDate: '2025-01-01',
      mainModule: 'main.js',
      modules: {
        'main.js': {
          js: `
            import {WorkerEntrypoint} from "cloudflare:workers";
            import wasmModule from "./math.wasm";
            export default class extends WorkerEntrypoint {
              async add(a, b) {
                const instance = await WebAssembly.instantiate(wasmModule);
                return instance.exports.add(a, b);
              }
            }
          `,
        },
        'math.wasm': {
          wasm: rab,
        },
      },
    });

    // Shrink to zero after load() captured the bytes.
    rab.resize(0);

    // Force compilation -- pre-fix this SIGSEGVs.
    let result = await worker.getEntrypoint().add(3, 4);
    assert.strictEqual(result, 7);
  },
};
