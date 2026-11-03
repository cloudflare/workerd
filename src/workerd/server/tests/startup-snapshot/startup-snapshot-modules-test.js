import { strictEqual } from 'node:assert';
import { EventEmitter } from 'node:events';
import { instance } from './counter.js';
import data from './data.json';

// A dynamic import of a module the main module imports statically resolves to the instance the
// top-level evaluation produced.
export const dynamicImportOfStaticBundleImport = {
  async test() {
    const counter = await import('./counter.js');
    strictEqual(counter.instance, instance);
    strictEqual(globalThis.counterEvaluations, 1);
  },
};

// A module first loaded after the snapshot links its static imports to the modules the top-level
// evaluation produced.
export const staticImportFromLazyModule = {
  async test() {
    const lazy = await import('./lazy.js');
    strictEqual(lazy.counterInstance, instance);
    strictEqual(globalThis.counterEvaluations, 1);
  },
};

// As for bundle modules, for a built-in module.
export const dynamicImportOfStaticBuiltinImport = {
  async test() {
    const events = await import('node:events');
    strictEqual(events.EventEmitter, EventEmitter);
  },
};

// As for bundle modules, for a synthetic module, whose exported value is recorded separately.
export const dynamicImportOfStaticJsonImport = {
  async test() {
    const json = await import('./data.json');
    strictEqual(json.default, data);
  },
};
