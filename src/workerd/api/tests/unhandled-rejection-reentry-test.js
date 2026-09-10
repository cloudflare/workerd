// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { strictEqual } from 'node:assert';

export const samePromiseHandledReentry = {
  async test() {
    const { promise: unhandled, resolve: resolveUnhandled } =
      Promise.withResolvers();
    const { promise: handled, resolve: resolveHandled } =
      Promise.withResolvers();
    let handledEvents = 0;
    let reentered = false;

    addEventListener('unhandledrejection', resolveUnhandled, { once: true });
    addEventListener('rejectionhandled', (event) => {
      ++handledEvents;
      if (!reentered) {
        reentered = true;
        event.promise.catch(() => {});
      }
      resolveHandled();
    });

    const target = Promise.reject(new Error('expected'));
    await unhandled;
    target.catch(() => {});
    await handled;
    await new Promise((resolve) => setTimeout(resolve, 10));
    strictEqual(handledEvents, 1);
  },
};
