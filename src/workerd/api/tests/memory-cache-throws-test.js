// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Whether MemoryCache.read() throws or rejects for an error it detects before it returns a
// promise. Under capture_async_api_throws it rejects; otherwise it throws. memory-cache-test.wd-test
// runs this both with the flag set by the compatibility date and with it enabled explicitly, the
// latter without memory_cache_delete, to check that delete() is exposed only under that flag.
import { match, ok, strictEqual } from 'node:assert';
import { env as importedEnv } from 'cloudflare:workers';

const capturesThrows = Cloudflare.compatibilityFlags.capture_async_api_throws;

// How `fn` fails: `{ thrown }` if it throws, `{ rejected }` if the promise it returns rejects.
async function failureOf(fn) {
  let promise;
  try {
    promise = fn();
  } catch (err) {
    return { thrown: err };
  }
  try {
    await promise;
  } catch (err) {
    return { rejected: err };
  }
  throw new Error('should have failed');
}

// What a read in the global scope, outside any request, throws or rejects with.
const readInGlobalScope = failureOf(() =>
  importedEnv.CACHE.read('global scope')
);

// The error that `failure` carries, checking that it was thrown or rejected as `read()` should.
function readError(failure) {
  if (capturesThrows) {
    strictEqual(failure.thrown, undefined, 'read() threw rather than rejected');
    return failure.rejected;
  }
  strictEqual(failure.rejected, undefined, 'read() rejected rather than threw');
  return failure.thrown;
}

export const readFailsBeforePromise = {
  async test(ctrl, env) {
    const notString = "parameter 1 is not of type 'string'.";

    let err = readError(await failureOf(() => env.CACHE.read(123)));
    strictEqual(err.name, 'TypeError');
    strictEqual(
      err.message,
      `Failed to execute 'read' on 'MemoryCache': ${notString}`
    );

    err = readError(
      await failureOf(() => env.CACHE.read('key', 'not a function'))
    );
    strictEqual(err.name, 'TypeError');
    strictEqual(
      err.message,
      "Failed to execute 'read' on 'MemoryCache': parameter 2 is not of type 'function'."
    );

    err = readError(await readInGlobalScope);
    ok(err instanceof Error);
    match(err.message, /global scope/);
  },
};

export const readRejectsKeyTooLarge = {
  async test(ctrl, env) {
    // A key that is too large rejects, whatever the flag.
    const failure = await failureOf(() => env.CACHE.read('a'.repeat(2049)));
    strictEqual(failure.thrown, undefined);
    strictEqual(failure.rejected.name, 'RangeError');
    strictEqual(failure.rejected.message, 'Key too large.');
  },
};

export const deleteExposedPerFlag = {
  test(ctrl, env) {
    const hasDelete = Cloudflare.compatibilityFlags.memory_cache_delete;
    strictEqual('delete' in env.CACHE, hasDelete);
    strictEqual(typeof env.CACHE.delete, hasDelete ? 'function' : 'undefined');
  },
};

export const deleteThrows = {
  async test(ctrl, env) {
    if (!Cloudflare.compatibilityFlags.memory_cache_delete) return;
    // delete() returns nothing, so it throws, whatever the flag.
    const failure = await failureOf(() => env.CACHE.delete(123));
    strictEqual(failure.thrown.name, 'TypeError');
    strictEqual(
      failure.thrown.message,
      "Failed to execute 'delete' on 'MemoryCache': parameter 1 is not of type 'string'."
    );
  },
};
