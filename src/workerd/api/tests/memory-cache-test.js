// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { strictEqual } from 'node:assert';
import unsafe from 'workerd:unsafe';

export const basic = {
  async test(ctrl, env) {
    let fallbackCalled = 0;
    const value = await env.CACHE.read('foo', async (key) => {
      strictEqual(key, 'foo');
      fallbackCalled++;
      return { value: 'bar' };
    });
    const value2 = await env.CACHE.read('foo', async (key) => {
      throw new Error('should not be called');
    });
    strictEqual(value, 'bar');
    strictEqual(value, value2);
    strictEqual(fallbackCalled, 1);
  },
};

export const loneSurrogateKey = {
  async test(ctrl, env) {
    const key = '\ud800';
    const replacement = '\ufffd';
    strictEqual(
      await env.CACHE.read(key, async () => ({ value: 'surrogate value' })),
      'surrogate value'
    );
    strictEqual(
      await env.CACHE.read(replacement, async () => ({
        value: 'replacement value',
      })),
      'replacement value'
    );
    strictEqual(await env.CACHE.read(key), 'surrogate value');
    strictEqual(await env.CACHE.read(replacement), 'replacement value');
    env.CACHE.delete(key);
    strictEqual(await env.CACHE.read(key), undefined);
    strictEqual(await env.CACHE.read(replacement), 'replacement value');

    await env.CACHE.read(key, async () => ({ value: 'surrogate value' }));
    await env.CACHE.read(replacement);
    await env.CACHE.read('eviction trigger', async () => ({
      value: 'trigger value',
    }));
    strictEqual(await env.CACHE.read(key), undefined);
  },
};

export const keysTooLarge = {
  async test(ctrl, env) {
    // Keys that are fewer than 2048 bytes should work.
    for (const key of ['', 'abc', 'a'.repeat(2048), 'ä'.repeat(1024)]) {
      const val = await env.CACHE.read(key, async (k) => {
        strictEqual(k, key);
        return { value: `value for ${k}` };
      });
      strictEqual(val, `value for ${key}`);
    }

    // Keys longer that 2048 bytes should reject.
    for (const key of ['a'.repeat(2049), 'ä'.repeat(1025)]) {
      await env.CACHE.read(key, async (k) => {
        throw new Error('should not be called');
      }).then(
        () => {
          throw new Error('should have rejected');
        },
        (err) => {
          strictEqual(err.message, 'Key too large.');
        }
      );
    }
  },
};

export const valueNotSerializable = {
  async test(ctrl, env) {
    try {
      await env.CACHE.read('foo', (key) => {
        return { value: Symbol('foo') };
      });
      throw new Error('should have failed');
    } catch (err) {
      strictEqual(err.message, 'failed to serialize symbol');
    }

    try {
      await env.CACHE.read('foo', (key) => {
        return { value: () => {} };
      });
      throw new Error('should have failed');
    } catch (err) {
      strictEqual(err.message, 'failed to serialize function');
    }
  },
};

export const evictionsHappen = {
  async test(ctrl, env) {
    // Our cache has a max of two items, let's fill it...
    await env.CACHE2.read('foo', async (key) => {
      return { value: 'foo' };
    });
    await env.CACHE2.read('bar', async (key) => {
      return { value: 'bar' };
    });
    // Nothing should be evicted at this point.
    // Let's make foo our most recently accessed item.
    strictEqual(await env.CACHE2.read('bar'), 'bar');
    strictEqual(await env.CACHE2.read('foo'), 'foo');
    // Now, let's add a third item, baz.
    await env.CACHE2.read('baz', async (key) => {
      return { value: 'baz' };
    });
    // At this point, 'bar' should have been evicted.
    strictEqual(await env.CACHE2.read('bar'), undefined);
  },
};

export const evictionsHappenValueSize = {
  async test(ctrl, env) {
    strictEqual(
      await env.CACHE3.read('foo', async (key) => {
        return { value: 'a'.repeat(495) };
      }),
      'a'.repeat(495)
    );

    strictEqual(await env.CACHE3.read('foo'), 'a'.repeat(495));

    strictEqual(
      await env.CACHE3.read('bar', async (key) => {
        return { value: 'a'.repeat(500) };
      }),
      'a'.repeat(500)
    );

    strictEqual(await env.CACHE3.read('bar'), undefined);

    for (let n = 0; n < 6; n++) {
      await env.CACHE3.read(`${n}`, async (key) => {
        return { value: 'a'.repeat(100) };
      });
      strictEqual(await env.CACHE3.read(`${n}`), 'a'.repeat(100));
    }
    strictEqual(await env.CACHE3.read('bar'), undefined);
  },
};

export const concurrentReads = {
  async test(ctrl, env) {
    const promises = [];
    promises.push(
      env.CACHE.read('qux', () => {
        return { value: 'qux' };
      })
    );
    promises.push(
      env.CACHE.read('qux', () => {
        throw new Error('should not have been called');
      })
    );
    const results = await Promise.allSettled(promises);
    strictEqual(results[0].value, 'qux');
    strictEqual(results[1].value, 'qux');
  },
};

export const delayedFallback = {
  async test(ctrl, env) {
    const foo = await env.CACHE.read('123', async () => {
      await scheduler.wait(100);
      return { value: 123 };
    });
    strictEqual(foo, 123);
  },
};

export const expiredEviction = {
  async test(ctrl, env) {
    const ret = await env.CACHE.read('expires', async () => {
      return { value: 'foo', expiration: Date.now() + 100 };
    });
    strictEqual(ret, 'foo');
    await scheduler.wait(600);
    strictEqual(await env.CACHE.read('expires'), undefined);
  },
};

export const fallbackThrows = {
  async test(ctrl, env) {
    try {
      await env.CACHE.read('foo', () => {
        throw new Error('foo');
      });
      throw new Error('should have thrown');
    } catch (err) {
      strictEqual(err.message, 'foo');
    }

    try {
      await env.CACHE.read('foo', async () => {
        throw new Error('foo');
      });
      throw new Error('should have thrown');
    } catch (err) {
      strictEqual(err.message, 'foo');
    }

    try {
      await env.CACHE.read('foo', () => {
        return Promise.reject(new Error('foo'));
      });
      throw new Error('should have thrown');
    } catch (err) {
      strictEqual(err.message, 'foo');
    }
  },
};

export const fallbackQueueMicrotask = {
  async test(ctrl, env) {
    const ret = await env.CACHE.read('microtask', () => {
      const { promise, resolve } = Promise.withResolvers();
      globalThis.queueMicrotask(() => {
        resolve({ value: 'xyz' });
      });
      return promise;
    });
    strictEqual(ret, 'xyz');
  },
};

export const fallbackChainingOnError = {
  async test(ctrl, env) {
    const promises = [];
    promises.push(
      env.CACHE.read('fallback', () => {
        throw new Error('foo');
      })
    );
    promises.push(
      env.CACHE.read('fallback', () => {
        return { value: 'bar' };
      })
    );
    const results = await Promise.allSettled(promises);
    // The first one failed.
    strictEqual(results[0].reason.message, 'foo');
    // The second one succeeded.
    strictEqual(results[1].value, 'bar');
  },
};

export const fallbackChainingOnErrorManyWaiters = {
  async test(ctrl, env) {
    if (!unsafe.isTestAutogateEnabled()) return;

    const waiters = 64;
    const promises = [];
    for (let i = 0; i < waiters - 1; i++) {
      promises.push(
        env.CACHE.read('manyWaiters', () => {
          throw new Error(`fallback ${i} failed`);
        })
      );
    }
    promises.push(
      env.CACHE.read('manyWaiters', () => {
        return { value: 'last' };
      })
    );

    const results = await Promise.allSettled(promises);
    strictEqual(results.length, waiters);
    for (let i = 0; i < waiters - 1; i++) {
      strictEqual(results[i].status, 'rejected');
      strictEqual(results[i].reason.message, `fallback ${i} failed`);
    }
    strictEqual(results[waiters - 1].status, 'fulfilled');
    strictEqual(results[waiters - 1].value, 'last');
    strictEqual(await env.CACHE.read('manyWaiters'), 'last');
  },
};

export const fallbackChainingAllWaitersFail = {
  async test(ctrl, env) {
    if (!unsafe.isTestAutogateEnabled()) return;

    const waiters = 32;
    const promises = [];
    for (let i = 0; i < waiters; i++) {
      promises.push(
        env.CACHE.read('allFail', () => {
          throw new Error(`nope ${i}`);
        })
      );
    }
    const results = await Promise.allSettled(promises);
    for (let i = 0; i < waiters; i++) {
      strictEqual(results[i].status, 'rejected');
      strictEqual(results[i].reason.message, `nope ${i}`);
    }
    strictEqual(await env.CACHE.read('allFail'), undefined);
    strictEqual(
      await env.CACHE.read('allFail', () => ({ value: 'recovered' })),
      'recovered'
    );
  },
};

export const fallbackNotLocked = {
  async test(ctrl, env) {
    // Test that one long running fallback does not block another one.
    const promises = [];
    promises.push(
      env.CACHE.read('aaa', async () => {
        await scheduler.wait(500);
        return { value: 'aaa' };
      })
    );
    promises.push(
      env.CACHE.read('bbb', async () => {
        return { value: 'bbb' };
      })
    );
    const raced = await Promise.race(promises);
    strictEqual(raced, 'bbb');
  },
};
