import { rejects, ok } from 'node:assert';

export const test = {
  async test() {
    await rejects(import('node:crypto'), {
      message: /^No such module/,
    });
    await rejects(import('node:buffer'), {
      message: /^No such module/,
    });
  },
};

import source wasmSource from 'wasm';
export const wasmSourcePhaseTest = {
  async test() {
    ok(wasmSource instanceof WebAssembly.Module);
    // The source object should be a WebAssembly.Module that can be instantiated
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

// Dynamic source phase only supported for new module registry
export const dynamicWasmSourcePhaseTest = {
  async test() {
    await rejects(import.source('wasm'), {
      message: /Not supported/,
    });
  },
};

// Dynamic source phase only supported for new module registry
export const dynamicSourcePhaseErrorTest = {
  async test() {
    await rejects(import.source('worker'), {
      message: /Not supported/,
    });
  },
};
