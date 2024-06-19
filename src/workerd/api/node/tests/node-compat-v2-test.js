// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//

// Imports of Node.js built-ins should work both with and without
// the 'node:' prefix.
import { default as assert } from 'node:assert';
import { default as assert2 } from 'assert';
const assert3 = (await import('node:assert')).default;
const assert4 = (await import('assert')).default;

assert.strictEqual(assert, assert2);
assert.strictEqual(assert, assert3);
assert.strictEqual(assert, assert4);

export const nodeJsExpectedGlobals = {
  async test() {
    // Expected Node.js globals Buffer, process, and global should be present.
    const { Buffer } = await import('node:buffer');
    assert.strictEqual(Buffer, globalThis.Buffer);

    const { default: process } = await import('node:process');
    assert.strictEqual(process, globalThis.process);

    assert.strictEqual(global, globalThis);
  }
};

export const nodeJsGetBuiltins = {
  async test() {
    // node:* modules in the worker bundle should override the built-in modules...
    const { default: fs } = await import('node:fs');
    const { default: path } = await import('node:path');

    await import ('node:path');

    // But process.getBuiltinModule should always return the built-in module.
    const builtInPath = process.getBuiltinModule('node:path');
    const builtInFs = process.getBuiltinModule('node:fs');

    // These are from the worker bundle....
    assert.strictEqual(fs, 1);
    assert.strictEqual(path, 2);

    // But these are from the built-ins...
    // node:fs is not implemented currently so it should be undefined here.
    assert.strictEqual(builtInFs, undefined);

    // node:path is implemented tho...
    assert.notStrictEqual(path, builtInPath);
    assert.strictEqual(typeof builtInPath, 'object');
    assert.strictEqual(typeof builtInPath.join, 'function');

    // While process.getBuiltinModule(...) in Node.js only returns Node.js
    // built-ins, our impl will return cloudflare: and workerd: built-ins
    // also, for completeness. A key difference, however, is that for non-Node.js
    // built-ins, the return value is the module namespace rather than the default
    // export.

    const socket = await import('cloudflare:sockets');
    assert.strictEqual(process.getBuiltinModule('cloudflare:sockets'), socket);
  }
};

export const nodeJsEventsExports = {
  async test() {
    // Expected node:events exports should be present
    const { EventEmitter, getMaxListeners, usingDomains } = await import('node:events');
    assert.notEqual(getMaxListeners, undefined);
    assert.strictEqual(getMaxListeners, EventEmitter.getMaxListeners);
    assert.notEqual(usingDomains, undefined);
    assert.strictEqual(usingDomains, EventEmitter.usingDomains);
  }
};

export const nodeJsBufferExports = {
  async test() {
    // Expected node:buffer exports should be present
    const { atob, btoa, Blob } = await import('node:buffer');
    assert.notEqual(atob, undefined);
    assert.strictEqual(atob, globalThis.atob);
    assert.notEqual(btoa, undefined);
    assert.strictEqual(btoa, globalThis.btoa);
    assert.notEqual(Blob, undefined);
    assert.strictEqual(Blob, globalThis.Blob);
  }
};
