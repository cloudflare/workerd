// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { strictEqual, throws } from 'node:assert';
import { DurableObject } from 'cloudflare:workers';
import { retryable } from 'cloudflare:durable-objects';

export const marksMethodsFromStaticBlocks = {
  test() {
    // Static blocks run after the class installs its methods, so one can sit above the method it
    // marks.
    class Counter extends DurableObject {
      static {
        strictEqual(retryable(this.prototype.reset), undefined);
      }
      reset() {}

      static {
        strictEqual(
          retryable(this.prototype.increment, this.prototype.decrement),
          undefined
        );
      }
      increment() {}
      decrement() {}
    }
    strictEqual(retryable(Counter.prototype.reset), undefined);
    strictEqual(retryable(), undefined);
  },
};

export const rejectsNonFunctions = {
  test() {
    const method = function () {};
    for (const value of [undefined, null, 'reset', 1, {}, Symbol('reset')]) {
      throws(() => retryable(value), TypeError);
      throws(() => retryable(method, value), TypeError);
    }
    throws(
      () => {
        class Counter extends DurableObject {
          static {
            retryable(this.prototype.missing);
          }
        }
        return Counter;
      },
      { name: 'TypeError', message: 'retryable() accepts only functions.' }
    );
  },
};
