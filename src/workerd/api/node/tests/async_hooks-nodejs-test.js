// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import async_hooks from 'node:async_hooks';
import { ok, deepStrictEqual, strictEqual } from 'node:assert';

export const testErrorMethodNotImplemented = {
  async test() {
    deepStrictEqual(async_hooks.executionAsyncId(), 0);
    deepStrictEqual(async_hooks.triggerAsyncId(), 0);
    deepStrictEqual(async_hooks.executionAsyncResource(), Object.create(null));
    ok(async_hooks.createHook({}));
  },
};

export const asyncLocalStorageOptions = {
  test() {
    const als = new async_hooks.AsyncLocalStorage({
      defaultValue: 1,
      name: 'foo',
    });
    strictEqual(als.name, 'foo');
    strictEqual(als.getStore(), 1);
    strictEqual(
      als.run(2, () => als.getStore()),
      2
    );
    strictEqual(als.getStore(), 1);
  },
};
