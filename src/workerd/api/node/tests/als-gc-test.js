// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { AsyncLocalStorage } from 'node:async_hooks';

const store = new AsyncLocalStorage();

export const test = {
  test() {
    const outerValue = crypto.randomUUID();
    // eslint-disable-next-line @typescript-eslint/no-unused-expressions
    ({ [outerValue]: `value doesn't matter here` });
    store.run(outerValue, () => {
      for (let i = 0; i < 1_000; i++) {
        const storeValue = store.getStore();
        if (!storeValue) {
          throw new Error(`Failed on attempt ${i}.`);
        }
        for (let j = 0; j < 1_000; j++) {
          (() => Math.random())();
        }
      }
    });
  },
};
