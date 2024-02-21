import {
  AsyncLocalStorage,
} from 'node:async_hooks';

export const customthenable = {
  // Test to ensure that async context is propagated into custom thenables.
  async test() {

    // This should just work
    const als = new AsyncLocalStorage();
    if (!(als instanceof AsyncLocalStorage)) {
      throw new Error('Expected an instance of AsyncLocalStorage');
    }

    // This should not
    try {
      await import('node:assert');
      throw new Error('Expected an error to be thrown');
    } catch (err) {
      if (err.message !== 'No such module "node:assert".') {
        throw err;
      }
    }
  }
};
