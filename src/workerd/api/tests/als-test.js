import {
  strictEqual,
} from 'node:assert';

import {
  AsyncLocalStorage,
} from 'node:async_hooks';

export const customthenable = {
  // Test to ensure that async context is propagated into custom thenables.
  async test() {
    const als = new AsyncLocalStorage();
    const result = await als.run(123, async () => {
      return await {
        then(done) {
          done(als.getStore());
        }
      };
    });
    strictEqual(result, 123);

    const result2 = await als.run(123, async () => {
      return await new Promise((resolve) => resolve({
        then(done) {
          done(als.getStore());
        }
      }));
    });
    strictEqual(result2, 123);
  }
};
