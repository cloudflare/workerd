// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { rejects, ok } from 'node:assert';

export const test = {
  async test() {
    // The worker-bundle modules shadowing node:crypto and node:path try to
    // import an internal (node-internal:*) module, which bundle code must never
    // be able to reach. That property holds under both module registries; only
    // the error message differs (legacy: 'No such module ...', new registry:
    // 'Module not found: ...'). See the config for why node:path (and not
    // node:buffer) carries the static-import case.
    await rejects(import('node:crypto'), {
      message: /^(No such module|Module not found)/,
    });
    await rejects(import('node:path'), {
      message: /^(No such module|Module not found)/,
    });
  },
};

import source wasmSource from 'wasm';
export const wasmSourcePhaseTest = {
  async test() {
    ok(wasmSource instanceof WebAssembly.Module);
    await WebAssembly.instantiate(wasmSource, {});
  },
};

export const wasmModuleTest = {
  async test() {
    const { default: wasm } = await import('wasm');
    ok(wasm instanceof WebAssembly.Module);
    await WebAssembly.instantiate(wasm, {});
  },
};

export const dynamicWasmSourcePhaseTest = {
  async test() {
    // The original module registry does not support dynamic source-phase
    // imports; the new module registry does (for Wasm). Accept either the
    // legacy rejection or the new registry's successful compile.
    try {
      const source = await import.source('wasm');
      ok(source instanceof WebAssembly.Module);
      await WebAssembly.instantiate(source, {});
    } catch (err) {
      ok(/Not supported/.test(err.message), `unexpected error: ${err.message}`);
    }
  },
};

export const dynamicSourcePhaseErrorTest = {
  async test() {
    // Source-phase imports of non-Wasm modules fail under both registries;
    // only the message differs (legacy: 'Not supported', new registry:
    // 'Source phase import not available for module: ...').
    await rejects(import.source('worker'), {
      message: /Not supported|Source phase import not available/,
    });
  },
};
