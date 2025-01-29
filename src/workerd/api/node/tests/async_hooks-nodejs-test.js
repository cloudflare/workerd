import async_hooks from 'node:async_hooks';
import { ok, deepStrictEqual } from 'node:assert';

export const testErrorMethodNotImplemented = {
  async test() {
    deepStrictEqual(async_hooks.executionAsyncId(), 0);
    deepStrictEqual(async_hooks.triggerAsyncId(), 0);
    deepStrictEqual(async_hooks.executionAsyncResource(), Object.create(null));
    ok(async_hooks.createHook({}));
  },
};
