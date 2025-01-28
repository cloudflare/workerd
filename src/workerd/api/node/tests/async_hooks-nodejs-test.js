import async_hooks from 'node:async_hooks';
import { throws } from 'node:assert';

export const testErrorMethodNotImplemented = {
  async test() {
    const methods = [
      'createHook',
      'executionAsyncId',
      'executionAsyncResource',
      'triggerAsyncId',
    ];

    for (const method of methods) {
      throws(() => async_hooks[method](), {
        name: 'Error',
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
      });
    }
  },
};
