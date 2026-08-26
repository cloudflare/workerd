// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// How cancel reasons of every TYPE surface on the writable side (extends
// the reason-identity divergence of ledger #8 to non-Error values;
// ledger #19):
// - C++ tunnels the reason through kj::Exception and always surfaces an
//   Error object: strings become the Error's message, undefined becomes
//   the canonical "Stream was cancelled.", standard error types
//   (TypeError, RangeError, ...) are preserved, and a custom subclass's
//   name survives as the surfaced Error's name (via the pinned
//   enhanced_error_serialization flag; without it the name folds into a
//   plain Error's message).
// - TypeScript rejects with the original reason VALUE untouched — the same
//   Error instance, the same string, even undefined itself.
// Both the pending write and the pending close observe the same value.

import { ok, strictEqual } from 'node:assert';
import { usingTsImpl } from 'which-impl';
import { captureRejection } from 'propagation-helpers';

class ExampleError extends Error {
  constructor(message) {
    super(message);
    this.name = 'ExampleError';
  }
}

export const cancelReasonTypeSurfacing = {
  async test() {
    const cases = [
      { reason: new Error('test'), cppString: 'Error: test' },
      { reason: 'test', cppString: 'Error: test' },
      // A string that merely looks like a typed error is not interpreted:
      // it stays a plain Error message.
      { reason: 'jsg.Error: test', cppString: 'Error: jsg.Error: test' },
      {
        reason: new TypeError('Problems!'),
        cppString: 'TypeError: Problems!',
        cppType: TypeError,
      },
      {
        reason: new RangeError('Problems!'),
        cppString: 'RangeError: Problems!',
        cppType: RangeError,
      },
      { reason: undefined, cppString: 'Error: Stream was cancelled.' },
      {
        reason: new ExampleError('foo bar'),
        cppString: 'ExampleError: foo bar',
      },
    ];
    for (const { reason, cppString, cppType } of cases) {
      const its = new IdentityTransformStream();
      const writer = its.writable.getWriter();
      const reader = its.readable.getReader();
      const writePromise = writer.write(new TextEncoder().encode('a'));
      const closePromise = writer.close();
      await reader.cancel(reason);
      for (const promise of [writePromise, closePromise]) {
        const err = await captureRejection(promise);
        if (usingTsImpl) {
          strictEqual(err, reason);
        } else {
          strictEqual(String(err), cppString);
          if (cppType !== undefined) {
            ok(err instanceof cppType);
          }
        }
      }
    }
  },
};
