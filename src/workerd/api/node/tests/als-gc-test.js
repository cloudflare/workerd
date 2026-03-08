import { AsyncLocalStorage } from 'node:async_hooks';

const store = new AsyncLocalStorage();

export const test = {
  test() {
    const outerValue = crypto.randomUUID();
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
