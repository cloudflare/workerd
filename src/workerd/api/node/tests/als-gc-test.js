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

// A store that reaches a Response created inside run() must still be collectable. The Response
// captures the current frame, and the frame holds the store, so this is a cycle through C++.
// See https://github.com/cloudflare/workerd/issues/7851.
export const storeReachingResponseIsCollected = {
  async test() {
    const als = new AsyncLocalStorage();
    const count = 100;
    let finalized = 0;
    const registry = new FinalizationRegistry(() => {
      finalized++;
    });

    for (let i = 0; i < count; i++) {
      const store = {};
      registry.register(store, undefined);
      als.run(store, () => {
        store.res = new Response(null);
      });
    }

    for (let i = 0; i < 10 && finalized < count; i++) {
      gc();
      await scheduler.wait(1);
    }

    if (finalized !== count) {
      throw new Error(`Only ${finalized} of ${count} stores were collected.`);
    }
  },
};
