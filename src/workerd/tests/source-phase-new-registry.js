import { ok } from 'node:assert';

// Test source phase imports for Wasm modules with the new registry
import source wasmSource from 'wasm';

export const wasmSourcePhaseTestNewRegistry = {
  async test() {
    console.log('Starting new registry test');
    ok(wasmSource instanceof WebAssembly.Module);
    // The source object should be a WebAssembly.Module that can be instantiated
    await WebAssembly.instantiate(wasmSource, {});
    console.log('New registry test completed successfully');
  },
};
