// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { strictEqual } from 'node:assert';

// A fire-and-forget microtask scheduled during entrypoint evaluation must have
// run before the first request is dispatched. This worker has exactly one test
// so that no earlier request's microtask checkpoint can mask a missing flush.
let startupMicrotaskRan = false;
Promise.resolve().then(() => {
  startupMicrotaskRan = true;
});

export const startupMicrotasksFlushed = {
  test() {
    strictEqual(startupMicrotaskRan, true);
  },
};
