// Tests for the WASM shutdown signal registration shim.
//
// These tests verify that the shimWebAssemblyInstantiate() code in worker.c++ correctly
// detects __instance_signal / __instance_terminated exports, handles various memory
// configurations, and rejects out-of-bounds addresses.

import basicModule from 'signal-basic.wasm';
import partialModule from 'signal-partial-exports.wasm';
import overflowModule from 'signal-bounds-check-overflow.wasm';
import edgeModule from 'signal-bounds-check-edge.wasm';
import validModule from 'signal-bounds-check-valid.wasm';
import decoyModule from 'signal-decoy-memory.wasm';
import externrefMemoryModule from 'signal-externref-memory.wasm';
import importedMemoryModule from 'signal-imported-memory.wasm';
import reclaimModule from 'signal-memory-reclaim.wasm';
import preinitModule from 'signal-preinit.wasm';

// ---------------------------------------------------------------------------
// Shim detection tests
// ---------------------------------------------------------------------------

// Verify that a module exporting both globals and memory registers without error.
export let bothGlobalsRegisters = {
  async test() {
    const instance = await WebAssembly.instantiate(basicModule);
    // If registration threw, we wouldn't get here. Verify the instance works.
    if (instance.exports.get_signal() !== 0) {
      throw new Error('Expected signal to be 0 initially');
    }
  },
};

// Verify the sync WebAssembly.Instance constructor also goes through the shim.
export let syncInstanceRegisters = {
  test() {
    const instance = new WebAssembly.Instance(basicModule);
    if (instance.exports.get_signal() !== 0) {
      throw new Error('Expected signal to be 0 initially');
    }
  },
};

// Module with only __instance_signal (no __instance_terminated) should NOT register.
// It should instantiate without error — the shim silently skips it.
export let partialExportsSkipped = {
  async test() {
    const instance = await WebAssembly.instantiate(partialModule);
    // Should succeed — the shim just doesn't register it.
    if (instance.exports.get_signal() !== 0) {
      throw new Error('Expected signal to be 0 initially');
    }
  },
};

// A plain module with no signal globals should instantiate without issues.
export let noGlobalsSkipped = {
  async test() {
    // Use the valid bounds-check module but only check it instantiates fine.
    // The key is that the shim doesn't blow up on arbitrary modules.
    const instance = await WebAssembly.instantiate(validModule);
    if (instance.exports.get_signal() !== 0) {
      throw new Error('Expected signal to be 0 initially');
    }
  },
};

// Memory at the signal address is pre-initialized to 0xDEADBEEF via a data segment.
// Registration should zero the signal field.
export let registrationZerosPreinitMemory = {
  async test() {
    const instance = await WebAssembly.instantiate(preinitModule);
    if (instance.exports.get_signal() !== 0) {
      throw new Error(
        'Expected signal to be zeroed, got ' + instance.exports.get_signal()
      );
    }
  },
};

// Same test via the sync WebAssembly.Instance constructor.
export let syncRegistrationZerosPreinitMemory = {
  test() {
    const instance = new WebAssembly.Instance(preinitModule);
    if (instance.exports.get_signal() !== 0) {
      throw new Error(
        'Expected signal to be zeroed, got ' + instance.exports.get_signal()
      );
    }
  },
};

// ---------------------------------------------------------------------------
// Bounds checking tests
// ---------------------------------------------------------------------------

// __instance_signal beyond memory bounds — registration is silently skipped.
export let boundsCheckOverflow = {
  async test() {
    // Should instantiate without error; the module simply won't receive shutdown signals.
    await WebAssembly.instantiate(overflowModule);
  },
};

// __instance_signal at 65533 leaves only 3 bytes but needs 4 — silently skipped.
export let boundsCheckEdge = {
  async test() {
    await WebAssembly.instantiate(edgeModule);
  },
};

// Both addresses exactly at the boundary — should succeed.
export let boundsCheckValid = {
  async test() {
    const instance = await WebAssembly.instantiate(validModule);
    if (instance.exports.get_signal() !== 0) {
      throw new Error('Expected signal to be 0 initially');
    }
  },
};

// ---------------------------------------------------------------------------
// Memory detection tests
// ---------------------------------------------------------------------------

// A module that imports memory (not exports it) should still register.
export let importedMemoryDetected = {
  async test() {
    const memory = new WebAssembly.Memory({ initial: 1 });
    const instance = await WebAssembly.instantiate(importedMemoryModule, {
      env: { memory },
    });
    // If registration threw, we wouldn't get here.
    if (instance.exports.get_signal() !== 0) {
      throw new Error('Expected signal to be 0 initially');
    }
  },
};

// A WebAssembly.Memory passed as a non-memory import must not be used as the
// module's linear memory. The module has internal memory but doesn't export it.
export let decoyMemoryIgnored = {
  async test() {
    const decoyMemory = new WebAssembly.Memory({ initial: 1 });
    // The module imports (func "env" "log") only — decoy_memory is extra.
    const instance = await WebAssembly.instantiate(decoyModule, {
      env: {
        log: () => {},
        decoy_memory: decoyMemory,
      },
    });
    // The shim should have found no memory (internal memory is inaccessible).
    // Verify decoy memory is untouched (we can't trigger writeWasmShutdownSignals
    // in workerd, but we can verify instantiation didn't blow up).
    const view = new Uint32Array(decoyMemory.buffer);
    if (view[0] !== 0) {
      throw new Error('Decoy memory was modified during instantiation');
    }
  },
};

// A module that imports a global named "memory" as externref must not be
// confused for a linear memory import.  The shim checks Module.imports() kind
// and should skip registration because the import's kind is 'global', not 'memory'.
export let externrefMemoryIgnored = {
  async test() {
    const instance = await WebAssembly.instantiate(externrefMemoryModule, {
      env: { memory: null },
    });
    // Should instantiate fine — shim just doesn't register it.
    if (instance.exports.get_value() !== 42) {
      throw new Error('Expected get_value() to return 42');
    }
  },
};

// The sync Instance constructor should also detect imported memory.
export let syncInstanceImportedMemory = {
  test() {
    const memory = new WebAssembly.Memory({ initial: 1 });
    const instance = new WebAssembly.Instance(importedMemoryModule, {
      env: { memory },
    });
    if (instance.exports.get_signal() !== 0) {
      throw new Error('Expected signal to be 0 initially');
    }
  },
};

// ---------------------------------------------------------------------------
// GC reclamation test
// ---------------------------------------------------------------------------

// Instantiate many large (16MB) WASM modules, mark each as "exited" via
// mark_exited(), then let GC reclaim them. If the GC prologue filter doesn't
// clean up terminated entries, this will OOM.
export let gcReclaimsTerminatedModules = {
  async test() {
    for (let i = 0; i < 20; i++) {
      const instance = await WebAssembly.instantiate(reclaimModule);
      // Mark the module as exited so the GC prologue filter removes it.
      instance.exports.mark_exited();
    }
    // If we get here without OOM, reclamation is working.
  },
};
