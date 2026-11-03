// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Top-level code that touches what V8 adds to a context only when it is not building a startup
// snapshot: Error.stackTraceLimit, WebAssembly, SharedArrayBuffer and the Iterator helpers behind
// shipped flags (see jsg/snapshot-globals.h). One service per case: MODE selects the case's
// top-level behaviour, and each case reports what its Worker sees at request time. The runner
// asserts the reports. Under the startup-snapshot autogate every service here goes through the
// zygote; without it they are plain Workers and the same assertions hold with snapshotUsed false.

import { deepStrictEqual, ok, strictEqual } from 'node:assert';
import { env } from 'cloudflare:workers';

const MODE = env.MODE;

// Whether this Worker was started from a startup snapshot rather than running its top level
// itself. In the zygote's env a service binding is an `undefined` placeholder (the started Worker
// supplies the value: isDeferredForSnapshot in server/workerd-api.c++), so at module scope SELF
// is undefined exactly when this code runs in the zygote, and the constant is baked into the
// snapshot. Nothing else in
// this test depends on that detail.
const snapshotUsed = typeof env.SELF === 'undefined';

const WASM_EMPTY_MODULE = new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0]);
const topLevel = {};

switch (MODE) {
  case 'runner':
  case 'control':
    topLevel.stackIsString = typeof new Error('probe').stack === 'string';
    break;
  case 'stl-set':
    Error.stackTraceLimit = 50;
    break;
  case 'stl-save-raise-restore': {
    // Effect's import-time probe: raise the limit, look at a stack, put the limit back.
    const saved = Error.stackTraceLimit;
    Error.stackTraceLimit = 2;
    topLevel.probeStackIsString = typeof new Error('probe').stack === 'string';
    Error.stackTraceLimit = saved;
    break;
  }
  case 'stl-delete':
    delete Error.stackTraceLimit;
    break;
  case 'stl-nonconfigurable':
    Object.defineProperty(Error, 'stackTraceLimit', {
      value: 5,
      configurable: false,
    });
    break;
  case 'error-frozen':
    Object.freeze(Error);
    break;
  case 'iter-assign':
    Iterator.concat = () => 1;
    break;
  case 'iter-define':
    Object.defineProperty(Iterator.prototype, 'join', {
      value() {
        return 'polyfill';
      },
      writable: true,
      configurable: true,
    });
    break;
  case 'wasm-probe':
    topLevel.wasmType = typeof WebAssembly;
    break;
  case 'wasm-assign':
    globalThis.WebAssembly = {};
    break;
  case 'wasm-use':
    topLevel.valid = WebAssembly.validate(WASM_EMPTY_MODULE);
    break;
  case 'sab-probe':
    topLevel.sabType = typeof SharedArrayBuffer;
    break;
  default:
    throw new Error(`unknown MODE ${MODE}`);
}

function report() {
  const desc = Object.getOwnPropertyDescriptor(Error, 'stackTraceLimit');
  return {
    mode: MODE,
    snapshotUsed,
    topLevel,
    stackTraceLimit:
      desc === undefined
        ? null
        : {
            value: desc.value,
            writable: desc.writable,
            enumerable: desc.enumerable,
            configurable: desc.configurable,
          },
    errorFrozen: Object.isFrozen(Error),
    types: {
      WebAssembly: typeof WebAssembly,
      SharedArrayBuffer: typeof SharedArrayBuffer,
      concat: typeof Iterator.concat,
      join: typeof Iterator.prototype.join,
    },
    concat: MODE === 'iter-assign' ? Iterator.concat() : undefined,
    join: [1, 2].values().join(),
    wasmValidates:
      typeof WebAssembly.validate === 'function'
        ? WebAssembly.validate(WASM_EMPTY_MODULE)
        : null,
  };
}

const V8_DEFAULT = {
  value: 10,
  writable: true,
  enumerable: true,
  configurable: true,
};
const NATIVE_TYPES = {
  WebAssembly: 'object',
  SharedArrayBuffer: 'function',
  concat: 'function',
  join: 'function',
};

async function fetch() {
  return Response.json(report());
}

async function test(ctrl, env) {
  // The runner went through the same pipeline as the cases: whether it was started from a
  // snapshot says whether the pipeline is on in this variant.
  const pipelineOn = snapshotUsed;

  // Cases whose top level the zygote can run: started from the snapshot when the pipeline is on.
  const transferable = {
    control(r) {
      deepStrictEqual(r.stackTraceLimit, V8_DEFAULT);
      strictEqual(r.topLevel.stackIsString, true);
      deepStrictEqual(r.types, NATIVE_TYPES);
      strictEqual(r.join, '1,2');
      strictEqual(r.wasmValidates, true);
    },
    'stl-set'(r) {
      deepStrictEqual(r.stackTraceLimit, { ...V8_DEFAULT, value: 50 });
    },
    'stl-save-raise-restore'(r) {
      deepStrictEqual(r.stackTraceLimit, V8_DEFAULT);
      strictEqual(r.topLevel.probeStackIsString, true);
    },
    'stl-delete'(r) {
      strictEqual(r.stackTraceLimit, null);
    },
  };
  // Cases whose top level fails the zygote: always started as plain Workers, which run the top
  // level themselves against the real objects.
  const fallback = {
    'stl-nonconfigurable'(r) {
      deepStrictEqual(r.stackTraceLimit, {
        ...V8_DEFAULT,
        value: 5,
        configurable: false,
      });
    },
    'error-frozen'(r) {
      strictEqual(r.errorFrozen, true);
      deepStrictEqual(r.stackTraceLimit, {
        ...V8_DEFAULT,
        writable: false,
        configurable: false,
      });
    },
    'iter-assign'(r) {
      strictEqual(r.concat, 1);
    },
    'iter-define'(r) {
      strictEqual(r.join, 'polyfill');
    },
    'wasm-probe'(r) {
      strictEqual(r.topLevel.wasmType, 'object');
      deepStrictEqual(r.types, NATIVE_TYPES);
    },
    'wasm-assign'(r) {
      strictEqual(r.types.WebAssembly, 'object');
      strictEqual(r.wasmValidates, null);
    },
    'wasm-use'(r) {
      strictEqual(r.topLevel.valid, true);
    },
    'sab-probe'(r) {
      strictEqual(r.topLevel.sabType, 'function');
      deepStrictEqual(r.types, NATIVE_TYPES);
    },
  };

  const run = async (name, expectedSnapshotUsed, check) => {
    const response = await env[name].fetch('http://case/');
    const r = await response.json();
    strictEqual(r.mode, name);
    strictEqual(r.snapshotUsed, expectedSnapshotUsed, `${name}: snapshotUsed`);
    ok(r.errorFrozen === (name === 'error-frozen'), `${name}: Error frozen`);
    check(r);
  };
  for (const [name, check] of Object.entries(transferable)) {
    await run(name, pipelineOn, check);
  }
  for (const [name, check] of Object.entries(fallback)) {
    await run(name, false, check);
  }
}

// Only the runner has a test handler: workerd test runs test() of every service that exports one.
export default MODE === 'runner' ? { fetch, test } : { fetch };
