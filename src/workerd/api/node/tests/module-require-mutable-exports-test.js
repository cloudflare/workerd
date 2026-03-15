// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { createRequire } from 'node:module';
import { strictEqual, notStrictEqual, ok, doesNotThrow } from 'node:assert';

const require = createRequire('/');

export const testTimersPromisesMutable = {
  async test() {
    const timersPromises = require('node:timers/promises');
    const originalSetImmediate = timersPromises.setImmediate;
    ok(typeof originalSetImmediate === 'function');

    const patchedSetImmediate = async function patchedSetImmediate() {
      return 'patched';
    };

    doesNotThrow(() => {
      timersPromises.setImmediate = patchedSetImmediate;
    });

    strictEqual(timersPromises.setImmediate, patchedSetImmediate);
    timersPromises.setImmediate = originalSetImmediate;
    strictEqual(timersPromises.setImmediate, originalSetImmediate);

    // require() should return the same object as import { default }
    const { default: timersPromises2 } = await import('node:timers/promises');
    strictEqual(timersPromises, timersPromises2);

    // But import() returns the namespace object, not the default export
    const timersPromisesNamespace = await import('node:timers/promises');
    notStrictEqual(timersPromises, timersPromisesNamespace);
    strictEqual(timersPromisesNamespace.default, timersPromises);
  },
};

// Test that patching the require()'d object doesn't affect the original named exports
export const testPatchingDoesNotAffectNamedExports = {
  async test() {
    const timersPromises = require('node:timers/promises');
    const originalSetTimeout = timersPromises.setTimeout;

    // Patch the require()'d object
    const patchedSetTimeout = async function patched() {
      return 'patched';
    };
    timersPromises.setTimeout = patchedSetTimeout;

    // The named export from the module namespace should be unaffected
    const { setTimeout: namedSetTimeout } = await import(
      'node:timers/promises'
    );
    notStrictEqual(namedSetTimeout, patchedSetTimeout);
    strictEqual(namedSetTimeout, originalSetTimeout);

    // Restore
    timersPromises.setTimeout = originalSetTimeout;
  },
};

export const testTimersMutable = {
  test() {
    const timers = require('node:timers');
    const originalSetTimeout = timers.setTimeout;
    ok(typeof originalSetTimeout === 'function');

    const patchedSetTimeout = function patchedSetTimeout() {
      return 'patched';
    };

    doesNotThrow(() => {
      timers.setTimeout = patchedSetTimeout;
    });

    strictEqual(timers.setTimeout, patchedSetTimeout);
    timers.setTimeout = originalSetTimeout;
  },
};

export const testBufferMutable = {
  test() {
    const buffer = require('node:buffer');
    const originalBuffer = buffer.Buffer;
    ok(typeof originalBuffer === 'function');

    const patchedBuffer = function PatchedBuffer() {
      return 'patched';
    };

    doesNotThrow(() => {
      buffer.Buffer = patchedBuffer;
    });

    strictEqual(buffer.Buffer, patchedBuffer);
    buffer.Buffer = originalBuffer;
  },
};

export const testUtilMutable = {
  test() {
    const util = require('node:util');
    const originalPromisify = util.promisify;
    ok(typeof originalPromisify === 'function');

    const patchedPromisify = function patchedPromisify() {
      return 'patched';
    };

    doesNotThrow(() => {
      util.promisify = patchedPromisify;
    });

    strictEqual(util.promisify, patchedPromisify);
    util.promisify = originalPromisify;
  },
};

export const testRequireCachesMutableObject = {
  test() {
    const timersPromises1 = require('node:timers/promises');
    const timersPromises2 = require('node:timers/promises');

    strictEqual(timersPromises1, timersPromises2);

    const patchedSetImmediate = async function patched() {
      return 'patched';
    };
    const original = timersPromises1.setImmediate;

    timersPromises1.setImmediate = patchedSetImmediate;
    strictEqual(timersPromises2.setImmediate, patchedSetImmediate);
    timersPromises1.setImmediate = original;
  },
};

// When require_returns_default_export is enabled, require() should return the
// default export directly (which is the object with all the functions),
// not the namespace wrapper with both `default` and named exports.
export const testRequireReturnsDefaultExport = {
  test() {
    const timers = require('node:timers');
    // With require_returns_default_export enabled, timers should be the
    // default export object directly, not the namespace wrapper.
    // The default export IS the object with setTimeout, setInterval, etc.
    ok(typeof timers.setTimeout === 'function');
    ok(typeof timers.setInterval === 'function');
    ok(typeof timers.clearTimeout === 'function');
    ok(typeof timers.clearInterval === 'function');
    // The namespace wrapper would have a 'default' property, but when
    // we return the default export directly, there's no 'default' property
    // on the returned object (unless the default export itself has one).
    strictEqual(timers.default, undefined);
  },
};
