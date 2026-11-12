// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A URL and its searchParams keep each other alive, and the pair is
// collected once neither is reachable. Iterators keep their params alive.

import { strictEqual } from 'node:assert';

// Collects until `done()` holds, or a few times when there is no condition.
// The C++ implementations free a URL and its searchParams over successive
// collections, so one gc() reclaims only half of the pairs; and
// FinalizationRegistry callbacks run in a later task.
async function collect(done = () => false) {
  for (let i = 0; i < 10 && !done(); i++) {
    gc();
    await scheduler.wait(1);
  }
}

export const searchParamsKeepsItsUrlAlive = {
  async test() {
    let params = (() => {
      const url = new URL('http://a/?a=1');
      return url.searchParams;
    })();
    await collect();
    params.append('b', '2');
    strictEqual(params.toString(), 'a=1&b=2');

    let url = (() => new URL('http://a/?a=1'))();
    void url.searchParams;
    await collect();
    url.searchParams.append('b', '2');
    strictEqual(url.href, 'http://a/?a=1&b=2');
    params = url = undefined;
  },
};

export const iteratorKeepsItsParamsAlive = {
  async test() {
    const it = (() => new URL('http://a/?a=1&b=2').searchParams.keys())();
    await collect();
    strictEqual(it.next().value, 'a');
    strictEqual(it.next().value, 'b');
  },
};

export const unreachablePairsAreCollected = {
  async test() {
    let collected = 0;
    const registry = new FinalizationRegistry(() => {
      collected++;
    });
    const kCount = 16;
    (() => {
      for (let i = 0; i < kCount; i++) {
        const url = new URL(`http://a/?i=${i}`);
        const params = url.searchParams;
        params.keys();
        registry.register(url, i);
        registry.register(params, i);
      }
    })();
    await collect(() => collected === 2 * kCount);
    strictEqual(collected, 2 * kCount);
  },
};
