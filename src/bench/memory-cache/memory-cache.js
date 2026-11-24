// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// MemoryCache read() hits and misses, with and without a fallback, a fallback that caches a new
// value, and delete(), each with a small and an object value. memory-cache-rust.wd-bench
// runs the same cases against the Rust MemoryCache.

const VALUES = {
  number: 42,
  object: {
    id: 1234,
    name: 'a cached value',
    tags: ['one', 'two', 'three'],
    nested: { a: 1, b: 'two', c: [3, 4, 5] },
  },
};

export default {
  async bench(b, env) {
    // Separate caches, so that the misses and deletes never evict the values that hit.
    const {
      CACHE: cache,
      MISS_CACHE: missCache,
      DELETE_CACHE: deleteCache,
    } = env;
    const fallbackValue = (value) => () => ({ value });
    const never = () => {
      throw new Error('the fallback of a hit ran');
    };

    for (const [type, value] of Object.entries(VALUES)) {
      const key = `hit-${type}`;
      await cache.read(key, fallbackValue(value));

      b.run(`read hit ${type}`, async () => {
        b.blackBox(await cache.read(key));
      });
      b.run(`read hit with fallback ${type}`, async () => {
        b.blackBox(await cache.read(key, never));
      });

      // A new key on every call, so that every read misses and its fallback caches the value.
      let next = 0;
      b.run(`read miss with fallback ${type}`, async () => {
        b.blackBox(
          await missCache.read(`miss-${type}-${next++}`, fallbackValue(value))
        );
      });
    }

    b.run('read miss', async () => {
      b.blackBox(await cache.read('absent'));
    });

    b.run('delete absent', () => {
      cache.delete('absent');
    });

    // Each batch deletes keys that setup cached.
    let batch = 0;
    let deleted = 0;
    b.run(
      'delete present',
      () => {
        deleteCache.delete(`delete-${batch}-${deleted++}`);
      },
      {
        batch: 1024,
        async setup() {
          batch++;
          deleted = 0;
          for (let i = 0; i < 1024; i++) {
            await deleteCache.read(
              `delete-${batch}-${i}`,
              fallbackValue(VALUES.number)
            );
          }
        },
      }
    );
  },
};
