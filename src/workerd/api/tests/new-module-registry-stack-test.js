// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { ok, strictEqual, rejects } from 'node:assert';
import { boom as esmBoom } from 'thrower';
import cjs from 'cjs-thrower';

// Stack frames identify modules by their canonical URLs -- the same URLs
// reported by import.meta.url -- so tooling (e.g. source-map consumers) can
// map frames back to the bundled sources. workerd itself does not apply
// source maps; stable, canonical frame URLs are the contract it provides.

function frameOf(fn) {
  try {
    fn();
    throw new Error('expected fn to throw');
  } catch (e) {
    return e;
  }
}

export const stackTraces = {
  async test() {
    // An error thrown inside an ESM bundle module carries that module's URL
    // and the (fixed) line number of the throw site.
    const esmErr = frameOf(esmBoom);
    strictEqual(esmErr.message, 'esm-boom');
    ok(
      esmErr.stack.includes('at boom (file:///bundle/thrower:2:'),
      `unexpected esm stack: ${esmErr.stack}`
    );

    // Same for a CommonJS bundle module: the CJS eval function is compiled
    // with the module's canonical URL as its script origin, keeping CJS and
    // ESM frames consistent.
    const cjsErr = frameOf(cjs.boom);
    strictEqual(cjsErr.message, 'cjs-boom');
    ok(
      cjsErr.stack.includes('at boom (file:///bundle/cjs-thrower:1:'),
      `unexpected cjs stack: ${cjsErr.stack}`
    );

    // Frames from this (embedded) main module use its canonical URL too, and
    // that URL matches import.meta.url.
    strictEqual(import.meta.url, 'file:///bundle/worker');
    const hereErr = frameOf(() => {
      throw new Error('here');
    });
    ok(
      hereErr.stack.includes('file:///bundle/worker:'),
      `unexpected main-module stack: ${hereErr.stack}`
    );

    // A module that fails during lazy dynamic-import evaluation rejects with
    // a stack pointing into the failing module.
    await rejects(import('lazy-thrower'), (e) => {
      strictEqual(e.message, 'lazy-boom');
      ok(
        e.stack.includes('file:///bundle/lazy-thrower:1:'),
        `unexpected lazy stack: ${e.stack}`
      );
      return true;
    });
  },
};
