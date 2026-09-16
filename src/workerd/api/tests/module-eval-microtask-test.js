// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { strictEqual, deepStrictEqual } from 'node:assert';
import { createRequire } from 'node:module';
import process from 'node:process';

const require = createRequire(
  Cloudflare.compatibilityFlags.new_module_registry ? import.meta.url : '/'
);

// Synchronously evaluating a module whose graph has no top-level await must not
// run unrelated pending microtasks. Each case uses a module nothing else in this
// worker has loaded, so the evaluation actually happens here.
export const getBuiltinModuleDoesNotRunMicrotasks = {
  test() {
    const order = [];
    Promise.resolve().then(() => order.push('microtask'));
    const os = process.getBuiltinModule('node:os');
    order.push('sync');
    strictEqual(typeof os.platform, 'function');
    deepStrictEqual(order, ['sync']);
  },
};

export const requireDoesNotRunMicrotasks = {
  test() {
    const order = [];
    Promise.resolve().then(() => order.push('microtask'));
    const cjs = require('sync-cjs');
    order.push('sync');
    strictEqual(cjs.ok, true);
    deepStrictEqual(order, ['sync']);
  },
};

export const requireEsmDoesNotRunMicrotasks = {
  test() {
    const order = [];
    Promise.resolve().then(() => order.push('microtask'));
    const esm = require('sync-esm');
    order.push('sync');
    strictEqual(esm.ok, true);
    deepStrictEqual(order, ['sync']);
  },
};

// A pending top-level await at evaluation depth 0 still gets settled.
export const dynamicImportStillSettlesTopLevelAwait = {
  async test() {
    const mod = await import('tla');
    strictEqual(mod.value, 42);
  },
};
